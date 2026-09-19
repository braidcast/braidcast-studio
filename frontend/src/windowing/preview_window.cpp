// bridge.hpp pulls in the CEF headers, whose CefDOMNode declares methods like
// GetNextSibling that <windows.h> would otherwise macro-clobber. Include it (and
// thus CEF) before any Windows header so CEF parses clean.
#include "bridge.hpp"
#include "event_names.hpp"

#include "preview_window.hpp"

#include "settings/GeneralSettings.hpp"
#include "multistream/CanvasRuntime.hpp"
#include "multistream/CanvasStore.hpp"
#include "obs_bootstrap.hpp"
#include "overlay/overlay_sources.hpp"
#include "overlay/overlay_viewport.hpp"
#include "scene/transitions.hpp"
#include "scene/scene_persistence.hpp"
#include "source_render.hpp"

#include <CanvasDefinition.hpp>

#include <obs.h>

#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "log.hpp"

// Item-edit handle bit flags, mirroring the legacy preview so the resize math is
// identical.
#define ITEM_LEFT (1 << 0)
#define ITEM_RIGHT (1 << 1)
#define ITEM_TOP (1 << 2)
#define ITEM_BOTTOM (1 << 3)
#define ITEM_ROT (1 << 4)

namespace {

constexpr float kHandleRadius = 4.0f;     // handle half-size in screen px
constexpr float kHandleSelRadius = 6.0f;  // hit-test radius (kHandleRadius * 1.5)
constexpr float kBoxLineThickness = 2.0f; // selection outline thickness in screen px

// How far the rotation handle's disc centre stands off the item's top edge, in screen
// px: the legacy HANDLE_RADIUS * radius * 1.5 - radius
// (frontend_old/widgets/OBSBasicPreview.cpp:402) at the radii above. The hit-test and
// the draw both measure from this, and the disc is exactly the kHandleSelRadius grab
// zone, so every drawn pixel of the disc grabs the rotation and no bare stem does.
constexpr float kRotHandleDistance = (kHandleRadius * 1.5f - 1.0f) * kHandleSelRadius;

// Two rotations closer than this, modulo a full turn, count as the same angle.
constexpr float kRotSameAngleEpsilon = 0.001f;

// Rotation snapping, from the legacy RotateItem (OBSBasicPreview.cpp:1566-1591). The
// resolved angle is wrapped into [kRotAngleMin, kRotAngleMax), the range the legacy
// pointer angle (atan2 plus a quarter turn) lands in, so the targets span exactly that
// range. Unmodified, the angle is pulled within
// kRotSnapPull of the angle the gesture started at and of every multiple of
// kRotSnapStep, and is free between them. Shift pulls within half a kRotShiftStep of
// every multiple of it, which leaves no angle unpulled, so the item turns in hard
// steps. Ctrl without Shift turns it freely.
constexpr float kRotAngleMin = -90.0f;
constexpr float kRotAngleMax = 270.0f;
constexpr float kRotSnapPull = 5.0f;
constexpr float kRotSnapStep = 45.0f;
constexpr float kRotShiftStep = 15.0f;

// Margin reserved on every side of the surface, in device px, before the canvas is
// fitted into it. Without it a dock whose aspect ratio matches the canvas gets a
// zero-px letterbox, and the selection handles of an item at the canvas edge land
// outside the window with nowhere to draw. Matches the legacy frontend's
// PREVIEW_EDGE_SIZE, which insets the same way for the same reason.
constexpr int kPreviewEdgeSize = 10;

// Zoom notches are geometric, so every step is the same ratio and a step out
// exactly undoes a step in: level L is kZoomMaxAmount^(L/kZoomMaxLevel), putting
// 1:1 at level 0 and the extremes at 8x and 1/8x. Ported from the legacy
// preview's MAX_SCALING_LEVEL / MAX_SCALING_AMOUNT / ZOOM_SENSITIVITY
// (frontend_old/widgets/OBSBasicPreview.hpp:15-17).
constexpr int kZoomMaxLevel = 32;
constexpr float kZoomMaxAmount = 8.0f;

// D3D11 bounds a viewport's origin and extent at +-32768. The scene phase sets a
// viewport spanning the whole scaled canvas, so a large canvas at a large zoom
// would otherwise hand the rasterizer an out-of-range rect (a 7680-wide canvas at
// 8x is 61440 px). Cap the amount rather than the level, so the level the user
// asked for is still what a step out reverses.
constexpr float kMaxViewportExtent = 32767.0f;

enum class ItemHandle : uint32_t {
	None = 0,
	TopLeft = ITEM_TOP | ITEM_LEFT,
	TopCenter = ITEM_TOP,
	TopRight = ITEM_TOP | ITEM_RIGHT,
	CenterLeft = ITEM_LEFT,
	CenterRight = ITEM_RIGHT,
	BottomLeft = ITEM_BOTTOM | ITEM_LEFT,
	BottomCenter = ITEM_BOTTOM,
	BottomRight = ITEM_BOTTOM | ITEM_RIGHT,
	Rot = ITEM_ROT,
};

PreviewManager *g_instance = nullptr;

// The process-wide PreviewOverlays, one atomic per field, written only by
// Preview::LoadOverlays. The values below are not defaults -- GeneralSettings owns those,
// and LoadOverlays has run before any preview exists -- they only keep the fields defined
// until it does. A reader may see one frame that mixes an old field with a new one; each
// field is independent, so that frame is still a valid picture.
struct OverlayFlags {
	std::atomic<PreviewOverflowMode> overflow{PreviewOverflowMode::Hidden};
	std::atomic<bool> overflowInvisible{false};
	std::atomic<bool> safeAreas{false};
	std::atomic<bool> spacingHelpers{false};

	PreviewOverlays Load() const
	{
		return PreviewOverlays(overflow.load(), overflowInvisible.load(), safeAreas.load(),
				       spacingHelpers.load());
	}

	void Store(const PreviewOverlays &o)
	{
		overflow.store(o.overflow);
		overflowInvisible.store(o.overflowInvisible);
		safeAreas.store(o.safeAreas);
		spacingHelpers.store(o.spacingHelpers);
	}
};

OverlayFlags g_overlays;

struct OverflowModeEntry {
	const char *token;
	PreviewOverflowMode mode;
};

constexpr OverflowModeEntry kOverflowModes[] = {
	{"hidden", PreviewOverflowMode::Hidden},
	{"selection", PreviewOverflowMode::Selection},
	{"always", PreviewOverflowMode::Always},
};

constexpr bool IsKnownOverflowToken(std::string_view token)
{
	for (const OverflowModeEntry &e : kOverflowModes) {
		if (token == e.token) {
			return true;
		}
	}
	return false;
}

// LoadOverlays falls back to the General settings default, which leaves it nothing to fall
// back to unless that default is one of the tokens above.
static_assert(IsKnownOverflowToken(kDefaultPreviewOverflow), "the default overflow mode is not a known token");

// The Default canvas has no obs_canvas_t mix (it uses the global pipeline). The
// manager keys its Default surface under the empty string; a caller may also
// address it by the Default canvas's own uuid, which normalizes to "" here.
bool IsDefaultCanvasUuid(const std::string &uuid)
{
	return ObsBootstrap::Canvases().IsDefaultUuid(uuid);
}

// The letterbox transform the draw callback computed last frame, in HWND device
// px. Mouse client px -> canvas: (px - drawOrigin) / scale.
struct PreviewTransform {
	float scale = 0.0f;
	int drawX = 0;
	int drawY = 0;
	float baseCX = 0.0f;
	float baseCY = 0.0f;
	// The surface size the row above was computed against. The UI thread needs it to
	// bound a pan and to anchor a zoom against the same frame the pointer was over,
	// and the draw callback is the only place that knows it.
	int surfaceCX = 0;
	int surfaceCY = 0;
};

// What the UI thread asks the next frame to show. Fit mode is the default and
// reproduces the margin-inset letterbox exactly; fixed mode pins the scale to a
// zoom level and offsets the centered canvas by a pan. It sits beside
// PreviewTransform under the same mutex because the pair is one conversation: this
// is the input the draw callback reads, PreviewTransform is the result it
// publishes back for hit-testing.
struct PreviewView {
	bool fixed = false;
	int zoomLevel = 0;
	float scrollX = 0.0f; // device px, added to the centered draw origin
	float scrollY = 0.0f;
	// Blocks editing gestures. Selection, the context menu and the view commands
	// keep working while locked.
	bool locked = false;

	// Back to fit-to-surface. Deliberately leaves `locked` alone -- the lock is a
	// separate choice from the zoom, and a Scale command must not silently unlock
	// the preview. Named so the set of "zoom fields" lives in one place and cannot
	// drift from the callers that reset them.
	void ResetZoom()
	{
		fixed = false;
		zoomLevel = 0;
		scrollX = 0.0f;
		scrollY = 0.0f;
	}
};

// The zoom amount for a level, clamped to the level range.
float ZoomAmountForLevel(int level)
{
	const int clamped = std::clamp(level, -kZoomMaxLevel, kZoomMaxLevel);
	return std::pow(kZoomMaxAmount, float(clamped) / float(kZoomMaxLevel));
}

// The inverse: the level whose amount is nearest `amount`. Used to seed a fixed
// zoom from whatever scale fit mode was last showing, so the first notch steps
// from what is on screen rather than jumping to 1:1.
int ZoomLevelForAmount(float amount)
{
	if (amount <= 0.0f) {
		return 0;
	}
	const float level = std::log(amount) / std::log(kZoomMaxAmount) * float(kZoomMaxLevel);
	return std::clamp(int(std::lround(level)), -kZoomMaxLevel, kZoomMaxLevel);
}

// A level's amount, reduced if it would put the scene phase's viewport past the
// rasterizer's range. Both the draw callback and the zoom command resolve a level
// through here, so they cannot disagree about what a level means.
float FixedZoomScale(int level, float baseCX, float baseCY)
{
	const float amount = ZoomAmountForLevel(level);
	const float maxBase = std::max(baseCX, baseCY);
	return maxBase > 0.0f ? std::min(amount, kMaxViewportExtent / maxBase) : amount;
}

// Bound a pan so the canvas and the surface always overlap. The bound is the
// legacy ClampScrollingOffsets rule
// (frontend_old/widgets/OBSBasicPreview.cpp:2710-2733) restated as one axis pair,
// and what it allows depends on which rect is bigger: a canvas larger than the
// surface may be carried until one of its own edges reaches the surface's center,
// while a canvas smaller than the surface may be carried until its center reaches
// the surface's edge. Either way the two rects still intersect, which is the
// property that matters -- the canvas can never be panned out of sight.
void ClampScroll(PreviewView &view, float scaledCX, float scaledCY, int cx, int cy)
{
	const float boundX = std::max((scaledCX - float(cx)) * 0.5f, 0.0f) + float(cx) * 0.5f;
	const float boundY = std::max((scaledCY - float(cy)) * 0.5f, 0.0f) + float(cy) * 0.5f;
	view.scrollX = std::clamp(view.scrollX, -boundX, boundX);
	view.scrollY = std::clamp(view.scrollY, -boundY, boundY);
}

// The fit-mode scale: the canvas fitted into the surface less the edge margin on
// each axis. Extracted because the draw callback is no longer its only caller --
// the view getter has to report what the NEXT frame will show, and a second copy
// of this is exactly how the menu would come to disagree with the picture.
//
// The caller keeps centering against the full surface, so the leftover extent is
// the margin, split evenly, and the canvas lands inset by kPreviewEdgeSize on each
// side of the limiting axis. A surface too narrow to hold two margins drops the
// margin on that axis rather than shrinking into it: subtracting unconditionally
// would fit the canvas into a sliver and rasterize nothing, turning a merely
// cramped dock black. Falling back to the full extent is what this drew before the
// margin existed. Either branch is <= cx/cy, so the scaled canvas is never larger
// than the surface and the centered draw origin stays non-negative.
float FitScale(int cx, int cy, float baseCX, float baseCY)
{
	const int availCX = cx > kPreviewEdgeSize * 2 ? cx - kPreviewEdgeSize * 2 : cx;
	const int availCY = cy > kPreviewEdgeSize * 2 ? cy - kPreviewEdgeSize * 2 : cy;
	return (float(availCX) / baseCX < float(availCY) / baseCY) ? float(availCX) / baseCX : float(availCY) / baseCY;
}

// The scale the next frame will draw at, derived from the view rather than read
// back off the last one, at the last surface size the draw callback reported.
//
// That distinction is the whole reason this exists. PreviewTransform.scale is
// written by the render thread a frame after the UI thread changes the view, so
// anything on the UI thread reading it back gets the scale from BEFORE the command
// it just applied. Two wheel notches inside one frame would then pair a fresh zoom
// level with a stale scale and throw the zoom anchor, and the view getter would
// answer a menu with the percentage the preview is leaving rather than the one it
// is arriving at.
float PendingScale(const PreviewView &view, const PreviewTransform &t)
{
	if (t.baseCX <= 0.0f || t.baseCY <= 0.0f) {
		return t.scale;
	}
	return view.fixed ? FixedZoomScale(view.zoomLevel, t.baseCX, t.baseCY)
			  : FitScale(t.surfaceCX, t.surfaceCY, t.baseCX, t.baseCY);
}

// Whether the pan modifier is down right now. The overlay is a WS_CHILD sibling of
// the CEF browser HWND and never takes the keyboard focus (nothing in the frontend
// calls SetFocus, and showing it passes SWP_NOACTIVATE), so WM_KEYDOWN never
// arrives here and the key has to be sampled inside a mouse message instead --
// the same way this file already reads Ctrl/Shift/Alt for snapping and crop.
bool PanModifierHeld()
{
	return GetKeyState(VK_SPACE) < 0;
}

// A scene-item key paired with the uuid of the scene it was resolved in. Item ids
// are unique only within one scene and restart at 1 in the next, so a key kept
// across a scene switch names an unrelated item in the new scene; Resolve()
// answers nothing for any scene other than the one Set() recorded. Each key carries
// its own uuid because selection and hover are written at different moments and a
// switch can land between them.
struct SceneItemRef {
	SceneItemKey key;
	std::string sceneUuid;

	// `sceneSource` is the scene `newKey` was resolved in; ignored for a negative id.
	void Set(obs_source_t *sceneSource, const SceneItemKey &newKey)
	{
		key = newKey;
		const char *uuid = (key.id >= 0 && sceneSource) ? obs_source_get_uuid(sceneSource) : nullptr;
		sceneUuid = uuid ? uuid : std::string();
	}
	void Clear() { Set(nullptr, SceneItemKey()); }
	bool Empty() const { return key.id < 0; }
	std::optional<SceneItemKey> Resolve(const char *uuid) const
	{
		if (key.id < 0 || !uuid || sceneUuid != uuid) {
			return std::nullopt;
		}
		return key;
	}
	// The item this names in `scene`, whose uuid is `uuid`, or null.
	obs_sceneitem_t *Find(obs_scene_t *scene, const char *uuid) const
	{
		const std::optional<SceneItemKey> resolved = Resolve(uuid);
		return resolved ? SceneItems::FindItem(scene, *resolved) : nullptr;
	}
};

bool ContainsKey(const std::vector<SceneItemKey> &keys, const SceneItemKey &key)
{
	return std::find(keys.begin(), keys.end(), key) != keys.end();
}

// A multi-item selection. This IS the collection form of SceneItemRef, with the
// scene uuid hoisted out of the elements rather than repeated in each: a selection
// cannot span scenes (the docks' own model clears on a scene change for the same
// reason), so one uuid for the whole set both removes the redundancy and makes that
// invariant structural. Resolve() is the scene-checked reader: it answers nothing for
// any scene other than the one Set recorded, exactly like SceneItemRef::Resolve, and it
// is what every key bound for libobs, the bridge or a gesture goes through. `keys` is
// read directly only where the caller has already established which scene it is holding.
// Members may be top-level items or children of any of the scene's groups.
//
// Insertion-ordered, and the LAST member is the focus: what the single-id readers
// (the bridge reply, the isolation self-test, a right-click that lands outside the
// set) report, mirroring `sourceSelection.item` on the web side.
struct SceneItemSelection {
	std::vector<SceneItemKey> keys;
	std::string sceneUuid;

	bool Empty() const { return keys.empty(); }
	size_t Size() const { return keys.size(); }
	SceneItemKey Anchor() const { return keys.empty() ? SceneItemKey() : keys.back(); }

	void Clear()
	{
		keys.clear();
		sceneUuid.clear();
	}

	// Replace the whole set. `sceneSource` is the scene the keys were resolved in.
	void Set(obs_source_t *sceneSource, const std::vector<SceneItemKey> &newKeys)
	{
		keys = newKeys;
		const char *uuid = (!keys.empty() && sceneSource) ? obs_source_get_uuid(sceneSource) : nullptr;
		sceneUuid = uuid ? uuid : std::string();
	}

	void SetOne(obs_source_t *sceneSource, const SceneItemKey &key)
	{
		if (key.id < 0) {
			Clear();
			return;
		}
		Set(sceneSource, std::vector<SceneItemKey>{key});
	}

	// Ctrl-click: add or remove, with the focus following the row just touched --
	// the newly added one, or (when the focus itself was removed) the last member
	// left. Matches SourceSelection::toggle so the two models cannot drift.
	void Toggle(obs_source_t *sceneSource, const SceneItemKey &key)
	{
		if (key.id < 0) {
			return;
		}
		// A toggle against a set recorded in another scene is a fresh selection:
		// the old keys name items that are not on screen.
		const char *uuid = sceneSource ? obs_source_get_uuid(sceneSource) : nullptr;
		if (!uuid || sceneUuid != uuid) {
			SetOne(sceneSource, key);
			return;
		}
		auto it = std::find(keys.begin(), keys.end(), key);
		if (it != keys.end()) {
			keys.erase(it);
			if (keys.empty()) {
				sceneUuid.clear();
			}
		} else {
			keys.push_back(key);
		}
	}

	// The selected keys, but only for the scene they were recorded in; any other
	// scene gets an empty set. The single gate every reader passes through.
	std::vector<SceneItemKey> Resolve(const char *uuid) const
	{
		if (keys.empty() || !uuid || sceneUuid != uuid) {
			return {};
		}
		return keys;
	}
};

// Per-drag state, captured when the gesture begins on the UI thread and only touched
// on the UI thread, so a drag is atomic. We store the id (re-resolved each message)
// and the box-transform-derived matrices, never an obs_sceneitem_t*. The id is
// scene-scoped like the selection and hover ids: re-resolving a bare id would let
// a scene switch mid-gesture land the drag's writes -- and the save that follows
// them -- on the new scene's item of the same id.
enum class DragMode { None, Move, Resize, Rotate };

// One member of a gesture. A move drags the whole selection, so DragState holds a
// vector of these; a resize or a rotation holds exactly one, which keeps the undo
// capture below the same shape for all of them.
struct DragItem {
	SceneItemRef id;
	// The item's pos when the gesture began, in the space that pos is written in: its
	// group's for a child, the canvas's for a top-level item.
	vec2 startItemPos = {};
	// The map from that space onto the canvas -- the group's, or the identity at top level.
	// SceneItems::CanvasToOwnerVector against it carries the gesture's canvas offset back
	// into it. Read once at the press: the gesture holds every group's re-fit and never writes
	// the group's own transform itself. A write from elsewhere between two mouse messages (an
	// undo, the dock, a bridge call) is not tracked, as the top-level start position is not.
	matrix4 ownerToCanvas = {};
};

// How a crop drag turns box-edge travel into source px, captured at the press by
// CaptureCropFrame. Per item axis: own-space px per source px -- the item's own space, which
// is its group's for a child and the canvas's at top level -- the gap between each box edge
// and the picture inside it (a bounds letterbox; zero unbounded), and whether the picture
// is mirrored inside the box so that a box edge crops the source's opposite side.
struct CropFrame {
	vec2 scale = {};
	vec2 gapTl = {};
	vec2 gapBr = {};
	bool flipX = false;
	bool flipY = false;
};

struct DragState {
	DragMode mode = DragMode::None;
	// Set on the first mouse-move that reaches a resolvable, unlocked item, BEFORE the
	// geometry math runs -- so it means "this gesture got as far as trying", not "the
	// transform changed". A drag mode can still refuse the frame (CropItem returns
	// untouched for a source reporting zero size). It gates the save, which is
	// idempotent either way; anything that must not fire on a no-op gesture compares the
	// geometry instead.
	bool moved = false;
	// The gesture's anchor: the resize or rotation target, and the member whose scene the whole
	// gesture is re-resolved against. Every `items` entry shares its scene.
	SceneItemRef id;
	// Everything the gesture moves, anchor last. Empty until a gesture begins.
	std::vector<DragItem> items;
	vec2 startCanvasPos = {}; // mouse canvas pos at mousedown
	// The selection's combined extent at gesture start. Cached rather than recomputed
	// per frame so the snap input is the START box translated by the gesture's offset:
	// deriving it from the live items each frame would feed the snap a box its own
	// previous correction had already moved.
	vec3 startBoundsTl = {};
	vec3 startBoundsBr = {};
	bool hasStartBounds = false;
	// The top-level ids the gesture moves, which the snap leaves out of its candidates. A
	// child contributes its GROUP's id instead of its own: the group's box is derived from
	// the child, so snapping the child to its own group's edges would chase itself, and the
	// child's own id can collide with an unrelated top-level item's.
	std::vector<int64_t> snapExcludeIds;
	// Every group the gesture writes into, its re-fit held from the press until the drag ends
	// on ANY path (see FinishDrag). Held for a child's move as well as its resize, because
	// our move is absolute from the start position and libobs's re-fit shifts every sibling
	// mid-drag, which would move the start positions out from under it.
	std::vector<SceneItems::GroupResizeDeferral> groupHolds;
	ItemHandle handle = ItemHandle::None;
	// The anchor's own space, the one its transform is written in: its group's for a child,
	// the canvas's for a top-level item. A resize, rotation or crop runs entirely in that
	// space -- the same obs_sceneitem_set_* calls, the same matrices, whatever the group does
	// -- so the pointer crosses into it once per frame (canvasToOwner), and the resize snap,
	// which measures against the canvas, crosses back (ownerToCanvas). Both are the identity
	// for a top-level anchor, which is what keeps that path byte-identical.
	matrix4 canvasToOwner = {};
	matrix4 ownerToCanvas = {};
	matrix4 itemToScreen = {};
	matrix4 screenToItem = {};
	vec2 stretchItemSize = {};
	obs_sceneitem_crop startCrop = {};
	CropFrame crop = {}; // how the item's picture sat in its box at the press
	// Rotate only: the item's rotation at gesture start (degrees), the box centre it
	// turns about, the pointer's angle about that centre at the press (degrees), and the
	// item's position relative to the centre with the start rotation taken out.
	// `rotateApplied` records that a frame has written an angle other than the start
	// one, so a return to the start angle knows whether to restore.
	float rotateStartAngle = 0.0f;
	vec2 rotateCenter = {};
	float rotatePressAngle = 0.0f;
	vec2 rotateOffset = {};
	bool rotateApplied = false;

	// Every dragged item's geometry at gesture start, as the one opaque batch payload
	// Bridge::CaptureItemTransformStates produces. Empty when no item resolved, and
	// cleared whenever a gesture ends, so it is non-empty only while a gesture with
	// something to reverse is in flight.
	std::string undoBefore;

	void Reset()
	{
		mode = DragMode::None;
		moved = false;
		id.Clear();
		items.clear();
		hasStartBounds = false;
		snapExcludeIds.clear();
		// Ends every hold. FinishDrag swaps them out ahead of this so the gesture's AFTER
		// state is still read under them; every other caller has none left to end.
		groupHolds.clear();
		handle = ItemHandle::None;
		// Back to "the anchor is written in canvas space", so a gesture that does not use
		// the pair (a move, which converts per member instead) cannot inherit the last
		// gesture's group.
		matrix4_identity(&canvasToOwner);
		matrix4_identity(&ownerToCanvas);
		undoBefore.clear();
	}
};

// The rubber-band gesture. Kept apart from DragState for the same reason the pan is:
// every `drag.mode != DragMode::None` test in this file means "an item gesture is in
// flight", and a band moves no item. UI thread, except the three fields the draw
// callback reads under the state mutex.
struct BoxState {
	bool active = false; // a band is being dragged right now
	vec2 start = {};     // canvas-space press point
	vec2 current = {};   // canvas-space pointer
	// The selection as it stood at the PRESS, and the scene it belonged to. Recorded
	// UNCONDITIONALLY on every band press -- one vector copy -- and consulted at the
	// release only if a modifier is held then. That split is the whole point: the user
	// starts sweeping and only afterwards decides to add to, subtract from or invert
	// what was already selected. Gating the snapshot on a press-time modifier instead
	// would make the release-time read decorative, since the only sequences it could
	// honour are the ones already committed to at the press.
	std::vector<SceneItemKey> preSelection;
	std::string preSceneUuid;
};

// --- hit-testing (ported from legacy FindItemAtPos) -------------------------

bool SceneItemHasVideo(obs_sceneitem_t *item)
{
	const uint32_t flags = obs_source_get_output_flags(obs_sceneitem_get_source(item));
	return (flags & OBS_SOURCE_VIDEO) != 0;
}

bool CloseFloat(float a, float b, float epsilon = 0.01f)
{
	return std::fabs(a - b) <= epsilon;
}

struct HitFind {
	vec2 pos;
	obs_sceneitem_t *item;
	// The click-through cycle. `selected` is the set the caller currently holds and
	// `selectBelow` arms the walk; see HitTestItemId for what they do.
	const std::vector<int64_t> *selected = nullptr;
	bool selectBelow = false;
};

// The unit box's corners, in the winding BoxCorners uses. Built once: this runs per item
// per hit test, and per item per frame.
const std::array<vec3, 4> &UnitBoxCorners()
{
	static const std::array<vec3, 4> corners = [] {
		std::array<vec3, 4> c;
		vec3_set(&c[0], 0.0f, 0.0f, 0.0f);
		vec3_set(&c[1], 1.0f, 0.0f, 0.0f);
		vec3_set(&c[2], 1.0f, 1.0f, 0.0f);
		vec3_set(&c[3], 0.0f, 1.0f, 0.0f);
		return c;
	}();
	return corners;
}

// The inverse of an item's box transform, or false for a DEGENERATE one, which every
// reader must skip: a zero-scale item would otherwise swallow clicks across the whole
// canvas or draw a collapsed box. matrix4_inv refuses a near-zero determinant and leaves
// `inverse` unwritten when it does; past that, the unit box's corners must come back
// from a round trip through the inverse. The round trip's error is affine in the unit
// position, so a point inside the box comes back at least as close as the worst corner.
bool InvertBoxTransform(const matrix4 &transform, matrix4 &inverse)
{
	if (!matrix4_inv(&inverse, &transform)) {
		return false;
	}
	for (const vec3 &corner : UnitBoxCorners()) {
		vec3 mapped;
		vec3_transform(&mapped, &corner, &transform);
		vec3 back;
		vec3_transform(&back, &mapped, &inverse);
		if (!CloseFloat(back.x, corner.x) || !CloseFloat(back.y, corner.y)) {
			return false;
		}
	}
	return true;
}

// True when `canvasPos` falls inside `item`'s transformed unit box, carried through
// `groupItem`, the group item drawing it (null for a top-level item). Ported from the legacy
// FindItemAtPos; shared by the click hit-test, the rubber band and the
// is-a-selected-item-here test so the three cannot disagree about what "inside" means.
bool PointInItemBox(obs_sceneitem_t *item, obs_sceneitem_t *groupItem, const vec2 &canvasPos)
{
	matrix4 transform;
	matrix4 inverse;
	if (!SceneItems::ItemBoxThroughGroup(item, groupItem, transform) || !InvertBoxTransform(transform, inverse)) {
		return false;
	}

	vec3 pos3;
	vec3_set(&pos3, canvasPos.x, canvasPos.y, 0.0f);
	vec3 local;
	vec3_transform(&local, &pos3, &inverse);
	return local.x >= 0.0f && local.x <= 1.0f && local.y >= 0.0f && local.y <= 1.0f;
}

// Topmost-wins: obs_scene_enum_items yields bottom-to-top, so the last match
// (overwriting `item`) is the topmost hit.
bool FindItemAtPos(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	HitFind *data = static_cast<HitFind *>(param);

	if (!SceneItemHasVideo(item) || obs_sceneitem_locked(item)) {
		return true;
	}

	// The scene's own enumeration never descends into a group, so every item here is top
	// level and carries no group transform.
	if (PointInItemBox(item, nullptr, data->pos)) {
		// Click-through: on reaching a hit that is ALREADY selected, either stop
		// and keep the hit below it (stepping one down the stack), or -- when this
		// selected item is itself the bottom-most hit -- disarm and carry on
		// climbing, which returns the topmost hit and so wraps the cycle back to
		// the start. Ported from the legacy preview
		// (frontend_old/widgets/OBSBasicPreview.cpp:162-168).
		if (data->selectBelow && data->selected &&
		    std::find(data->selected->begin(), data->selected->end(), obs_sceneitem_get_id(item)) !=
			    data->selected->end()) {
			if (data->item) {
				return false;
			}
			data->selectBelow = false;
		}
		data->item = item;
	}
	return true;
}

// Returns the item id at a canvas-space point, or -1. Caller holds the scene alive
// (the returned id is re-resolved later, never a pointer).
//
// With `selected` non-null the walk cycles DOWN the z-stack instead of always
// answering the topmost hit, so repeated clicks in one spot reach what is buried
// under a full-frame capture. The cycle carries NO state of its own -- no cursor,
// no counter, no last-click position, no timeout: it is a pure function of the
// selection the caller passes in, which is exactly why it needs no reset rule.
// Clicking elsewhere resets it implicitly, because the previously selected item is
// no longer under the cursor. This matches the legacy preview, which reads
// obs_sceneitem_selected for the same purpose; we pass the set instead so
// State::selected stays the one source of truth and the libobs flags stay a
// write-only mirror.
int64_t HitTestItemId(obs_scene_t *scene, const vec2 &canvasPos, const std::vector<int64_t> *selected = nullptr)
{
	HitFind data{canvasPos, nullptr, selected, selected != nullptr};
	obs_scene_enum_items(scene, FindItemAtPos, &data);
	return data.item ? obs_sceneitem_get_id(data.item) : int64_t(-1);
}

// Whether a preview gesture may write to `item`, drawn through `groupItem` (the group item
// holding it, null at top level). THE predicate: the hover cursor, the handle hit-test, the
// press, the move and the handle draw all read this one, so none of them can offer an edit
// another refuses.
//
// It is false for an item that draws no video or is locked; for a child of a LOCKED GROUP,
// because a group's lock covers what it holds; and for a child its group cannot take a
// canvas-space placement through (SceneItems::CanvasPlacementRefusal). That last one is what
// fences a BOUNDED group: every gesture here is driven by a canvas-space pointer, and a
// bounded group rescales its content into its bounds after each child write, so nothing the
// pointer asked for survives the re-fit -- refusing the gesture is the only answer that does
// not corrupt the geometry. Such a child stays selectable and outlined, and the dock and the
// Transform dialog still edit it, because those write its group space directly.
bool ItemTakesGesture(obs_sceneitem_t *item, obs_sceneitem_t *groupItem)
{
	return item && SceneItemHasVideo(item) && !obs_sceneitem_locked(item) &&
	       !(groupItem && obs_sceneitem_locked(groupItem)) &&
	       SceneItems::CanvasPlacementRefusal(item, groupItem) == nullptr;
}

// One selected member resolved for a gesture. Borrowed pointers, for immediate use on the UI
// thread.
struct GestureTarget {
	obs_sceneitem_t *item = nullptr;      // null when the key does not resolve
	obs_sceneitem_t *groupItem = nullptr; // the group item drawing a child, null at top level
	bool editable = false;                // ItemTakesGesture for the pair above
};

GestureTarget ResolveGestureTarget(obs_scene_t *scene, const SceneItemKey &key)
{
	GestureTarget target;
	target.item = SceneItems::FindItem(scene, key);
	if (!target.item) {
		return target;
	}
	target.groupItem = key.IsTopLevel() ? nullptr : SceneItems::GroupItemOf(target.item, scene);
	target.editable = ItemTakesGesture(target.item, target.groupItem);
	return target;
}

// Is any member of `keys` under the point, and draggable? Deliberately NOT the cycling
// hit-test: a press on an item that is already selected has to drag the WHOLE selection, and
// the cycle would answer with whatever sits underneath instead. The legacy preview draws
// the same distinction, testing SelectedAtPos at the press and only running the
// cycling ProcessClick when that says no (frontend_old/widgets/OBSBasicPreview.cpp:638
// and :1630-1632).
bool SelectedItemAtPos(obs_scene_t *scene, const std::vector<SceneItemKey> &keys, const vec2 &canvasPos)
{
	for (const SceneItemKey &key : keys) {
		const GestureTarget target = ResolveGestureTarget(scene, key);
		if (target.editable && PointInItemBox(target.item, target.groupItem, canvasPos)) {
			return true;
		}
	}
	return false;
}

// --- handle hit-testing (ported from legacy FindHandleAtPos, no group descent) ---

vec3 GetTransformedPos(float x, float y, const matrix4 &mat)
{
	vec3 result;
	vec3_set(&result, x, y, 0.0f);
	vec3_transform(&result, &result, &mat);
	return result;
}

// Turn `v` by `radians` about the origin, in the convention matrix4_rotate_aa4f about +z
// produces (clockwise on the y-down canvas). Ported from the legacy preview's RotatePos.
vec2 RotateVec2(const vec2 &v, float radians)
{
	const float c = std::cos(radians);
	const float s = std::sin(radians);
	vec2 out;
	vec2_set(&out, c * v.x - s * v.y, s * v.x + c * v.y);
	return out;
}

// `degrees` restated as the same angle in [lo, lo + 360).
float WrapDegrees(float degrees, float lo)
{
	float d = std::fmod(degrees - lo, 360.0f);
	if (d < 0.0f) {
		d += 360.0f;
	}
	if (d >= 360.0f) {
		d -= 360.0f;
	}
	return d + lo;
}

// The unit-box y of the edge the rotation handle stands off: the item's visual top,
// which is the box's bottom edge once a negative y scale has flipped it. A bounded
// item's box is sized by its bounds rather than its scale, so a flip never reaches it.
// Shared by the hit-test and the draw so the two cannot put the handle on different
// edges.
float RotHandleEdgeY(obs_sceneitem_t *item)
{
	vec2 scale;
	obs_sceneitem_get_scale(item, &scale);
	return (scale.y < 0.0f && obs_sceneitem_get_bounds_type(item) == OBS_BOUNDS_NONE) ? 1.0f : 0.0f;
}

// Where the rotation handle stands off `item`: the unit-box y of the edge it springs from, and
// the CANVAS-space unit vector pointing out of that edge, which is what the stand-off is
// measured along.
//
// The direction is read off the box's own y axis rather than turned by the item's rotation,
// which is the same vector for a top-level item -- the box's y axis IS its scale turned by its
// rotation -- and is also right for a child, whose box carries its group's rotation, scale and
// mirroring that no single angle of the item's own describes.
//
// A box with no y extent has no such axis, so the legacy direction stands in: the item's own
// up, turned by its own rotation, which is what this file used for every item before the box
// axis replaced it. It keeps a zero-height top-level item's handle exactly where it was, and
// any fixed direction will do for a box that draws nothing.
void RotHandleStandoff(obs_sceneitem_t *item, const matrix4 &boxTransform, float &edgeY, vec2 &out)
{
	edgeY = RotHandleEdgeY(item);
	vec2 axis;
	vec2_set(&axis, boxTransform.y.x, boxTransform.y.y);
	const float length = vec2_len(&axis);
	if (length <= 0.0f) {
		const float radians = RAD(obs_sceneitem_get_rot(item));
		vec2_set(&out, std::sin(radians), -std::cos(radians));
		return;
	}
	// (edgeY - 0.5) * 2 is -1 at the v=0 edge and +1 at the v=1 edge: away from the centre.
	vec2_mulf(&out, &axis, (edgeY - 0.5f) * 2.0f / length);
}

// Test the 8 resize handles and the rotation handle of `item` against a canvas-space
// point. `scale` is the letterbox screen-px-per-canvas-unit, which keeps both the grab
// radius and the rotation handle's stand-off a fixed number of screen px. `outDist`
// receives the winning handle's distance, so a caller testing several selected items can
// pick the globally closest rather than the first to match.
ItemHandle FindHandleAtPos(obs_sceneitem_t *item, obs_sceneitem_t *groupItem, const vec2 &canvasPos, float scale,
			   float *outDist = nullptr)
{
	matrix4 transform;
	if (!SceneItems::ItemBoxThroughGroup(item, groupItem, transform)) {
		return ItemHandle::None;
	}
	float rotEdgeY = 0.0f;
	vec2 rotOut;
	RotHandleStandoff(item, transform, rotEdgeY, rotOut);

	vec3 pos3;
	vec3_set(&pos3, canvasPos.x, canvasPos.y, 0.0f);

	const float radius = kHandleSelRadius / scale;
	ItemHandle found = ItemHandle::None;
	float closest = radius;

	struct HandleCoord {
		float x, y;
		ItemHandle handle;
	};
	// The rotation handle is last so a box handle at the same distance wins, as in the
	// legacy test order. Its y and its direction are resolved per item by RotHandleStandoff.
	static const HandleCoord kHandles[] = {
		{0.0f, 0.0f, ItemHandle::TopLeft},      {0.5f, 0.0f, ItemHandle::TopCenter},
		{1.0f, 0.0f, ItemHandle::TopRight},     {0.0f, 0.5f, ItemHandle::CenterLeft},
		{1.0f, 0.5f, ItemHandle::CenterRight},  {0.0f, 1.0f, ItemHandle::BottomLeft},
		{0.5f, 1.0f, ItemHandle::BottomCenter}, {1.0f, 1.0f, ItemHandle::BottomRight},
		{0.5f, 0.0f, ItemHandle::Rot},
	};
	for (const auto &h : kHandles) {
		vec3 handlePos;
		if (h.handle == ItemHandle::Rot) {
			// Out from the edge midpoint along the item's own up direction.
			handlePos = GetTransformedPos(h.x, rotEdgeY, transform);
			handlePos.x += rotOut.x * kRotHandleDistance / scale;
			handlePos.y += rotOut.y * kRotHandleDistance / scale;
		} else {
			handlePos = GetTransformedPos(h.x, h.y, transform);
		}
		const float dist = vec3_dist(&handlePos, &pos3);
		if (dist < radius && dist < closest) {
			closest = dist;
			found = h.handle;
		}
	}
	if (outDist) {
		*outDist = closest;
	}
	return found;
}

// What a press at a canvas-space point would grab.
struct GestureAtPos {
	ItemHandle handle = ItemHandle::None;
	obs_sceneitem_t *item = nullptr;      // the selected item, when handle != None
	obs_sceneitem_t *groupItem = nullptr; // its group, when that item is a child
	int64_t bodyId = -1;                  // topmost top-level item under the point, else -1
};

// A resize or rotation handle of a currently-selected item wins over an item body,
// and the body hit-test is skipped entirely once a handle matches. Shared by
// OnLeftDown and the hover cursor so the cursor cannot advertise a gesture other than
// the one the click starts. `scale` is the letterbox screen-px-per-canvas-unit, so
// the grab zone keeps a fixed kHandleSelRadius screen-px radius at any canvas size.
//
// EVERY selected item offers its handles, not just the anchor, and the globally
// closest wins -- with a multi-selection, only the anchor being grabbable would be
// arbitrary. A resize or rotation still acts on that ONE item, matching the legacy
// preview, which likewise keeps a single stretchItem while the selection may be larger.
// `selected` doubles as the click-through cycle's input for the body hit-test, which
// `cycleBelow` arms. A Ctrl-click passes false: the legacy preview's DoCtrlSelect
// takes selectBelow=false so a modifier click always toggles the TOPMOST hit rather
// than walking the stack (frontend_old/widgets/OBSBasicPreview.cpp:730).
GestureAtPos ResolveGestureAtPos(obs_scene_t *scene, const std::vector<SceneItemKey> &selected, const vec2 &canvasPos,
				 float scale, bool cycleBelow = true)
{
	GestureAtPos gesture;
	if (scale > 0.0f) {
		float closest = std::numeric_limits<float>::infinity();
		for (const SceneItemKey &key : selected) {
			// The same members the draw gives handles to, so nothing undrawn can be grabbed.
			const GestureTarget target = ResolveGestureTarget(scene, key);
			if (!target.editable) {
				continue;
			}
			float dist = closest;
			const ItemHandle handle =
				FindHandleAtPos(target.item, target.groupItem, canvasPos, scale, &dist);
			if (handle != ItemHandle::None && dist < closest) {
				closest = dist;
				gesture.handle = handle;
				gesture.item = target.item;
				gesture.groupItem = target.groupItem;
			}
		}
		if (gesture.handle != ItemHandle::None) {
			return gesture;
		}
	}
	// The body hit-test walks the scene's own items only, so the cycle compares bare ids
	// against top-level items and takes the top-level members alone: a child's id can collide
	// with a top-level item's, and passing it here would cycle past the wrong item.
	const std::vector<int64_t> cycleIds = cycleBelow ? SceneItems::TopLevelIds(selected) : std::vector<int64_t>();
	gesture.bodyId = HitTestItemId(scene, canvasPos, cycleBelow ? &cycleIds : nullptr);
	return gesture;
}

// The directional cursor for a resize handle, ported from the legacy preview's
// UpdateCursor: the handle's edge flags are remapped through the item's rotation
// octant and its negative scales, so the arrow points along the edge the drag
// will actually move rather than along the unrotated one.
const wchar_t *CursorForHandle(obs_sceneitem_t *item, obs_sceneitem_t *groupItem, ItemHandle handle)
{
	uint32_t flags = uint32_t(handle);
	if (flags == 0) {
		return IDC_ARROW;
	}
	if (flags & ITEM_ROT) {
		// Ahead of the remapping below, which reads ITEM_LEFT..ITEM_BOTTOM only and
		// would answer a rotation handle with a resize cursor. The legacy preview shows
		// Qt::OpenHandCursor here and Qt::ClosedHandCursor while turning
		// (OBSBasicPreview.cpp:657-660, :1657). Win32 has no stock open or closed hand,
		// so its one hand cursor stands in for both.
		return IDC_HAND;
	}

	// The remap's model of a box is "scale, then turn": its canvas x axis is (scale.x, 0)
	// turned by `rotationDegrees`, its y axis (0, scale.y) turned by the same.
	//
	// A child draws under its group's turn, scale and mirroring as well as its own, and that
	// composition is NOT the sum of the two rotations: a single-axis reflection conjugates a
	// rotation to its inverse, so a group with an odd number of negative scale axes makes the
	// effective turn groupRot - itemRot. Summing them is off by 2 * itemRot there -- with the
	// item at 30 degrees in a group scaled (-1, 1) the box's x axis lands at 150 degrees on the
	// canvas while the sum says 210, two octants out. So the two linear maps are composed and
	// the model read back off the result instead. `groupItem` is null at top level, which skips
	// the block entirely and leaves the inputs exactly as they were -- including for a bounded
	// item, whose box_scale is the positive bounds and would have lost its flip.
	float rotationDegrees = obs_sceneitem_get_rot(item);
	vec2 scale;
	obs_sceneitem_get_scale(item, &scale);
	if (groupItem) {
		const float radians = RAD(rotationDegrees);
		const float cosR = std::cos(radians);
		const float sinR = std::sin(radians);
		// libobs transforms row vectors, so a turn takes x to (cos, sin) and y to (-sin, cos).
		vec2 axisX;
		vec2_set(&axisX, scale.x * cosR, scale.x * sinR);
		vec2 axisY;
		vec2_set(&axisY, -scale.y * sinR, scale.y * cosR);
		matrix4 groupDraw;
		obs_sceneitem_get_draw_transform(groupItem, &groupDraw);
		// The group's translation is irrelevant to an axis, so only its 2x2 part applies.
		auto through = [&groupDraw](vec2 &v) {
			vec2_set(&v, v.x * groupDraw.x.x + v.y * groupDraw.y.x,
				 v.x * groupDraw.x.y + v.y * groupDraw.y.y);
		};
		through(axisX);
		through(axisY);
		const float lengthX = vec2_len(&axisX);
		if (lengthX > 0.0f) {
			// (scale, rot) and (-scale, rot + 180) describe the same pair of axes, and the
			// tests below read `octant` modulo 4 and modulo 2, so the branch is free. Keep
			// the one whose x sign matches the item's own.
			const float signX = scale.x < 0.0f ? -1.0f : 1.0f;
			rotationDegrees = DEG(std::atan2(signX * axisX.y, signX * axisX.x));
			// det = scale.x * scale.y for a scale-then-turn, so its sign fixes y's.
			const float det = axisX.x * axisY.y - axisX.y * axisY.x;
			scale.x = signX * lengthX;
			scale.y = (det < 0.0f ? -signX : signX) * vec2_len(&axisY);
		}
	}

	// The octant and parity tests below index off a rotation in [0,360).
	const float rotation = WrapDegrees(rotationDegrees, 0.0f);
	const int octant = int(std::round(rotation / 45.0f));

	const bool isCorner = (flags & (flags - 1)) != 0;

	if (scale.x < 0.0f && isCorner) {
		flags ^= ITEM_LEFT | ITEM_RIGHT;
	}
	if (scale.y < 0.0f && isCorner) {
		flags ^= ITEM_TOP | ITEM_BOTTOM;
	}

	if (octant % 4 >= 2) {
		if (isCorner) {
			flags ^= ITEM_TOP | ITEM_BOTTOM;
		} else {
			flags = (flags >> 2) | (flags << 2);
		}
	}

	if (octant % 2 == 1) {
		if (isCorner) {
			flags &= (flags % 3 == 0) ? ~uint32_t(ITEM_TOP | ITEM_BOTTOM)
						  : ~uint32_t(ITEM_LEFT | ITEM_RIGHT);
		} else {
			flags = (flags % 4 == 0) ? flags | flags >> ((flags / 2) - 1)
						 : flags | ((flags >> 2) | (flags << 2));
		}
	}

	if (((flags & ITEM_LEFT) && (flags & ITEM_TOP)) || ((flags & ITEM_RIGHT) && (flags & ITEM_BOTTOM))) {
		return IDC_SIZENWSE;
	}
	if (((flags & ITEM_LEFT) && (flags & ITEM_BOTTOM)) || ((flags & ITEM_RIGHT) && (flags & ITEM_TOP))) {
		return IDC_SIZENESW;
	}
	if (flags & (ITEM_LEFT | ITEM_RIGHT)) {
		return IDC_SIZEWE;
	}
	if (flags & (ITEM_TOP | ITEM_BOTTOM)) {
		return IDC_SIZENS;
	}
	return IDC_ARROW;
}

// The modifier keys as of right now. Polled rather than tracked: the overlay is a
// borderless child that never takes focus, so WM_KEYDOWN never reaches it and the only
// truthful read is the one taken while handling the mouse message itself.
struct Modifiers {
	bool ctrl = false;
	bool shift = false;
	bool alt = false;
	bool Any() const { return ctrl || shift || alt; }
};

// What a scripted gesture holds instead of the keyboard, empty outside one. The smoke
// self-test drives Alt-crop and Ctrl-no-snap through it, because a headless run has no key to
// press and the modifiers are sampled from the keyboard rather than carried in the message.
// UI thread only, and set only for the length of one DragForTest call.
std::optional<Modifiers> g_testModifiers;

Modifiers ReadModifiers()
{
	if (g_testModifiers) {
		return *g_testModifiers;
	}
	Modifiers m;
	m.ctrl = GetKeyState(VK_CONTROL) < 0;
	m.shift = GetKeyState(VK_SHIFT) < 0;
	m.alt = GetKeyState(VK_MENU) < 0;
	return m;
}

// Holds `mods` for the length of one scripted gesture and puts the keyboard back afterwards.
struct ScopedTestModifiers {
	explicit ScopedTestModifiers(const Modifiers &mods) { g_testModifiers = mods; }
	ScopedTestModifiers(const ScopedTestModifiers &) = delete;
	ScopedTestModifiers &operator=(const ScopedTestModifiers &) = delete;
	~ScopedTestModifiers() { g_testModifiers.reset(); }
};

// --- selection bounds + box select ------------------------------------------

// The item box's four canvas-space corners, in winding order so that consecutive
// entries (wrapping at the end) bound one edge. The min/max readers do not care about
// the order; the edge walk in IntersectBox does.
std::array<vec3, 4> BoxCorners(const matrix4 &transform)
{
	return {{
		GetTransformedPos(0.0f, 0.0f, transform),
		GetTransformedPos(1.0f, 0.0f, transform),
		GetTransformedPos(1.0f, 1.0f, transform),
		GetTransformedPos(0.0f, 1.0f, transform),
	}};
}

// The combined canvas-space extent of `keys`, or false when none of them resolve.
// Shared by the two readers that need a selection's extent, but they pass DIFFERENT key
// sets on purpose and so the two boxes coincide only while every selected member is one the
// gesture can move. The drawn box spans the whole selection, because it shows what is
// selected. The move gesture's snap box spans only the members that gesture will actually
// move, because a locked member's overhang, or a child the gesture refuses, would offset every
// mover by an edge nothing is dragging. The render thread is one of the two readers, so each
// member is held while its box is read.
bool SelectionBounds(obs_scene_t *scene, const std::vector<SceneItemKey> &keys, vec3 &tl, vec3 &br)
{
	bool first = true;
	for (const SceneItemKey &key : keys) {
		SceneItems::HeldSceneItem held;
		matrix4 boxTransform;
		if (!SceneItems::AcquireItem(scene, key, held) ||
		    !SceneItems::ItemBoxThroughGroup(held.item, held.group, boxTransform)) {
			continue;
		}
		vec3 itemTl;
		vec3 itemBr;
		SceneItems::BoxExtent(boxTransform, itemTl, itemBr);
		if (first) {
			tl = itemTl;
			br = itemBr;
			first = false;
		} else {
			vec3_min(&tl, &tl, &itemTl);
			vec3_max(&br, &br, &itemBr);
		}
	}
	return !first;
}

// Standard CCW segment-vs-segment tests, ported from the legacy preview's
// CounterClockwise/IntersectLine/IntersectBox (OBSBasicPreview.cpp:1048-1106).
bool CounterClockwise(float x1, float x2, float x3, float y1, float y2, float y3)
{
	return (y3 - y1) * (x2 - x1) > (y2 - y1) * (x3 - x1);
}

bool IntersectLine(float x1, float x2, float x3, float x4, float y1, float y2, float y3, float y4)
{
	const bool a = CounterClockwise(x1, x2, x3, y1, y2, y3);
	const bool b = CounterClockwise(x1, x2, x4, y1, y2, y4);
	const bool c = CounterClockwise(x3, x4, x1, y3, y4, y1);
	const bool d = CounterClockwise(x3, x4, x2, y3, y4, y2);
	return (a != b) && (c != d);
}

// Does any edge of the item box whose corners are `c` cross any edge of the rect
// [x1,x2]x[y1,y2]? Takes the corners rather than the transform because the only caller
// already needs them for its own probes.
bool IntersectBox(const std::array<vec3, 4> &c, float x1, float x2, float y1, float y2)
{
	for (int i = 0; i < 4; i++) {
		const vec3 &a = c[i];
		const vec3 &b = c[(i + 1) % 4];
		if (IntersectLine(x1, x1, a.x, b.x, y1, y2, a.y, b.y) ||
		    IntersectLine(x1, x2, a.x, b.x, y1, y1, a.y, b.y) ||
		    IntersectLine(x2, x2, a.x, b.x, y1, y2, a.y, b.y) ||
		    IntersectLine(x1, x2, a.x, b.x, y2, y2, a.y, b.y)) {
			return true;
		}
	}
	return false;
}

struct BoxFind {
	vec2 corner1;
	vec2 corner2;
	std::vector<int64_t> ids;
};

// Rubber-band membership, ported from the legacy FindItemsInBox
// (OBSBasicPreview.cpp:1120-1205). The test is INTERSECT, not contain: any of the
// item's four corners or its centre inside the rect, any edge crossing any rect
// edge, or the band's own moving corner inside the item (which is what catches an
// item so large it swallows the whole band). Unlike the click hit-test this one
// also skips INVISIBLE items -- a band sweeping empty canvas must not pick up
// sources the user has hidden.
bool FindItemsInBox(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *data = static_cast<BoxFind *>(param);

	if (!SceneItemHasVideo(item) || obs_sceneitem_locked(item) || !obs_sceneitem_visible(item)) {
		return true;
	}

	vec2 lo, hi;
	vec2_min(&lo, &data->corner1, &data->corner2);
	vec2_max(&hi, &data->corner1, &data->corner2);
	const float x1 = lo.x, x2 = hi.x, y1 = lo.y, y2 = hi.y;

	matrix4 transform;
	if (!SceneItems::ItemBoxToCanvas(item, transform)) {
		return true;
	}
	const std::array<vec3, 4> corners = BoxCorners(transform);

	const auto inRect = [&](const vec3 &p) {
		return p.x > x1 && p.x < x2 && p.y > y1 && p.y < y2;
	};
	const auto take = [&]() {
		data->ids.push_back(obs_sceneitem_get_id(item));
	};

	// The band's moving corner inside the item's unit box -- what catches an item so
	// large it swallows the whole band.
	if (PointInItemBox(item, nullptr, data->corner2)) {
		take();
		return true;
	}

	if (inRect(GetTransformedPos(0.5f, 0.5f, transform))) {
		take();
		return true;
	}
	for (const vec3 &p : corners) {
		if (inRect(p)) {
			take();
			return true;
		}
	}

	if (IntersectBox(corners, x1, x2, y1, y2)) {
		take();
	}
	return true;
}

// Every item the band currently covers, bottom-to-top.
std::vector<int64_t> BoxItems(obs_scene_t *scene, const vec2 &corner1, const vec2 &corner2)
{
	BoxFind data{corner1, corner2, {}};
	obs_scene_enum_items(scene, FindItemsInBox, &data);
	return data.ids;
}

// --- resize math (ported from legacy GetItemSize/StretchItem/ClampAspect) ----

vec2 GetItemSize(obs_sceneitem_t *item)
{
	vec2 size;
	if (obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE) {
		obs_sceneitem_get_bounds(item, &size);
	} else {
		obs_source_t *source = obs_sceneitem_get_source(item);
		obs_sceneitem_crop crop;
		vec2 scale;
		obs_sceneitem_get_scale(item, &scale);
		obs_sceneitem_get_crop(item, &crop);
		size.x = fmaxf(float(int(obs_source_get_width(source)) - crop.left - crop.right), 0.0f);
		size.y = fmaxf(float(int(obs_source_get_height(source)) - crop.top - crop.bottom), 0.0f);
		vec2_mul(&size, &size, &scale);
	}
	return size;
}

vec3 CalculateStretchPos(obs_sceneitem_t *item, const vec3 &tl, const vec3 &br)
{
	const uint32_t alignment = obs_sceneitem_get_alignment(item);
	vec3 pos;
	vec3_zero(&pos);

	if (alignment & OBS_ALIGN_LEFT) {
		pos.x = tl.x;
	} else if (alignment & OBS_ALIGN_RIGHT) {
		pos.x = br.x;
	} else {
		pos.x = (br.x - tl.x) * 0.5f + tl.x;
	}

	if (alignment & OBS_ALIGN_TOP) {
		pos.y = tl.y;
	} else if (alignment & OBS_ALIGN_BOTTOM) {
		pos.y = br.y;
	} else {
		pos.y = (br.y - tl.y) * 0.5f + tl.y;
	}
	return pos;
}

void ClampAspect(ItemHandle handle, vec3 &tl, vec3 &br, vec2 &size, const vec2 &baseSize)
{
	const float baseAspect = baseSize.x / baseSize.y;
	const float aspect = size.x / size.y;
	const uint32_t flags = uint32_t(handle);

	if (handle == ItemHandle::TopLeft || handle == ItemHandle::TopRight || handle == ItemHandle::BottomLeft ||
	    handle == ItemHandle::BottomRight) {
		if (aspect < baseAspect) {
			if ((size.y >= 0.0f && size.x >= 0.0f) || (size.y <= 0.0f && size.x <= 0.0f)) {
				size.x = size.y * baseAspect;
			} else {
				size.x = size.y * baseAspect * -1.0f;
			}
		} else {
			if ((size.y >= 0.0f && size.x >= 0.0f) || (size.y <= 0.0f && size.x <= 0.0f)) {
				size.y = size.x / baseAspect;
			} else {
				size.y = size.x / baseAspect * -1.0f;
			}
		}
	} else if (handle == ItemHandle::TopCenter || handle == ItemHandle::BottomCenter) {
		if ((size.y >= 0.0f && size.x >= 0.0f) || (size.y <= 0.0f && size.x <= 0.0f)) {
			size.x = size.y * baseAspect;
		} else {
			size.x = size.y * baseAspect * -1.0f;
		}
	} else if (handle == ItemHandle::CenterLeft || handle == ItemHandle::CenterRight) {
		if ((size.y >= 0.0f && size.x >= 0.0f) || (size.y <= 0.0f && size.x <= 0.0f)) {
			size.y = size.x / baseAspect;
		} else {
			size.y = size.x / baseAspect * -1.0f;
		}
	}

	size.x = std::round(size.x);
	size.y = std::round(size.y);

	if (flags & ITEM_LEFT) {
		tl.x = br.x - size.x;
	} else if (flags & ITEM_RIGHT) {
		br.x = tl.x + size.x;
	}
	if (flags & ITEM_TOP) {
		tl.y = br.y - size.y;
	} else if (flags & ITEM_BOTTOM) {
		br.y = tl.y + size.y;
	}
}

// Defined later in the file's other anonymous-namespace block; same translation
// unit, so declaring it here lets StretchItem's resize-snap reuse it unchanged.
vec3 CanvasSnapOffset(const GeneralSettings &gs, obs_scene_t *scene, const std::vector<int64_t> &draggedIds,
		      const vec3 &tl, const vec3 &br, float baseW, float baseH);

// Seeds the item-local box corners from the drag-start size (tl at the origin, br
// at drag.stretchItemSize) and moves the live-dragged edge(s) to the mouse's
// current item-local position (`ownerPos`, the pointer in the item's own space, mapped
// through drag.screenToItem, the same matrix BeginResize captured at mousedown). Shared by
// StretchItem (scale/bounds resize) and CropItem (Alt-crop) so both drag modes read the
// identical own-space->item-local mapping and the identical live-edge selection.
void DragBoxLocal(const DragState &drag, const vec2 &ownerPos, vec3 &tl, vec3 &br, vec3 &pos3)
{
	const uint32_t flags = uint32_t(drag.handle);

	vec3_zero(&tl);
	vec3_set(&br, drag.stretchItemSize.x, drag.stretchItemSize.y, 0.0f);

	vec3_set(&pos3, ownerPos.x, ownerPos.y, 0.0f);
	vec3_transform(&pos3, &pos3, &drag.screenToItem);

	if (flags & ITEM_LEFT) {
		tl.x = pos3.x;
	} else if (flags & ITEM_RIGHT) {
		br.x = pos3.x;
	}
	if (flags & ITEM_TOP) {
		tl.y = pos3.y;
	} else if (flags & ITEM_BOTTOM) {
		br.y = pos3.y;
	}
}

// Resize the active drag item to the current mouse position, in the item's OWN space (see
// DragState::canvasToOwner): group space for a child, the canvas's for a top-level item.
// Single-select, OBS_BOUNDS_NONE (scale) and bounds paths; aspect is preserved on corner and
// edge drags unless Shift requests free aspect.
// Snaps the moving edge(s) to canvas edges/center/other sources, mirroring
// move-drag's CanvasSnapOffset via a per-live-edge probe box (see below).
void StretchItem(const DragState &drag, obs_sceneitem_t *item, const vec2 &ownerPos, obs_scene_t *scene,
		 const GeneralSettings &gs, float snapBaseW, float snapBaseH, const Modifiers &mods)
{
	const obs_bounds_type boundsType = obs_sceneitem_get_bounds_type(item);
	const uint32_t flags = uint32_t(drag.handle);

	vec3 tl, br, pos3;
	DragBoxLocal(drag, ownerPos, tl, br, pos3);

	// --- resize-snap ---
	// Only one edge per live axis moves; the opposite edge is a fixed anchor.
	// Build a canvas-space probe box where the anchor edge is collapsed onto the
	// moving edge's coordinate, so CanvasSnapOffset's left/right/center checks all
	// evaluate against the true moving edge. Non-live axes pass the real box and
	// discard the returned offset for that axis.
	const bool xLive = (flags & (ITEM_LEFT | ITEM_RIGHT)) != 0;
	const bool yLive = (flags & (ITEM_TOP | ITEM_BOTTOM)) != 0;
	if (gs.snapEnabled && !mods.ctrl && snapBaseW > 0.0f && snapBaseH > 0.0f && (xLive || yLive)) {
		// itemToScreen lands in the item's own space, so a child's box crosses into the
		// canvas through its group before it is measured against canvas edges and against
		// other items' boxes, which are canvas-space too.
		vec3 canvasTl, canvasBr;
		vec3_transform(&canvasTl, &tl, &drag.itemToScreen);
		vec3_transform(&canvasBr, &br, &drag.itemToScreen);
		vec3_transform(&canvasTl, &canvasTl, &drag.ownerToCanvas);
		vec3_transform(&canvasBr, &canvasBr, &drag.ownerToCanvas);

		vec3 probeTl = canvasTl, probeBr = canvasBr;
		if (xLive) {
			if (flags & ITEM_LEFT) {
				probeBr.x = probeTl.x;
			} else {
				probeTl.x = probeBr.x;
			}
		}
		if (yLive) {
			if (flags & ITEM_TOP) {
				probeBr.y = probeTl.y;
			} else {
				probeTl.y = probeBr.y;
			}
		}
		if (probeTl.x > probeBr.x) {
			std::swap(probeTl.x, probeBr.x);
		}
		if (probeTl.y > probeBr.y) {
			std::swap(probeTl.y, probeBr.y);
		}

		// The dragged item excludes itself from source-snapping; for a child that
		// exclusion is its GROUP, whose box the child's own edges define (see
		// DragState::snapExcludeIds).
		vec3 snap = CanvasSnapOffset(gs, scene, drag.snapExcludeIds, probeTl, probeBr, snapBaseW, snapBaseH);

		// Back the other way: canvas -> the item's own space through its group, then
		// own-space -> item-local, which is rotation-only for a delta (itemToScreen has no
		// scale component: local and own space share units, differing by rotation
		// and translation, and translation drops out for a delta).
		vec2 canvasSnap;
		vec2_set(&canvasSnap, snap.x, snap.y);
		// A group with no inverse never starts a gesture (GestureTarget, through
		// CanvasPlacementRefusal -> CanvasToGroup), and that refuses on the determinant of
		// this very matrix by the same rule this call uses, so the false branch is
		// unreachable; leaving the offset zero there snaps nothing rather than applying a
		// canvas-space delta in group space.
		vec2 ownerSnap;
		vec2_zero(&ownerSnap);
		SceneItems::CanvasToOwnerVector(drag.ownerToCanvas, canvasSnap, ownerSnap);
		const vec2 localSnap = RotateVec2(ownerSnap, RAD(-obs_sceneitem_get_rot(item)));

		if (xLive) {
			if (flags & ITEM_LEFT) {
				tl.x += localSnap.x;
			} else {
				br.x += localSnap.x;
			}
		}
		if (yLive) {
			if (flags & ITEM_TOP) {
				tl.y += localSnap.y;
			} else {
				br.y += localSnap.y;
			}
		}
	}
	// --- end resize-snap ---

	obs_source_t *source = obs_sceneitem_get_source(item);
	const uint32_t source_cx = obs_source_get_width(source);
	const uint32_t source_cy = obs_source_get_height(source);
	if (!source_cx || !source_cy) {
		return;
	}

	vec2 baseSize;
	vec2_set(&baseSize, float(source_cx), float(source_cy));
	vec2 size;
	vec2_set(&size, br.x - tl.x, br.y - tl.y);

	if (boundsType != OBS_BOUNDS_NONE) {
		if (tl.x > br.x) {
			std::swap(tl.x, br.x);
		}
		if (tl.y > br.y) {
			std::swap(tl.y, br.y);
		}
		vec2_abs(&size, &size);
		obs_sceneitem_set_bounds(item, &size);
	} else {
		obs_sceneitem_crop crop;
		obs_sceneitem_get_crop(item, &crop);
		baseSize.x -= float(crop.left + crop.right);
		baseSize.y -= float(crop.top + crop.bottom);

		if (!mods.shift) {
			ClampAspect(drag.handle, tl, br, size, baseSize);
		}

		vec2_div(&size, &size, &baseSize);
		obs_sceneitem_set_scale(item, &size);
	}

	pos3 = CalculateStretchPos(item, tl, br);
	vec3_transform(&pos3, &pos3, &drag.itemToScreen);
	vec2 newPos;
	vec2_set(&newPos, std::round(pos3.x), std::round(pos3.y));
	obs_sceneitem_set_pos(item, &newPos);
}

// Crop the active drag item to the current mouse position in the item's own space (Alt-drag;
// see DragState::canvasToOwner, which is what put the pointer there). Adjusts
// obs_sceneitem_crop's per-edge left/right/top/bottom in source px and never writes
// scale or bounds. Never snaps (OBS's crop drag ignores snapping outright), so this
// intentionally skips the CanvasSnapOffset step.
//
// Branches on the bounds type as the legacy CropItem does
// (frontend_old/widgets/OBSBasicPreview.cpp:1352-1469). Unbounded, the box is the
// cropped source times the scale, so the position follows the dragged edge to keep the
// opposite edge planted. Bounded, the box is the bounds and stays where it is while the
// bounds type re-fits the remaining source inside it, so the position is left alone.
// Either way the drag converts to source px through drag.crop; see CaptureCropFrame.
void CropItem(const DragState &drag, obs_sceneitem_t *item, const vec2 &ownerPos)
{
	const bool bounded = obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE;
	const uint32_t flags = uint32_t(drag.handle);

	vec3 tl, br, pos3;
	DragBoxLocal(drag, ownerPos, tl, br, pos3);

	const CropFrame &frame = drag.crop;
	const vec2 &scale = frame.scale;
	if (scale.x == 0.0f || scale.y == 0.0f) {
		return;
	}

	obs_source_t *source = obs_sceneitem_get_source(item);
	const int source_cx = int(obs_source_get_width(source));
	const int source_cy = int(obs_source_get_height(source));
	if (!source_cx || !source_cy) {
		return;
	}

	// A box edge's inward travel, less the gap between that edge and the picture, in source
	// px. Travel that stays inside the gap reaches no picture and crops nothing; outward
	// travel still uncrops.
	const auto sourcePx = [](float travel, float gap, float perPx) {
		const float past = travel > gap ? travel - gap : std::min(travel, 0.0f);
		return int(std::round(past / perPx));
	};

	// On a mirrored axis a box edge shows the source's opposite side, and libobs crops the
	// source before it mirrors, so the crop edge written is the opposite one.
	obs_sceneitem_crop crop = drag.startCrop;
	int &leftEdge = frame.flipX ? crop.right : crop.left;
	int &rightEdge = frame.flipX ? crop.left : crop.right;
	int &topEdge = frame.flipY ? crop.bottom : crop.top;
	int &bottomEdge = frame.flipY ? crop.top : crop.bottom;
	if (flags & ITEM_LEFT) {
		leftEdge += sourcePx(tl.x, frame.gapTl.x, scale.x);
	} else if (flags & ITEM_RIGHT) {
		rightEdge += sourcePx(drag.stretchItemSize.x - br.x, frame.gapBr.x, scale.x);
	}
	if (flags & ITEM_TOP) {
		topEdge += sourcePx(tl.y, frame.gapTl.y, scale.y);
	} else if (flags & ITEM_BOTTOM) {
		bottomEdge += sourcePx(drag.stretchItemSize.y - br.y, frame.gapBr.y, scale.y);
	}

	// Corner handles touch two edges on two different axes (e.g. top-left ->
	// left+top), never two edges of the same axis, so each live edge clamps
	// independently against its own untouched opposite edge. A 1px sliver of
	// source is kept visible so a drag can never crop past the far edge.
	const auto clampEdge = [](int &edge, int opposite, int sourceExtent) {
		edge = std::clamp(edge, 0, std::max(0, sourceExtent - 1 - opposite));
	};
	if (flags & ITEM_LEFT) {
		clampEdge(leftEdge, rightEdge, source_cx);
	} else if (flags & ITEM_RIGHT) {
		clampEdge(rightEdge, leftEdge, source_cx);
	}
	if (flags & ITEM_TOP) {
		clampEdge(topEdge, bottomEdge, source_cy);
	} else if (flags & ITEM_BOTTOM) {
		clampEdge(bottomEdge, topEdge, source_cy);
	}

	obs_sceneitem_set_crop(item, &crop);
	if (bounded) {
		return;
	}

	// Re-derive tl/br from the (possibly clamped) crop so the position update
	// below reflects what was actually applied, not the raw unclamped drag.
	tl.x = float(crop.left - drag.startCrop.left) * scale.x;
	br.x = drag.stretchItemSize.x - float(crop.right - drag.startCrop.right) * scale.x;
	tl.y = float(crop.top - drag.startCrop.top) * scale.y;
	br.y = drag.stretchItemSize.y - float(crop.bottom - drag.startCrop.bottom) * scale.y;

	pos3 = CalculateStretchPos(item, tl, br);
	vec3_transform(&pos3, &pos3, &drag.itemToScreen);
	vec2 newPos;
	vec2_set(&newPos, std::round(pos3.x), std::round(pos3.y));
	obs_sceneitem_set_pos(item, &newPos);
}

// The CropFrame for a crop drag starting now. `screenToItem` and `boxSize` are the
// resize's own-space->box-local matrix and box size, `crop` the crop at the press. "Own
// space" is the space the item's transform is written in: its group's for a child, the
// canvas's at top level.
//
// Unbounded, the box IS the picture: the scale is the item's own, signed, so a mirrored
// item's box-local space mirrors with it and the dragged edge is already the right crop
// edge; box_size = (source_size - crop) * scale (see GetItemSize above), and there is no
// gap.
//
// Bounded, the box is the bounds and the picture is fitted inside it, which libobs
// records in the draw transform whatever the bounds type. Its axis lengths are own-space px
// per source px, taken as magnitudes because a mirror is expressed by which crop edge is
// written, not by the sign of the conversion. Mapping the picture's corners through it
// into box-local space gives the gap on each side and, from which way round the corners
// land, whether the picture is mirrored on that axis.
//
// Read once at the press: the fit changes as the crop does, and re-reading it per frame
// would feed each frame's crop back into the next one's conversion.
CropFrame CaptureCropFrame(obs_sceneitem_t *item, const matrix4 &screenToItem, const vec2 &boxSize,
			   const obs_sceneitem_crop &crop)
{
	CropFrame frame;
	if (obs_sceneitem_get_bounds_type(item) == OBS_BOUNDS_NONE) {
		obs_sceneitem_get_scale(item, &frame.scale);
		return frame;
	}

	matrix4 draw;
	obs_sceneitem_get_draw_transform(item, &draw);
	vec2_set(&frame.scale, std::hypot(draw.x.x, draw.x.y), std::hypot(draw.y.x, draw.y.y));

	// The picture in source px, without the extra crop libobs applies when cropping to
	// bounds, which it does not expose. Whenever the picture overhangs the box, cropped to
	// bounds or drawn past it, the clamp below treats the box edge as the picture's edge, so
	// a drag crops from the first hidden row rather than jumping across the overhang.
	obs_source_t *source = obs_sceneitem_get_source(item);
	const float cx = float(std::max(0, int(obs_source_get_width(source)) - crop.left - crop.right));
	const float cy = float(std::max(0, int(obs_source_get_height(source)) - crop.top - crop.bottom));

	vec3 nearCorner = GetTransformedPos(0.0f, 0.0f, draw);
	vec3 farCorner = GetTransformedPos(cx, cy, draw);
	vec3_transform(&nearCorner, &nearCorner, &screenToItem);
	vec3_transform(&farCorner, &farCorner, &screenToItem);

	frame.flipX = farCorner.x < nearCorner.x;
	frame.flipY = farCorner.y < nearCorner.y;

	const auto gaps = [](float a, float b, float extent, float &gapLo, float &gapHi) {
		gapLo = std::clamp(std::min(a, b), 0.0f, extent);
		gapHi = extent - std::clamp(std::max(a, b), 0.0f, extent);
	};
	gaps(nearCorner.x, farCorner.x, boxSize.x, frame.gapTl.x, frame.gapBr.x);
	gaps(nearCorner.y, farCorner.y, boxSize.y, frame.gapTl.y, frame.gapBr.y);
	return frame;
}

// The id the snap leaves out for one gesture member: its own at top level, its GROUP's item id
// for a child (see DragState::snapExcludeIds). Zero ids are never real, so a child whose group
// item cannot be found contributes nothing rather than a wrong exclusion.
void AddSnapExclusion(std::vector<int64_t> &ids, obs_sceneitem_t *item, obs_sceneitem_t *groupItem)
{
	obs_sceneitem_t *topLevel = groupItem ? groupItem : item;
	if (!topLevel) {
		return;
	}
	const int64_t id = obs_sceneitem_get_id(topLevel);
	if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
		ids.push_back(id);
	}
}

// Record one member of a gesture: the key that re-resolves it, the position it started at, and
// the map from the space that position is written in onto the canvas. `sceneSource` is the
// scene the member belongs to, recorded with its key so a scene switch mid-gesture cannot
// redirect the drag onto the new scene's item of the same id. False when the member's group
// does not map to the canvas, which is the one thing every gesture here depends on.
bool AddDragItem(DragState &drag, obs_source_t *sceneSource, obs_sceneitem_t *item, obs_sceneitem_t *groupItem)
{
	matrix4 ownerToCanvas;
	if (!SceneItems::GroupToCanvas(groupItem, ownerToCanvas)) {
		return false;
	}
	drag.items.emplace_back();
	DragItem &member = drag.items.back();
	member.id.Set(sceneSource, SceneItems::KeyOf(item));
	obs_sceneitem_get_pos(item, &member.startItemPos);
	member.ownerToCanvas = ownerToCanvas;
	AddSnapExclusion(drag.snapExcludeIds, item, groupItem);
	return true;
}

// Open a gesture on the one item a handle names. The item is still recorded as a member
// the way a move records each of its own, so the undo capture has one shape for every
// gesture. `groupItem` is the group item drawing it, null at top level; its re-fit is held for
// the whole gesture, because the resize changes the child's extent and a re-fit between two
// frames would shift every sibling -- and the child's own start position -- under the drag.
// False when the group's maps cannot be built, leaving the drag untouched.
bool BeginHandleGesture(DragState &drag, DragMode mode, obs_source_t *sceneSource, obs_sceneitem_t *item,
			obs_sceneitem_t *groupItem, ItemHandle handle, const vec2 &startCanvasPos)
{
	matrix4 ownerToCanvas;
	matrix4 canvasToOwner;
	if (!SceneItems::GroupToCanvas(groupItem, ownerToCanvas) ||
	    !SceneItems::CanvasToGroup(groupItem, canvasToOwner)) {
		return false;
	}
	drag.Reset();
	SceneItems::HoldGroup(drag.groupHolds, groupItem);
	// Under the hold, so the flag the tick would have re-fitted the group from is consumed
	// here rather than left to move the group mid-gesture.
	SceneItems::ApplyPendingChildUpdate(item);
	drag.mode = mode;
	drag.moved = false;
	drag.canvasToOwner = canvasToOwner;
	drag.ownerToCanvas = ownerToCanvas;
	if (!AddDragItem(drag, sceneSource, item, groupItem)) {
		drag.Reset();
		return false;
	}
	drag.id = drag.items.back().id;
	drag.handle = handle;
	drag.startCanvasPos = startCanvasPos;
	return true;
}

// Capture the matrices/sizes a resize drag needs (legacy GetStretchHandleData) for the chosen
// item + handle. The box is the item's own, in the space its transform is written in, so the
// whole resize runs there and only the pointer crosses the group boundary.
void BeginResize(DragState &drag, obs_source_t *sceneSource, obs_sceneitem_t *item, obs_sceneitem_t *groupItem,
		 ItemHandle handle, const vec2 &startCanvasPos)
{
	if (!BeginHandleGesture(drag, DragMode::Resize, sceneSource, item, groupItem, handle, startCanvasPos)) {
		return;
	}
	matrix4 boxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);
	drag.stretchItemSize = GetItemSize(item);

	const float itemRot = obs_sceneitem_get_rot(item);
	vec3 itemUL;
	vec3_from_vec4(&itemUL, &boxTransform.t);

	matrix4_identity(&drag.itemToScreen);
	matrix4_rotate_aa4f(&drag.itemToScreen, &drag.itemToScreen, 0.0f, 0.0f, 1.0f, RAD(itemRot));
	matrix4_translate3f(&drag.itemToScreen, &drag.itemToScreen, itemUL.x, itemUL.y, 0.0f);

	matrix4_identity(&drag.screenToItem);
	matrix4_translate3f(&drag.screenToItem, &drag.screenToItem, -itemUL.x, -itemUL.y, 0.0f);
	matrix4_rotate_aa4f(&drag.screenToItem, &drag.screenToItem, 0.0f, 0.0f, 1.0f, RAD(-itemRot));

	obs_sceneitem_get_crop(item, &drag.startCrop);
	drag.crop = CaptureCropFrame(item, drag.screenToItem, drag.stretchItemSize, drag.startCrop);
}

// A canvas point in the space the gesture's anchor is written in: group space for a child, the
// canvas's own for a top-level item, where canvasToOwner is the identity.
vec2 ToOwnerSpace(const DragState &drag, const vec2 &canvasPos)
{
	vec3 point;
	vec3_set(&point, canvasPos.x, canvasPos.y, 0.0f);
	vec3_transform(&point, &point, &drag.canvasToOwner);
	vec2 out;
	vec2_set(&out, point.x, point.y);
	return out;
}

// The pointer's angle about the rotation pivot, in degrees. Both are in the anchor's own
// space, so a child turns by the angle its own rot is measured in -- the same one the dock's
// rotation field and the 45-degree snap targets use -- rather than by a canvas angle its
// group's scale would distort.
float PointerAngle(const DragState &drag, const vec2 &ownerPos)
{
	return DEG(std::atan2(ownerPos.y - drag.rotateCenter.y, ownerPos.x - drag.rotateCenter.x));
}

// Capture the pivot a rotation turns about, as the legacy FindHandleAtPos does when it
// picks the rotation handle (OBSBasicPreview.cpp:419-431): the box centre, and the
// item's position relative to it with the start rotation taken out, so each frame can
// place the position by turning that offset to the new angle. Also the pointer's own
// angle at the press, which each frame's rotation is measured from. All in the item's own
// space, which is where its position is written.
void BeginRotate(DragState &drag, obs_source_t *sceneSource, obs_sceneitem_t *item, obs_sceneitem_t *groupItem,
		 const vec2 &startCanvasPos)
{
	if (!BeginHandleGesture(drag, DragMode::Rotate, sceneSource, item, groupItem, ItemHandle::Rot,
				startCanvasPos)) {
		return;
	}
	matrix4 boxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);

	const vec3 center = GetTransformedPos(0.5f, 0.5f, boxTransform);

	drag.rotateStartAngle = obs_sceneitem_get_rot(item);
	vec2_set(&drag.rotateCenter, center.x, center.y);
	drag.rotatePressAngle = PointerAngle(drag, ToOwnerSpace(drag, startCanvasPos));
	vec2 offset;
	vec2_sub(&offset, &drag.items.back().startItemPos, &drag.rotateCenter);
	drag.rotateOffset = RotateVec2(offset, RAD(-drag.rotateStartAngle));
	drag.rotateApplied = false;
}

// The distance between two angles in degrees, the short way round.
float AngleGap(float a, float b)
{
	return std::fabs(std::remainder(a - b, 360.0f));
}

// Whether two rotations in degrees are the same angle modulo a full turn.
bool SameRotation(float a, float b)
{
	return AngleGap(a, b) < kRotSameAngleEpsilon;
}

// An angle in degrees after snapping; see kRotSnapPull for the rules. Distances are taken
// the short way round, so a target pulls across the wrap point as well. Each target is
// tried against the angle the previous one left, so a snap-step multiple within reach of
// the start angle takes over from it, as it does in the legacy macro sequence.
float SnapRotation(float angle, float startAngle, const Modifiers &mods)
{
	const auto pull = [&angle](float target, float within) {
		if (AngleGap(angle, target) < within) {
			angle = target;
		}
	};
	if (mods.shift) {
		for (float target = kRotAngleMin; target <= kRotAngleMax; target += kRotShiftStep) {
			pull(target, kRotShiftStep * 0.5f);
		}
	} else if (!mods.ctrl) {
		pull(startAngle, kRotSnapPull);
		for (float target = kRotAngleMin; target <= kRotAngleMax; target += kRotSnapStep) {
			pull(target, kRotSnapPull);
		}
	}
	return angle;
}

// Turn the active drag item by the angle the pointer has swept about the box centre since
// the press (after the legacy RotateItem, OBSBasicPreview.cpp:1557-1601, which instead
// points the item's top at the pointer and so jumps by however far off-axis the press
// landed on the disc). The position is the recorded offset turned to the new angle, which
// is what keeps the box centre fixed whatever the item's alignment.
//
// At the start angle, however it is spelled (-10 for a start of 350), nothing is
// computed or written: the recomputed position carries float error and the angle its
// wrapped form, either of which would make the gesture's AFTER capture differ from its
// BEFORE and record an undo entry for a press-and-jiggle. A return to the start angle
// from elsewhere writes the recorded start values back instead.
void RotateItem(DragState &drag, obs_sceneitem_t *item, const vec2 &ownerPos, const Modifiers &mods)
{
	const float swept = PointerAngle(drag, ownerPos) - drag.rotatePressAngle;
	const float angle =
		WrapDegrees(SnapRotation(drag.rotateStartAngle + swept, drag.rotateStartAngle, mods), kRotAngleMin);

	if (SameRotation(angle, drag.rotateStartAngle)) {
		if (drag.rotateApplied) {
			obs_sceneitem_set_rot(item, drag.rotateStartAngle);
			obs_sceneitem_set_pos(item, &drag.items.back().startItemPos);
			drag.rotateApplied = false;
		}
		return;
	}

	vec2 pos = RotateVec2(drag.rotateOffset, RAD(angle));
	vec2_add(&pos, &pos, &drag.rotateCenter);

	obs_sceneitem_set_rot(item, angle);
	obs_sceneitem_set_pos(item, &pos);
	drag.rotateApplied = true;
}

// One end of a drag's undo pair: EVERY dragged item's full geometry plus the keys that
// re-resolve each, addressed by this surface's canvas and by the scene they were resolved
// in. One payload for the whole gesture, so however many items a move drags it stays a
// single undo step. Empty when nothing resolved.
std::string CaptureDragUndoState(obs_canvas_t *targetCanvas, obs_source_t *sceneSource,
				 const std::vector<obs_sceneitem_t *> &items)
{
	if (items.empty() || !sceneSource) {
		return std::string();
	}
	const char *canvasUuid = targetCanvas ? obs_canvas_get_uuid(targetCanvas) : nullptr;
	const char *sceneName = obs_source_get_name(sceneSource);
	return Bridge::CaptureItemTransformStates(canvasUuid ? canvasUuid : "", sceneName ? sceneName : "",
						  items.data(), items.size());
}

// The dragged items of a gesture, re-resolved in `scene` and in the gesture's own order.
// Entries that no longer resolve are dropped, so the vector can be shorter than
// drag.items -- or empty, which every caller treats as "nothing to do".
std::vector<obs_sceneitem_t *> ResolveDragItems(obs_scene_t *scene, const std::vector<DragItem> &dragItems,
						const char *sceneUuid)
{
	std::vector<obs_sceneitem_t *> out;
	out.reserve(dragItems.size());
	for (const DragItem &d : dragItems) {
		obs_sceneitem_t *item = d.id.Find(scene, sceneUuid);
		if (item) {
			out.push_back(item);
		}
	}
	return out;
}

// A group locked after a gesture began stops that gesture where it stands, exactly as a lock
// on the item itself does. The gesture already holds a reference on every group it writes
// into, so this needs no lookup of its own; one locked group stops the whole drag, which over
// a selection spanning two groups is the conservative half of the trade.
bool AnyHeldGroupLocked(const DragState &drag)
{
	// GroupItem() reads null on an ended hold, and obs_sceneitem_locked does not take one.
	return std::any_of(drag.groupHolds.begin(), drag.groupHolds.end(),
			   [](const SceneItems::GroupResizeDeferral &hold) {
				   return hold.GroupItem() && obs_sceneitem_locked(hold.GroupItem());
			   });
}

// The scene a drag STARTED in, addref'd (caller releases) or null once that scene is
// gone. Deliberately not the surface's current scene, which is what the rest of the drag
// path resolves against: a scene switch mid-gesture makes the remaining frames inert but
// does not un-move what the earlier ones already moved, and the entry that reverses them
// has to name the scene they landed in.
obs_source_t *AcquireDragScene(const SceneItemRef &draggedRef)
{
	if (draggedRef.Empty()) {
		return nullptr;
	}
	return Bridge::AcquireSceneByUuid(draggedRef.sceneUuid); // addref'd
}

// --- selection -------------------------------------------------------------

bool SelectSetCb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	const auto *keys = static_cast<const std::vector<SceneItemKey> *>(param);
	obs_sceneitem_select(item, ContainsKey(*keys, SceneItems::KeyOf(item)));
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, SelectSetCb, param);
	}
	return true;
}

// Mirror `keys` onto libobs' own per-item selected flags, the children of every group
// included, deselecting everything else; an empty set clears. This is a one-way mirror:
// nothing in this frontend reads obs_sceneitem_selected back, because State::selected is
// the single source of truth. It is kept written because libobs and plugins surface the
// flag elsewhere.
void SelectSet(obs_scene_t *scene, const std::vector<SceneItemKey> &keys)
{
	obs_scene_enum_items(scene, SelectSetCb, const_cast<void *>(static_cast<const void *>(&keys)));
}

// `keys` without the children that no longer resolve in `scene`: their group was removed or
// ungrouped, or the child itself left it. A top-level key is kept as it is, which is how the
// selection has always treated one. A dropped child must not reach a reply or an event, where
// it would name an item the docks cannot find.
std::vector<SceneItemKey> WithoutStaleChildren(obs_scene_t *scene, std::vector<SceneItemKey> keys)
{
	keys.erase(std::remove_if(keys.begin(), keys.end(),
				  [scene](const SceneItemKey &key) {
					  return !key.IsTopLevel() && !SceneItems::FindItem(scene, key);
				  }),
		   keys.end());
	return keys;
}

// --- drawing (ported from legacy DrawLine/DrawSquareAtPos/DrawRect) ----------

// Draw a thin line (as a quad) in the current matrix space; thickness in screen
// px, divided by the per-axis box scale (itself screen px per unit) so the
// on-screen width is constant.
void DrawLine(float x1, float y1, float x2, float y2, float thickness, const vec2 &boxScale)
{
	vec2 scale;
	vec2_abs(&scale, &boxScale);
	const float tx = thickness / scale.x;
	const float ty = thickness / scale.y;
	const bool xAxis = !CloseFloat(x1, x2, 0.0000001f) || CloseFloat(y1, y2, 0.0000001f);

	float cx, cy;
	if (xAxis) {
		cx = std::fabs(x2 - x1) + tx;
		cy = ty;
	} else {
		cy = std::fabs(y2 - y1) + ty;
		cx = tx;
	}
	x1 -= tx * 0.5f;
	y1 -= ty * 0.5f;

	gs_matrix_push();
	gs_matrix_translate3f(x1, y1, 0.0f);
	gs_draw_quadf(nullptr, 0, cx, cy);
	gs_matrix_pop();
}

// The 4 box edges in unit space, drawn inside the box-transform matrix so a
// rotated/scaled item still boxes correctly.
void DrawRect(float thickness, const vec2 &boxScale)
{
	DrawLine(0.0f, 0.0f, 0.0f, 1.0f, thickness, boxScale);
	DrawLine(0.0f, 0.0f, 1.0f, 0.0f, thickness, boxScale);
	DrawLine(1.0f, 0.0f, 1.0f, 1.0f, thickness, boxScale);
	DrawLine(0.0f, 1.0f, 1.0f, 1.0f, thickness, boxScale);
}

// Push a matrix whose origin is the unit-space point (x, y) and whose units are screen
// px. Reads the current matrix -- in the editing phase, the letterbox scale times the
// item's box transform -- maps the point through it into that phase's screen-px space,
// and resets to a plain translation there, so a handle drawn after it keeps its screen
// size at any zoom or item scale. The caller pops.
void PushHandleAnchor(float x, float y)
{
	vec3 pos;
	vec3_set(&pos, x, y, 0.0f);
	matrix4 matrix;
	gs_matrix_get(&matrix);
	vec3_transform(&pos, &pos, &matrix);

	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_translate(&pos);
}

// Draw a filled, axis-aligned square at a unit-space handle coord; `halfSize` is screen
// px (see PushHandleAnchor).
void DrawSquareAtPos(float x, float y, float halfSize)
{
	PushHandleAnchor(x, y);
	gs_matrix_translate3f(-halfSize, -halfSize, 0.0f);
	gs_matrix_scale3f(halfSize * 2.0f, halfSize * 2.0f, 1.0f);
	gs_draw(GS_TRISTRIP, 0, 0);
	gs_matrix_pop();
}

// Draw the rotation handle: a stem out from the midpoint of the item's visual top edge
// to a filled disc, turned with the item, after the legacy DrawRotationHandle
// (frontend_old/widgets/OBSBasicPreview.cpp:1763-1793). The disc is centred on the point
// FindHandleAtPos tests and sized to its grab zone -- `edgeY` and `out` come from the same
// RotHandleStandoff call, so the two cannot disagree. The stem is drawn along -y before the
// turn, so the angle that points it along `out` is atan2(out.x, -out.y). Measured in screen px
// (see PushHandleAnchor); the caller has the Solid technique begun and the color set.
void DrawRotationHandle(float edgeY, const vec2 &out, gs_vertbuffer_t *circleBuffer)
{
	PushHandleAnchor(0.5f, edgeY);
	gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, std::atan2(out.x, -out.y));

	gs_matrix_push();
	gs_matrix_translate3f(-kBoxLineThickness * 0.5f, -kRotHandleDistance, 0.0f);
	gs_draw_quadf(nullptr, 0, kBoxLineThickness, kRotHandleDistance);
	gs_matrix_pop();

	gs_matrix_translate3f(-kHandleSelRadius, -kRotHandleDistance - kHandleSelRadius, 0.0f);
	gs_matrix_scale3f(kHandleSelRadius * 2.0f, kHandleSelRadius * 2.0f, 1.0f);
	gs_load_vertexbuffer(circleBuffer);
	gs_draw(GS_TRISTRIP, 0, 0);

	gs_matrix_pop();
}

// Outline colors: the selection green this file already used, and the legacy
// preview's hover blue (OBSBasic::GetHoverColor's non-override default,
// rgb(0,127,255)).
const vec4 kSelectionColor = {{{0.0f, 1.0f, 0.235f, 1.0f}}};
const vec4 kHoverColor = {{{0.0f, 0.498f, 1.0f, 1.0f}}};

// The combined bounding box drawn around a multi-item selection: the selection green
// at half alpha, so it reads as subordinate to the per-item outlines it encloses.
const vec4 kGroupBoxColor = {{{0.0f, 1.0f, 0.235f, 0.5f}}};

// The rubber band: the legacy DrawSelectionBox's 50%-alpha light-grey fill and
// opaque white border (frontend_old/widgets/OBSBasicPreview.cpp:2159-2163).
const vec4 kBandFillColor = {{{0.7f, 0.7f, 0.7f, 0.5f}}};
const vec4 kBandBorderColor = {{{1.0f, 1.0f, 1.0f, 1.0f}}};

// Draw an axis-aligned canvas-space rect, optionally filled. Runs in the editing
// phase, whose matrix stack already carries the letterbox scale, so `scale` converts
// the screen-px thickness into the canvas units the matrix is measured in. Shared by
// the combined selection box and the rubber band, which differ only in their colors
// and in whether they fill.
void DrawCanvasRect(const vec2 &tl, const vec2 &br, float scale, const vec4 &outline, const vec4 *fill,
		    gs_vertbuffer_t *fillBuffer)
{
	const float w = br.x - tl.x;
	const float h = br.y - tl.y;
	if (scale <= 0.0f || w <= 0.0f || h <= 0.0f) {
		return;
	}

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *colParam = gs_effect_get_param_by_name(solid, "color");

	// The unit square scaled to the rect, so DrawRect's unit-space edges land on it.
	// boxScale is screen px per unit along each axis, which is what keeps the stroke a
	// constant kBoxLineThickness on screen however far the preview is zoomed.
	vec2 boxScale;
	vec2_set(&boxScale, w * scale, h * scale);

	gs_matrix_push();
	gs_matrix_translate3f(tl.x, tl.y, 0.0f);
	gs_matrix_scale3f(w, h, 1.0f);

	if (fill && fillBuffer) {
		gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
		gs_technique_begin(tech);
		gs_technique_begin_pass(tech, 0);
		gs_effect_set_vec4(colParam, fill);
		gs_load_vertexbuffer(fillBuffer);
		gs_draw(GS_TRISTRIP, 0, 0);
		// Unbind before leaving: the device keeps the last loaded buffer, and
		// nothing downstream of this callback is obliged to load its own.
		gs_load_vertexbuffer(nullptr);
		gs_technique_end_pass(tech);
		gs_technique_end(tech);
	}

	gs_effect_set_vec4(colParam, &outline);
	while (gs_effect_loop(solid, "Solid")) {
		DrawRect(kBoxLineThickness, boxScale);
	}

	gs_matrix_pop();
}

// Draw `item`'s box outline in `color`, plus the 8 resize handles and the rotation
// handle when `handleBuffer` (the shared unit-quad TRISTRIP vertbuffer) and
// `circleBuffer` (the rotation handle's disc) are non-null. All are drawn inside the
// box-transform matrix in unit space, so they follow the item's rotation/scale. Runs
// in the draw callback's editing phase, whose ortho is screen px and whose matrix
// stack already carries the letterbox scale (see RenderPreview). `scale` = letterbox
// screen-px-per-canvas-unit: boxScale maps unit->screen px so the line thickness stays
// ~constant on screen, and the handle half-size is kHandleRadius unscaled because
// the handles draw off a reset matrix, in this phase's screen px (PushHandleAnchor).
// `held` is the item and, for a child, the group item drawing it.
void DrawItemBox(const SceneItems::HeldSceneItem &held, float scale, const vec4 &color, gs_vertbuffer_t *handleBuffer,
		 gs_vertbuffer_t *circleBuffer)
{
	matrix4 boxTransform;
	if (scale <= 0.0f || !SceneItems::ItemBoxThroughGroup(held.item, held.group, boxTransform)) {
		return;
	}

	// The length of each box axis on the canvas: a child's box is scaled by its group too.
	vec2 boxScale;
	vec2_set(&boxScale, std::hypot(boxTransform.x.x, boxTransform.x.y) * scale,
		 std::hypot(boxTransform.y.x, boxTransform.y.y) * scale);

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *colParam = gs_effect_get_param_by_name(solid, "color");

	gs_matrix_push();
	gs_matrix_mul(&boxTransform);

	gs_effect_set_vec4(colParam, &color);
	while (gs_effect_loop(solid, "Solid")) {
		DrawRect(kBoxLineThickness, boxScale);
	}

	if (handleBuffer && circleBuffer) {
		gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
		gs_technique_begin(tech);
		gs_technique_begin_pass(tech, 0);
		gs_load_vertexbuffer(handleBuffer);
		gs_effect_set_vec4(colParam, &color);

		DrawSquareAtPos(0.0f, 0.0f, kHandleRadius);
		DrawSquareAtPos(0.5f, 0.0f, kHandleRadius);
		DrawSquareAtPos(1.0f, 0.0f, kHandleRadius);
		DrawSquareAtPos(0.0f, 0.5f, kHandleRadius);
		DrawSquareAtPos(1.0f, 0.5f, kHandleRadius);
		DrawSquareAtPos(0.0f, 1.0f, kHandleRadius);
		DrawSquareAtPos(0.5f, 1.0f, kHandleRadius);
		DrawSquareAtPos(1.0f, 1.0f, kHandleRadius);
		// Last: it loads its own buffer over the one the squares draw from.
		float rotEdgeY = 0.0f;
		vec2 rotOut;
		RotHandleStandoff(held.item, boxTransform, rotEdgeY, rotOut);
		DrawRotationHandle(rotEdgeY, rotOut, circleBuffer);

		// Unbind before leaving: the device keeps the last loaded buffer, and
		// nothing downstream of this callback is obliged to load its own.
		gs_load_vertexbuffer(nullptr);
		gs_technique_end_pass(tech);
		gs_technique_end(tech);
	}

	gs_matrix_pop();
}

// --- guide overlays (ported from legacy DrawOverflow/RenderSafeAreas/DrawSpacingHelpers) ---

// The overflow fill's tile, generated rather than shipped: the legacy
// data/images/overflow.png's pattern -- a 32 px square of 45-degree stripes, 16 px white
// then 16 px black, every pixel at alpha 34. The PNG matches except that 29 of its black
// pixels are (1,1,1), a difference nothing at that alpha can show. The legacy
// DrawSelectedOverflow (frontend_old/widgets/OBSBasicPreview.cpp:1926-1927) repeats it
// once per 96 canvas px of the item's box.
constexpr uint32_t kOverflowTileSize = 32;
constexpr uint8_t kOverflowTileAlpha = 34;
constexpr float kOverflowTileCanvasPx = 96.0f;

gs_texture_t *CreateOverflowTexture()
{
	std::array<uint8_t, kOverflowTileSize * kOverflowTileSize * 4> pixels;
	for (uint32_t y = 0; y < kOverflowTileSize; y++) {
		for (uint32_t x = 0; x < kOverflowTileSize; x++) {
			const uint8_t value =
				((x + y + kOverflowTileSize - 2) % kOverflowTileSize) < kOverflowTileSize / 2 ? 255 : 0;
			const size_t i = (size_t(y) * kOverflowTileSize + x) * 4;
			pixels[i] = value;
			pixels[i + 1] = value;
			pixels[i + 2] = value;
			pixels[i + 3] = kOverflowTileAlpha;
		}
	}
	const uint8_t *data = pixels.data();
	return gs_texture_create(kOverflowTileSize, kOverflowTileSize, GS_RGBA, 1, &data, 0);
}

struct OverflowDraw {
	gs_texture_t *texture;
	const PreviewOverlays *overlays;
	const std::vector<SceneItemKey> *selected;
};

// Fill one item's whole box with the overflow tile. Drawn BEFORE the canvas, which
// then paints over the part inside it, so what stays striped is exactly the part of
// the item that lies outside the canvas. The item filters follow the legacy
// DrawSelectedOverflow (OBSBasicPreview.cpp:1864-1951), minus its group descent.
bool DrawItemOverflow(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	const auto *draw = static_cast<const OverflowDraw *>(param);
	if (obs_sceneitem_locked(item) || !SceneItemHasVideo(item)) {
		return true;
	}
	if (!draw->overlays->overflowInvisible && !obs_sceneitem_visible(item)) {
		return true;
	}
	if (draw->overlays->overflow != PreviewOverflowMode::Always &&
	    !ContainsKey(*draw->selected, SceneItems::KeyOf(item))) {
		return true;
	}

	matrix4 boxTransform;
	matrix4 inverse;
	if (!SceneItems::ItemBoxToCanvas(item, boxTransform) || !InvertBoxTransform(boxTransform, inverse)) {
		return true;
	}

	// One tile per kOverflowTileCanvasPx along each of the box's own axes, measured by
	// their length so a rotated item keeps its stripe density. A mirrored box negates one
	// texture axis, which mirrors the stripes back to the diagonal an unmirrored box shows.
	const float axisX = std::hypot(boxTransform.x.x, boxTransform.x.y);
	const float axisY = std::hypot(boxTransform.y.x, boxTransform.y.y);
	const bool mirrored = boxTransform.x.x * boxTransform.y.y - boxTransform.x.y * boxTransform.y.x < 0.0f;
	gs_effect_t *repeat = obs_get_base_effect(OBS_EFFECT_REPEAT);
	vec2 tiles;
	vec2_set(&tiles, (mirrored ? -axisX : axisX) / kOverflowTileCanvasPx, axisY / kOverflowTileCanvasPx);
	gs_effect_set_vec2(gs_effect_get_param_by_name(repeat, "scale"), &tiles);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(repeat, "image"), draw->texture);

	gs_matrix_push();
	gs_matrix_mul(&boxTransform);
	const bool previousSrgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	while (gs_effect_loop(repeat, "Draw")) {
		gs_draw_sprite(draw->texture, 0, 1, 1);
	}
	gs_enable_framebuffer_srgb(previousSrgb);
	gs_matrix_pop();
	return true;
}

// Safe-area margins, Rec. ITU-R BT.1848-1 / EBU R 95, and the centre marks on three
// edges, as the legacy InitSafeAreas builds them (frontend_old/utility/display-helpers.hpp:62-118):
// each strip is a 1 px line strip in unit canvas space.
constexpr float kActionSafe = 0.035f;
constexpr float kGraphicsSafe = 0.05f;
constexpr float kFourByThreeSafe = 0.1625f;
constexpr float kSafeMarkLength = 0.1f;

struct GuideStrip {
	int count;
	float points[5][2];
};

constexpr GuideStrip kSafeAreaStrips[] = {
	{5,
	 {{kActionSafe, kActionSafe},
	  {kActionSafe, 1.0f - kActionSafe},
	  {1.0f - kActionSafe, 1.0f - kActionSafe},
	  {1.0f - kActionSafe, kActionSafe},
	  {kActionSafe, kActionSafe}}},
	{5,
	 {{kGraphicsSafe, kGraphicsSafe},
	  {kGraphicsSafe, 1.0f - kGraphicsSafe},
	  {1.0f - kGraphicsSafe, 1.0f - kGraphicsSafe},
	  {1.0f - kGraphicsSafe, kGraphicsSafe},
	  {kGraphicsSafe, kGraphicsSafe}}},
	{5,
	 {{kFourByThreeSafe, kGraphicsSafe},
	  {1.0f - kFourByThreeSafe, kGraphicsSafe},
	  {1.0f - kFourByThreeSafe, 1.0f - kGraphicsSafe},
	  {kFourByThreeSafe, 1.0f - kGraphicsSafe},
	  {kFourByThreeSafe, kGraphicsSafe}}},
	{2, {{0.0f, 0.5f}, {kSafeMarkLength, 0.5f}}},
	{2, {{0.5f, 0.0f}, {0.5f, kSafeMarkLength}}},
	{2, {{1.0f, 0.5f}, {1.0f - kSafeMarkLength, 0.5f}}},
};

using SafeAreaBuffers = std::array<gs_vertbuffer_t *, std::size(kSafeAreaStrips)>;

// The legacy OUTLINE_COLOR, 0xFFD0D0D0.
const vec4 kSafeAreaColor = {{{0xD0 / 255.0f, 0xD0 / 255.0f, 0xD0 / 255.0f, 1.0f}}};

// Draw the safe-area strips over a canvas drawn `drawCX` x `drawCY` screen px, in the
// editing phase's screen-px space before any letterbox scale is pushed.
void DrawSafeAreas(const SafeAreaBuffers &buffers, float drawCX, float drawCY)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &kSafeAreaColor);

	gs_matrix_push();
	gs_matrix_scale3f(drawCX, drawCY, 1.0f);
	for (gs_vertbuffer_t *buffer : buffers) {
		gs_load_vertexbuffer(buffer);
		while (gs_effect_loop(solid, "Solid")) {
			gs_draw(GS_LINESTRIP, 0, 0);
		}
	}
	gs_load_vertexbuffer(nullptr);
	gs_matrix_pop();
}

// Spacing helpers, after the legacy DrawSpacingHelpers/RenderSpacingHelper
// (OBSBasicPreview.cpp:2482-2707): a line from each edge of the one selected item to
// the canvas edge it faces, labelled with its length in canvas px.
constexpr float kSpacingRotBreakpoint = 45.0f;
constexpr float kSpacingLabelMargin = 6.0f;
constexpr int kSpacingLabelFontSize = 16;

// One of the four labels: its private text source, created on first use on the
// render thread, and the px value it currently reads (-1 before the first).
struct SpacingLabel {
	obs_source_t *source = nullptr;
	int px = -1;
};

enum SpacingSide { kSpacingTop, kSpacingBottom, kSpacingLeft, kSpacingRight, kSpacingSideCount };

using SpacingLabels = std::array<SpacingLabel, kSpacingSideCount>;

// Draw one helper from `start` to `end` (canvas units, start nearer the canvas origin)
// and its label. Nothing when the item edge lies beyond the canvas edge it measures to.
// Runs with the letterbox scale pushed; `scale` is screen px per canvas unit.
void DrawSpacingHelper(SpacingLabel &label, int side, const vec3 &start, const vec3 &end, float scale)
{
	const bool horizontal = side == kSpacingLeft || side == kSpacingRight;
	if (horizontal ? end.x < start.x : end.y < start.y) {
		return;
	}
	const float length = vec3_dist(&start, &end);
	if (length <= 0.0f) {
		return;
	}

	if (!label.source) {
		OBSDataAutoRelease extra = obs_data_create();
		obs_data_set_int(extra, "outline_color", 0x000000);
		obs_data_set_int(extra, "outline_size", 3);
		const std::string name = "Preview spacing label " + std::to_string(side);
		label.source = SourceRender::CreateTextLabel(name.c_str(), "", kSpacingLabelFontSize, extra);
		if (!label.source) {
			return;
		}
	}

	const float labelW = float(obs_source_get_width(label.source)) / scale;
	const float labelH = float(obs_source_get_height(label.source)) / scale;
	const float margin = kSpacingLabelMargin / scale;
	vec2 labelPos;
	if (horizontal) {
		vec2_set(&labelPos, end.x - (end.x - start.x) * 0.5f - labelW * 0.5f,
			 end.y - margin - labelH * 0.5f - kHandleRadius / scale);
	} else {
		vec2_set(&labelPos, end.x + margin, end.y - (end.y - start.y) * 0.5f - labelH * 0.5f);
	}

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &kSelectionColor);
	vec2 boxScale;
	vec2_set(&boxScale, scale, scale);
	while (gs_effect_loop(solid, "Solid")) {
		DrawLine(start.x, start.y, end.x, end.y, kBoxLineThickness, boxScale);
	}

	const int px = int(length);
	if (px != label.px) {
		OBSDataAutoRelease settings = obs_source_get_settings(label.source);
		obs_data_set_string(settings, "text", (std::to_string(px) + " px").c_str());
		obs_source_update(label.source, settings);
		label.px = px;
	}

	PushHandleAnchor(labelPos.x, labelPos.y);
	obs_source_video_render(label.source);
	gs_matrix_pop();
}

// A child gets none. The side remap below decides which of the item's four edges faces which
// canvas edge from the item's OWN rotation and scale, which for a child are measured in its
// group's space; a group's own turn and mirroring never reach it, so through a rotated group
// it would label the wrong edges. Deliberately left as it is rather than wired up wrong.
void DrawSpacingHelpers(SpacingLabels &labels, obs_scene_t *scene, const std::vector<SceneItemKey> &selected,
			float scale, float baseCX, float baseCY)
{
	if (selected.size() != 1 || !selected.front().IsTopLevel()) {
		return;
	}
	SceneItems::HeldSceneItem held;
	if (!SceneItems::AcquireItem(scene, selected.front(), held) || obs_sceneitem_locked(held.item)) {
		return;
	}
	obs_sceneitem_t *item = held.item;
	const vec2 itemSize = GetItemSize(item);
	if (itemSize.x == 0.0f || itemSize.y == 0.0f) {
		return;
	}

	matrix4 boxTransform;
	if (!SceneItems::ItemBoxToCanvas(item, boxTransform)) {
		return;
	}
	obs_transform_info info;
	obs_sceneitem_get_info2(item, &info);

	// The unit-box midpoint of each side, then the legacy remap that decides which of
	// them faces which canvas edge: a flip swaps opposite sides, and every quarter turn
	// past 45 degrees moves each one round to the next.
	vec2 left, right, top, bottom;
	vec2_set(&left, 0.0f, 0.5f);
	vec2_set(&right, 1.0f, 0.5f);
	vec2_set(&top, 0.5f, 0.0f);
	vec2_set(&bottom, 0.5f, 1.0f);
	if (info.scale.x < 0.0f && info.bounds_type == OBS_BOUNDS_NONE) {
		std::swap(left, right);
	}
	if (info.scale.y < 0.0f && info.bounds_type == OBS_BOUNDS_NONE) {
		std::swap(top, bottom);
	}
	const float rot = info.rot;
	if (rot >= kSpacingRotBreakpoint) {
		for (float i = kSpacingRotBreakpoint; i <= 360.0f && rot >= i; i += 90.0f) {
			const vec2 l = left, r = right, t = top, b = bottom;
			top = l;
			right = t;
			bottom = r;
			left = b;
		}
	} else if (rot <= -kSpacingRotBreakpoint) {
		for (float i = -kSpacingRotBreakpoint; i >= -360.0f && rot <= i; i -= 90.0f) {
			const vec2 l = left, r = right, t = top, b = bottom;
			top = r;
			right = b;
			bottom = l;
			left = t;
		}
	}

	const vec3 l = GetTransformedPos(left.x, left.y, boxTransform);
	const vec3 r = GetTransformedPos(right.x, right.y, boxTransform);
	const vec3 t = GetTransformedPos(top.x, top.y, boxTransform);
	const vec3 b = GetTransformedPos(bottom.x, bottom.y, boxTransform);

	vec3 start, end;
	vec3_set(&start, t.x, 0.0f, 0.0f);
	vec3_set(&end, t.x, t.y, 0.0f);
	DrawSpacingHelper(labels[kSpacingTop], kSpacingTop, start, end, scale);

	vec3_set(&start, b.x, b.y, 0.0f);
	vec3_set(&end, b.x, baseCY, 0.0f);
	DrawSpacingHelper(labels[kSpacingBottom], kSpacingBottom, start, end, scale);

	vec3_set(&start, 0.0f, l.y, 0.0f);
	vec3_set(&end, l.x, l.y, 0.0f);
	DrawSpacingHelper(labels[kSpacingLeft], kSpacingLeft, start, end, scale);

	vec3_set(&start, r.x, r.y, 0.0f);
	vec3_set(&end, baseCX, r.y, 0.0f);
	DrawSpacingHelper(labels[kSpacingRight], kSpacingRight, start, end, scale);
}

} // namespace

// Per-surface state shared between the render thread (draw callback) and the UI
// thread (WndProc + bridge). One mutex guards the selection/hover ids + letterbox
// transform; copy out under the lock and never hold it across a libobs render
// call. The drag/cursor state + box buffer are touched only on their owning thread
// (drag + cursor = UI thread, box buffer = render thread under a graphics
// context), but live here so they are per-surface, not process-global.
struct PreviewSurface::State {
	std::mutex stateMutex;
	SceneItemSelection selected;
	SceneItemRef hovered;
	PreviewTransform transform;
	PreviewView view;

	DragState drag;                         // UI thread only
	const wchar_t *cursorShape = IDC_ARROW; // UI thread only; re-applied on WM_SETCURSOR
	bool mouseTracked = false;              // UI thread only; TME_LEAVE armed for this surface

	// The pan gesture, kept apart from DragState because it has no item: every
	// `drag.mode != DragMode::None` test in this file means "an item gesture is in
	// flight", and a pan must not answer those. UI thread only.
	bool panning = false;
	POINT panFrom = {};

	// The rubber band. `active`/`start`/`current` are written on the UI thread under
	// stateMutex and read by the draw callback; the rest is UI thread only.
	BoxState box;

	// What the press resolved, held until the first move or the release decides what
	// the gesture was. Selection is deliberately NOT applied at the press: pressing on
	// an already-selected item has to be able to drag the WHOLE selection, and
	// collapsing the set at the press would make that impossible. Ported from the
	// legacy preview, which likewise selects at the first move or at the release
	// (frontend_old/widgets/OBSBasicPreview.cpp:1630-1632 and :764-766). UI thread.
	bool pressPending = false;      // a left press is open and has not yet been resolved
	bool pressOverSelected = false; // it landed on an item already in the selection
	bool pressCtrl = false;         // Ctrl was held at the press
	bool pressModifier = false;     // Ctrl, Shift or Alt was held at the press
	vec2 pressCanvasPos = {};

	// Unit-quad TRISTRIP vertbuffer, used both for the selection handles and as the
	// rubber band's fill. Created lazily on the render thread and destroyed under a
	// graphics context in Destroy().
	gs_vertbuffer_t *boxBuffer = nullptr;
	// The rotation handle's disc, with the same lifetime as boxBuffer.
	gs_vertbuffer_t *circleBuffer = nullptr;
	// The guide overlays' graphics, with the same lifetime as boxBuffer.
	SafeAreaBuffers safeAreaBuffers = {};
	gs_texture_t *overflowTexture = nullptr;
	// The spacing helpers' labels. Created on the render thread; released in Destroy()
	// once the draw callback is gone.
	SpacingLabels spacingLabels;

	obs_canvas_t *targetCanvas = nullptr; // mirror of the surface's binding for the callback
};

namespace {

// Resolve the scene a surface edits, addref'd (caller releases) or null. Null
// targetCanvas => output channel 0 (the global current scene). A non-null canvas
// => that canvas's current channel-0 scene via the runtime.
obs_source_t *AcquireSurfaceSceneSource(obs_canvas_t *targetCanvas)
{
	if (!targetCanvas) {
		return Transitions::GetProgramScene(); // addref'd; unwraps the ch0 transition; null if unbound
	}
	const char *uuid = obs_canvas_get_uuid(targetCanvas);
	if (!uuid) {
		return nullptr;
	}
	return ObsBootstrap::CanvasRuntime().CurrentScene(uuid); // addref'd; null if unbound
}

// Build the surface's letterbox + base size from its mix. Null targetCanvas =>
// the global obs_get_video_info; otherwise the canvas's own video info. Returns
// false when no video info is available.
bool SurfaceVideoInfo(obs_canvas_t *targetCanvas, obs_video_info &ovi)
{
	if (!targetCanvas) {
		return obs_get_video_info(&ovi);
	}
	return obs_canvas_get_video_info(targetCanvas, &ovi);
}

void EnsureBoxBuffer(PreviewSurface::State *state)
{
	if (state->boxBuffer) {
		return;
	}
	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(1.0f, 0.0f);
	gs_vertex2f(0.0f, 1.0f);
	gs_vertex2f(1.0f, 1.0f);
	state->boxBuffer = gs_render_save();
}

// A unit disc built as the legacy preview builds its circleFill
// (frontend_old/widgets/OBSBasicPreview.cpp:2117-2129): each rim segment followed by the
// disc's bottom point, which as a strip fans the whole disc from that point.
void EnsureCircleBuffer(PreviewSurface::State *state)
{
	if (state->circleBuffer) {
		return;
	}
	constexpr int kSegments = 40;
	gs_render_start(true);
	float angle = 180.0f;
	for (int i = 0; i < kSegments; i++) {
		gs_vertex2f(std::sin(RAD(angle)) / 2.0f + 0.5f, std::cos(RAD(angle)) / 2.0f + 0.5f);
		angle += 360.0f / float(kSegments);
		gs_vertex2f(std::sin(RAD(angle)) / 2.0f + 0.5f, std::cos(RAD(angle)) / 2.0f + 0.5f);
		gs_vertex2f(0.5f, 1.0f);
	}
	state->circleBuffer = gs_render_save();
}

void EnsureSafeAreaBuffers(PreviewSurface::State *state)
{
	for (size_t i = 0; i < state->safeAreaBuffers.size(); i++) {
		if (state->safeAreaBuffers[i]) {
			continue;
		}
		const GuideStrip &strip = kSafeAreaStrips[i];
		gs_render_start(true);
		for (int p = 0; p < strip.count; p++) {
			gs_vertex2f(strip.points[p][0], strip.points[p][1]);
		}
		state->safeAreaBuffers[i] = gs_render_save();
	}
}

void EnsureOverflowTexture(PreviewSurface::State *state)
{
	if (!state->overflowTexture) {
		state->overflowTexture = CreateOverflowTexture();
	}
}

// The addressed canvas's uuid, or null for the Default surface (global path), which is how
// every preview event lets a listener filter to its own canvas.
Bridge::json CanvasField(obs_canvas_t *targetCanvas)
{
	const char *uuid = targetCanvas ? obs_canvas_get_uuid(targetCanvas) : nullptr;
	return uuid ? Bridge::json(std::string(uuid)) : Bridge::json(nullptr);
}

// Emit sceneItem.selected to JS for the surface's scene. An empty `keys` ->
// {scene:null,id:null,ids:[],refs:[],group:null}. Posts to the UI thread internally so it
// is safe from WndProc.
void EmitSelection(obs_canvas_t *targetCanvas, const std::vector<SceneItemKey> &keys)
{
	// `refs` is the whole selection, insertion-ordered, and `ids` its ids; `id` and `group`
	// name its focus (the last member), kept alongside so the single-selection readers on
	// both sides stay exactly as they were when one item is selected. `ids` is lossy once a
	// selection mixes owners: a child can share its id with a top-level item, so it can
	// repeat one and cannot say which item it names. `refs` is the authority.
	const SceneItemKey focus = keys.empty() ? SceneItemKey() : keys.back();
	const int64_t anchor = focus.id;
	const std::vector<int64_t> ids = SceneItems::IdsOf(keys);

	std::string sceneName;
	if (anchor >= 0) {
		obs_source_t *sceneSource = AcquireSurfaceSceneSource(targetCanvas);
		if (sceneSource) {
			const char *n = obs_source_get_name(sceneSource);
			if (n) {
				sceneName = n;
			}
			obs_source_release(sceneSource);
		}
	}
	using Bridge::json;
	json payload = json{
		{"scene", anchor >= 0 && !sceneName.empty() ? json(sceneName) : json(nullptr)},
		{"id", anchor >= 0 ? json(anchor) : json(nullptr)},
		{"ids", ids},
		{"refs", Bridge::SceneItemRefsJson(keys)},
		{"group", anchor >= 0 && !focus.IsTopLevel() ? json(focus.groupUuid) : json(nullptr)},
		// A per-canvas dock filters selection to its own canvas, since scene names collide.
		{"canvas", CanvasField(targetCanvas)},
	};
	Bridge::EmitEvent(EventNames::kSceneItemSelected, payload);
}

// Tell the UI whether the pointer is over this surface. Edge-triggered on both
// sides -- the enter by the arming edge below, the leave by RetractPointerOver's
// guard -- so holding the pointer still, dragging across the surface, or a resize
// burst that hides a surface the pointer was never over all post nothing. The UI needs it because the pan modifier is a plain
// SPACE: the native side samples the key itself, but the focused page element also
// acts on it, so the web view has to suppress that default -- and only while the
// pointer is actually here.
void EmitPointerOver(obs_canvas_t *targetCanvas, int windowId, bool over)
{
	using Bridge::json;
	Bridge::EmitEvent(EventNames::kPreviewPointerOver,
			  json{{"canvas", CanvasField(targetCanvas)}, {"window", windowId}, {"over", over}});
}

// Tell the UI a left or right button went down on this surface, once per press. A
// notification only: the press goes on to select or start a gesture exactly as it would
// without it. The web view needs it because a press here never reaches the page, so focus
// held by a page element would otherwise stay put.
void EmitPointerDown(obs_canvas_t *targetCanvas, int windowId)
{
	using Bridge::json;
	Bridge::EmitEvent(EventNames::kPreviewPointerDown,
			  json{{"canvas", CanvasField(targetCanvas)}, {"window", windowId}});
}

// Emit preview.contextMenu for a right-click at device-px (mx,my) in the overlay
// client. Carries the hit item's id/source/visible/locked (null id => empty area)
// plus the surface's scene name, addressed canvas uuid (null for Default), and the
// originating windowId, so JS filters to the right window+canvas and builds the
// menu without a round-trip. Posts via EmitEvent (broadcast to all browsers).
void EmitContextMenu(obs_canvas_t *targetCanvas, int windowId, obs_scene_t *scene, int64_t id, int mx, int my)
{
	using Bridge::json;

	std::string sceneName;
	if (scene) {
		obs_source_t *ss = obs_scene_get_source(scene); // borrowed
		const char *n = ss ? obs_source_get_name(ss) : nullptr;
		if (n) {
			sceneName = n;
		}
	}

	json sourceField = json(nullptr);
	bool visible = false;
	bool locked = false;
	if (id >= 0 && scene) {
		obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, id); // borrowed
		if (item) {
			obs_source_t *src = obs_sceneitem_get_source(item); // borrowed
			const char *sn = src ? obs_source_get_name(src) : nullptr;
			if (sn) {
				sourceField = json(std::string(sn));
			}
			visible = obs_sceneitem_visible(item);
			locked = obs_sceneitem_locked(item);
		}
	}

	json payload = json{
		{"canvas", CanvasField(targetCanvas)},
		{"window", windowId},
		{"x", mx},
		{"y", my},
		{"id", id >= 0 ? json(id) : json(nullptr)},
		{"scene", sceneName.empty() ? json(nullptr) : json(sceneName)},
		{"source", sourceField},
		{"visible", visible},
		{"locked", locked},
	};
	Bridge::EmitEvent(EventNames::kPreviewContextMenu, payload);
}

// Draw callback: fired by libobs once per frame on the render thread. cx/cy are
// the display (HWND) pixel size. Three phases, in the legacy RenderMain's order
// (frontend_old/widgets/OBSBasic_Preview.cpp:158-199): the overflow fill in a
// screen-px space covering the whole display; the surface's base canvas fitted into
// the display with letterboxing so the composited scene keeps its aspect ratio; then
// that screen-px space again for the safe areas and the editing overlay -- the hovered
// item's outline, the selected items' boxes + handles, the rubber band and the
// spacing helpers, each re-resolved by id from the surface's current scene. `data` is
// the PreviewSurface::State.
void RenderPreview(void *data, uint32_t cx, uint32_t cy)
{
	auto *state = static_cast<PreviewSurface::State *>(data);
	obs_canvas_t *targetCanvas = state->targetCanvas;

	obs_video_info ovi;
	if (!SurfaceVideoInfo(targetCanvas, ovi)) {
		return;
	}

	const float baseCX = float(ovi.base_width);
	const float baseCY = float(ovi.base_height);
	if (baseCX <= 0.0f || baseCY <= 0.0f || cx == 0 || cy == 0) {
		return;
	}

	float scale;
	int drawX;
	int drawY;
	{
		std::lock_guard<std::mutex> lock(state->stateMutex);
		if (state->view.fixed) {
			// Fixed scale: the zoom level sets the scale outright and the pan
			// offsets the centered canvas. No margin here -- it is a fit-mode
			// affordance for reaching a handle that falls outside the canvas, and
			// at a pinned scale the user reaches one by panning instead. Insetting
			// would only shrink a view they asked to be exactly this size.
			scale = FixedZoomScale(state->view.zoomLevel, baseCX, baseCY);
			// Re-clamp every frame: a dock resize can invalidate a pan that was
			// legal at the old size, and nothing else re-validates it.
			ClampScroll(state->view, baseCX * scale, baseCY * scale, int(cx), int(cy));
			drawX = int((float(cx) - baseCX * scale) * 0.5f + state->view.scrollX);
			drawY = int((float(cy) - baseCY * scale) * 0.5f + state->view.scrollY);
		} else {
			// Centered against the FULL surface, not the margin-reduced extent, so
			// the leftover is the margin split evenly across both sides.
			scale = FitScale(int(cx), int(cy), baseCX, baseCY);
			drawX = (int(cx) - int(baseCX * scale)) / 2;
			drawY = (int(cy) - int(baseCY * scale)) / 2;
		}

		state->transform.scale = scale;
		state->transform.drawX = drawX;
		state->transform.drawY = drawY;
		state->transform.baseCX = baseCX;
		state->transform.baseCY = baseCY;
		state->transform.surfaceCX = int(cx);
		state->transform.surfaceCY = int(cy);
	}

	const int drawCX = int(baseCX * scale);
	const int drawCY = int(baseCY * scale);
	// The canvas viewport covers whole pixels, so an overlay measured to a canvas edge has to
	// be mapped by the extent actually covered or it can land a pixel outside it.
	const float coverX = float(drawCX) / baseCX;
	const float coverY = float(drawCY) / baseCY;

	// Cheap gate: skip the scene addref entirely when there is nothing to draw over
	// the frame. The raw ids are enough here -- whether they still belong to the
	// current scene is settled by Resolve() below, once that scene is in hand.
	bool anyEditId;
	bool bandActive;
	bool locked;
	vec2 bandStart, bandCurrent;
	{
		std::lock_guard<std::mutex> lock(state->stateMutex);
		locked = state->view.locked;
		bandActive = state->box.active;
		bandStart = state->box.start;
		bandCurrent = state->box.current;
		anyEditId = !state->selected.Empty() || !state->hovered.Empty() || bandActive;
	}

	// A locked preview draws no overflow and no spacing helpers, as in the legacy
	// DrawOverflow and DrawSpacingHelpers: both describe an edit the lock refuses. The
	// safe areas describe the canvas, not an edit, and draw either way.
	const PreviewOverlays overlays = g_overlays.Load();
	const bool drawOverflow = !locked && overlays.overflow != PreviewOverflowMode::Hidden;
	const bool drawSpacing = !locked && overlays.spacingHelpers;
	// Overflow for every item needs the scene even with nothing selected.
	const bool needScene = anyEditId || (drawOverflow && overlays.overflow == PreviewOverflowMode::Always);

	obs_source_t *sceneSource = needScene ? AcquireSurfaceSceneSource(targetCanvas) : nullptr;
	obs_scene_t *scene = sceneSource ? obs_scene_from_source(sceneSource) : nullptr;
	std::vector<SceneItemKey> selected;
	std::optional<SceneItemKey> hovered;
	if (sceneSource) {
		const char *sceneUuid = obs_source_get_uuid(sceneSource);
		std::lock_guard<std::mutex> lock(state->stateMutex);
		selected = state->selected.Resolve(sceneUuid);
		hovered = state->hovered.Resolve(sceneUuid);
	}
	const bool hoverSelected = hovered && ContainsKey(selected, *hovered);

	gs_viewport_push();
	gs_projection_push();

	// Screen px with the canvas origin at 0,0, over the whole display rather than the
	// canvas viewport, so what falls in the letterbox is drawn instead of being clipped
	// by that viewport. The matrix scale pushed on top of it carries the items'
	// canvas-space box transforms into this space.
	const auto screenSpace = [&]() {
		gs_ortho(float(-drawX), float(cx) - float(drawX), float(-drawY), float(cy) - float(drawY), -100.0f,
			 100.0f);
		gs_reset_viewport();
	};

	if (scene && drawOverflow) {
		screenSpace();
		EnsureOverflowTexture(state);
		if (state->overflowTexture) {
			// Scaled by the covered extent rather than by `scale`, so an item flush with a
			// canvas edge ends on the same pixel as the canvas and leaves no striped sliver.
			gs_matrix_push();
			gs_matrix_scale3f(coverX, coverY, 1.0f);
			OverflowDraw draw{state->overflowTexture, &overlays, &selected};
			obs_scene_enum_items(scene, DrawItemOverflow, &draw);
			gs_matrix_pop();
		}
	}

	gs_ortho(0.0f, baseCX, 0.0f, baseCY, -100.0f, 100.0f);
	gs_set_viewport(drawX, drawY, drawCX, drawCY);

	// Color written, not blended, so the canvas covers the overflow fill beneath it
	// wherever the mix is transparent too. Over the display's black clear the two
	// blend modes produce the same pixels, so without overflow nothing changes.
	if (targetCanvas) {
		obs_render_canvas_texture_src_color_only(targetCanvas);
	} else {
		obs_render_main_texture_src_color_only();
	}

	screenSpace();

	if (overlays.safeAreas) {
		EnsureSafeAreaBuffers(state);
		DrawSafeAreas(state->safeAreaBuffers, float(drawCX), float(drawCY));
	}

	if (scene && (!selected.empty() || hovered || bandActive)) {
		gs_matrix_push();
		gs_matrix_scale3f(scale, scale, 1.0f);

		// Hover first, so the selected item's box and handles draw over it. An
		// eye-off item is skipped: outlining a source the user has hidden would
		// paint a box on apparently-empty canvas. Diverges from the legacy
		// preview, which hover-outlines invisible items.
		//
		// This is the render thread, and the UI thread can remove an item or a whole group
		// while a frame draws, so every item outlined here is held for its draw, and a
		// child's group item with it.
		if (hovered && !hoverSelected) {
			SceneItems::HeldSceneItem held;
			if (SceneItems::AcquireItem(scene, *hovered, held) && SceneItemHasVideo(held.item) &&
			    !obs_sceneitem_locked(held.item) && obs_sceneitem_visible(held.item)) {
				DrawItemBox(held, scale, kHoverColor, nullptr, nullptr);
			}
		}
		// Selection deliberately does NOT take the visible check above: an
		// eye-off source the user selected on purpose still shows its box and
		// handles, which is the only way to see and adjust a hidden item's
		// transform in the preview. Hover is the passive case, selection the
		// asked-for one, so the asymmetry is the intent, not an oversight.
		//
		// Every member gets its own outline, and on an unlocked preview its own
		// handles: with a multi-selection, only the anchor being grabbable would be
		// arbitrary, and ResolveGestureAtPos tests all of them for exactly that
		// reason. A locked preview keeps the outlines, which show what is selected,
		// and drops the handles, which would promise an edit the lock refuses. A group's
		// child is drawn exactly like a top-level item, handles included, and drops them on
		// the same predicate a press would refuse it on (ItemTakesGesture) -- so a child of
		// a bounded group is outlined without them, while a child of a locked group is
		// skipped outright below, the way a locked item is.
		for (const SceneItemKey &key : selected) {
			SceneItems::HeldSceneItem held;
			if (!SceneItems::AcquireItem(scene, key, held) || !SceneItemHasVideo(held.item) ||
			    obs_sceneitem_locked(held.item)) {
				continue;
			}
			if (held.group && obs_sceneitem_locked(held.group)) {
				continue;
			}
			if (locked || !ItemTakesGesture(held.item, held.group)) {
				DrawItemBox(held, scale, kSelectionColor, nullptr, nullptr);
			} else {
				EnsureBoxBuffer(state);
				EnsureCircleBuffer(state);
				DrawItemBox(held, scale, kSelectionColor, state->boxBuffer, state->circleBuffer);
			}
		}

		// The combined bounding box over a multi-item selection. DELIBERATELY
		// NOT PARITY: the legacy preview computes this extent (AddItemBounds)
		// but only ever feeds it to the snapping math, and draws nothing. It is
		// drawn here because a selection whose overall extent you cannot see is
		// worse to work with -- do not "fix" it back to the legacy behaviour.
		// It spans every selected id, locked ones included: it shows what is
		// selected, not what a drag would move. The move gesture's snap box
		// comes from the same helper built over the movers only, so the two
		// boxes differ exactly when the selection holds a locked item.
		if (selected.size() > 1) {
			vec3 tl, br;
			if (SelectionBounds(scene, selected, tl, br)) {
				vec2 tl2, br2;
				vec2_set(&tl2, tl.x, tl.y);
				vec2_set(&br2, br.x, br.y);
				DrawCanvasRect(tl2, br2, scale, kGroupBoxColor, nullptr, nullptr);
			}
		}

		// The rubber band, over everything it is sweeping.
		if (bandActive) {
			vec2 tl2, br2;
			vec2_min(&tl2, &bandStart, &bandCurrent);
			vec2_max(&br2, &bandStart, &bandCurrent);
			EnsureBoxBuffer(state);
			DrawCanvasRect(tl2, br2, scale, kBandBorderColor, &kBandFillColor, state->boxBuffer);
		}

		// Last, as in the legacy RenderMain, which draws them after the rest of the
		// scene editing. They need exactly one selected item, so the gate above covers them.
		if (drawSpacing) {
			// On the covered extent, like the overflow and safe-area passes: a helper runs to
			// a canvas edge, so it has to end where that edge does. The pass around it keeps
			// `scale`, which is what the item boxes, handles and rubber band already draw at.
			gs_matrix_push();
			gs_matrix_identity();
			gs_matrix_scale3f(coverX, coverY, 1.0f);
			DrawSpacingHelpers(state->spacingLabels, scene, selected, scale, baseCX, baseCY);
			gs_matrix_pop();
		}

		gs_matrix_pop();
	}
	if (sceneSource) {
		obs_source_release(sceneSource);
	}

	gs_projection_pop();
	gs_viewport_pop();
}

} // namespace

PreviewSurface::PreviewSurface(HWND host, HINSTANCE instance, obs_canvas_t *targetCanvas, int windowId)
	: state_(new State()),
	  targetCanvas_(targetCanvas),
	  windowId_(windowId),
	  overlay_(host, instance, RenderPreview, state_, this, "preview")
{
	state_->targetCanvas = targetCanvas;
}

PreviewSurface::~PreviewSurface()
{
	Destroy();
	delete state_;
}

// Client px (device) -> canvas coords using the last-rendered letterbox transform.
// Returns false when the transform is not yet known (no frame drawn).
namespace {
bool ClientToCanvas(PreviewSurface::State *state, int mx, int my, vec2 &out)
{
	std::lock_guard<std::mutex> lock(state->stateMutex);
	if (state->transform.scale <= 0.0f) {
		return false;
	}
	out.x = (float(mx) - float(state->transform.drawX)) / state->transform.scale;
	out.y = (float(my) - float(state->transform.drawY)) / state->transform.scale;
	return true;
}

float CurrentScale(PreviewSurface::State *state)
{
	std::lock_guard<std::mutex> lock(state->stateMutex);
	return state->transform.scale;
}

// The Scale-submenu commands. The token vocabulary lives next to the behaviour it
// names, as one row per command, so adding a command is a row here plus a case --
// the bridge never branches on the string and cannot drift from this list.
enum class ViewAction { ZoomIn, ZoomOut, ScaleToWindow, ScaleToCanvas };

struct ViewActionEntry {
	const char *token;
	ViewAction action;
};

constexpr ViewActionEntry kViewActions[] = {
	{"zoomIn", ViewAction::ZoomIn},
	{"zoomOut", ViewAction::ZoomOut},
	{"scaleToWindow", ViewAction::ScaleToWindow},
	{"scaleToCanvas", ViewAction::ScaleToCanvas},
};

bool ViewActionFromToken(const std::string &token, ViewAction &out)
{
	for (const ViewActionEntry &e : kViewActions) {
		if (token == e.token) {
			out = e.action;
			return true;
		}
	}
	return false;
}
} // namespace

void PreviewSurface::OnLeftDown(int mx, int my)
{
	// A press with a gesture already in flight. SetCapture on an HWND that already holds
	// the capture sends no WM_CAPTURECHANGED, so none of the drag-end routes has run and
	// the assignments below would overwrite the live gesture's recorded BEFORE state --
	// losing the undo for a move that has already been applied to the item. End it
	// through the same path every other terminator uses. `mode` is the in-flight
	// predicate the whole file keys off (OnMouseMove's first line, FinishDrag's
	// `dragged`), and FinishDrag is idempotent, so this costs nothing when idle.
	//
	// Ahead of every early return below, so the drag state cannot outlive a press that
	// bails on an unrendered surface or an unresolvable scene either.
	if (state_->drag.mode != DragMode::None) {
		FinishDrag();
	}

	EndPan();
	// A band still open when a new press arrives is abandoned, not committed: the
	// press that would have committed it never reached the release path.
	CancelBox();
	state_->pressPending = false;

	if (HWND hwnd = overlay_.Hwnd()) {
		SetCapture(hwnd);
	}

	// Space + left-drag pans instead of touching an item, and only once zoomed:
	// fit mode has nothing to pan, since the whole canvas is already in view. The
	// modifier is sampled here and nowhere else in the gesture, which is what makes
	// pressing space mid-drag harmless -- an item gesture already in flight is never
	// converted to a pan, it just finishes as the move, resize or rotation it started as.
	// Ahead of the transform check below so a pan can still be started (and refused
	// by PanBy) on a surface that has not drawn yet.
	if (PanModifierHeld() && FixedScaling()) {
		state_->panning = true;
		state_->panFrom = POINT{mx, my};
		SetCursorShape(IDC_SIZEALL);
		return;
	}

	vec2 canvasPos;
	if (!ClientToCanvas(state_, mx, my, canvasPos)) {
		return;
	}

	obs_source_t *sceneSource = AcquireSurfaceSceneSource(targetCanvas_);
	if (!sceneSource) {
		return;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);

	const char *sceneUuid = obs_source_get_uuid(sceneSource);
	std::vector<SceneItemKey> selected;
	{
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		selected = state_->selected.Resolve(sceneUuid);
	}

	// A locked preview starts no editing gesture, so it offers no handles: passing an
	// empty selection skips the handle test. Selection stays live on purpose -- the
	// lock is about editing geometry, not about choosing what the docks show -- so a
	// click still selects and a drag still rubber-bands, but nothing moves or resizes.
	const bool locked = Locked();
	const Modifiers mods = ReadModifiers();
	const bool ctrlHeld = mods.ctrl;
	const bool modifierHeld = mods.Any();
	static const std::vector<SceneItemKey> kNoSelection;
	const GestureAtPos gesture = ResolveGestureAtPos(scene, locked ? kNoSelection : selected, canvasPos,
							 CurrentScale(state_), !ctrlHeld);

	// A handle of a selected item begins a resize or a rotation, and that is the one
	// decision a press still makes immediately: it names its target outright, so there is
	// nothing left for the first move to resolve.
	if (gesture.handle != ItemHandle::None) {
		const bool rotating = gesture.handle == ItemHandle::Rot;
		if (rotating) {
			BeginRotate(state_->drag, sceneSource, gesture.item, gesture.groupItem, canvasPos);
		} else {
			BeginResize(state_->drag, sceneSource, gesture.item, gesture.groupItem, gesture.handle,
				    canvasPos);
		}
		// Captured under the re-fit hold the gesture just took, so the BEFORE state reads
		// the group as it stands rather than as a re-fit triggered by the read would
		// leave it. Empty when the gesture refused to open, which leaves nothing to undo.
		if (state_->drag.mode != DragMode::None) {
			state_->drag.undoBefore = CaptureDragUndoState(targetCanvas_, sceneSource,
								       std::vector<obs_sceneitem_t *>{gesture.item});
		}
		HostLog(std::string("[preview] ") + (rotating ? "rotate" : "resize") +
			" start id=" + std::to_string(obs_sceneitem_get_id(gesture.item)) +
			(gesture.groupItem ? " (child)" : "") + " handle=" + std::to_string(uint32_t(gesture.handle)));
		obs_source_release(sceneSource);
		return;
	}

	// Everything else waits. The press only records what it landed on; whether it
	// becomes a move, a rubber band or a plain selection change is settled by the first
	// mouse-move (OnMouseMove) or by the release (OnLeftUp). See State::pressPending.
	state_->pressPending = true;
	state_->pressCtrl = ctrlHeld;
	state_->pressModifier = modifierHeld;
	state_->pressCanvasPos = canvasPos;
	// A locked preview must not move anything, so a press on a selected item there is
	// treated as empty space and starts a band instead of a drag.
	state_->pressOverSelected = !locked && SelectedItemAtPos(scene, selected, canvasPos);

	// The band's press-time snapshot, taken on every press so that a modifier pressed
	// DURING the sweep still has a set to combine against. See BoxState::preSelection.
	state_->box.preSelection = selected;
	state_->box.preSceneUuid = sceneUuid ? sceneUuid : std::string();

	HostLog("[preview] press canvas=(" + std::to_string(int(canvasPos.x)) + "," + std::to_string(int(canvasPos.y)) +
		") overSelected=" + (state_->pressOverSelected ? "1" : "0"));

	obs_source_release(sceneSource);
}

// Resolve this surface's scene and apply the deferred press against it. The release
// path has no scene in hand, unlike the first-move path which already acquired one for
// the gesture it is about to start.
void PreviewSurface::ApplyPressClickOnCurrentScene()
{
	obs_source_t *sceneSource = AcquireSurfaceSceneSource(targetCanvas_); // addref'd
	if (!sceneSource) {
		return;
	}
	// A release with no movement is a click, never a band, so nothing is pending.
	ApplyPressClick(sceneSource, obs_scene_from_source(sceneSource), false);
	obs_source_release(sceneSource);
}

// Apply the deferred press as a selection change: a plain press selects the item under
// it (or clears on empty canvas), a Ctrl press toggles it and does nothing at all on a
// miss. The hit-test is re-run HERE rather than reused from the press, because the
// click-through cycle reads the live selection -- which is exactly what makes a second
// click in the same spot land one step further down the stack.
//
// `bandPending` says this call is the first move of a press that is about to become a
// rubber band, which changes only what an empty hit does: see below.
void PreviewSurface::ApplyPressClick(obs_source_t *sceneSource, obs_scene_t *scene, bool bandPending)
{
	const char *sceneUuid = obs_source_get_uuid(sceneSource);
	std::vector<int64_t> selectedIds;
	{
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		selectedIds = SceneItems::TopLevelIds(state_->selected.Resolve(sceneUuid));
	}

	const int64_t hitId = HitTestItemId(scene, state_->pressCanvasPos, state_->pressCtrl ? nullptr : &selectedIds);

	if (hitId < 0 && (state_->pressCtrl || (bandPending && state_->pressModifier))) {
		// Nothing under the press, and a reason not to clear.
		//
		// Ctrl: a modifier click on empty canvas leaves the selection alone, matching
		// the legacy DoCtrlSelect's early return (OBSBasicPreview.cpp:731-733).
		//
		// An additive band (Shift/Ctrl/Alt held at the press): clearing here and
		// restoring from the snapshot at the release would push TWO selection changes
		// at the docks for one gesture -- an empty one the moment the sweep starts,
		// then the real one -- which reads as a flicker. FinishBox sets the selection
		// authoritatively, so this call has nothing to contribute.
		return;
	}

	std::vector<SceneItemKey> next;
	{
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		if (state_->pressCtrl) {
			state_->selected.Toggle(sceneSource, SceneItemKey(hitId));
		} else {
			state_->selected.SetOne(sceneSource, SceneItemKey(hitId));
		}
		next = state_->selected.keys;
	}
	// Every writer runs on the UI thread, so the set cannot change between the two locks.
	const std::vector<SceneItemKey> kept = WithoutStaleChildren(scene, next);
	if (kept.size() != next.size()) {
		next = kept;
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		state_->selected.Set(sceneSource, next);
	}
	SelectSet(scene, next);
	EmitSelection(targetCanvas_, next);
	HostLog("[preview] click hit id=" + std::to_string(hitId) + " selection=" + std::to_string(next.size()));
}

namespace {

// Below EPSILON a snap component counts as "unset". Mirrors the legacy preview's
// EPSILON; used so a later candidate only overrides an already-claimed axis when
// it is strictly closer.
constexpr float kSnapEpsilon = 0.0001f;

// Accumulates source-edge snapping for the dragged box [tl,br] against every
// OTHER scene item, seeded with the canvas-edge/center offset. Ported from the
// legacy OffsetData/GetSourceSnapOffset.
struct SnapAccum {
	float clampDist;
	// The TOP-LEVEL ids the gesture excludes from snapping -- a child contributes its
	// group's id, since that is the item this enumeration sees (see
	// DragState::snapExcludeIds). All of them are excluded, not just the anchor: with a
	// multi-selection dragged as a unit, snapping members to each other would fight the
	// gesture, since their relative positions never change.
	const std::vector<int64_t> *draggedIds;
	vec3 tl, br, offset;
};

bool SourceSnapCb(obs_scene_t * /* scene */, obs_sceneitem_t *item, void *param)
{
	auto *data = static_cast<SnapAccum *>(param);

	if (data->draggedIds && std::find(data->draggedIds->begin(), data->draggedIds->end(),
					  obs_sceneitem_get_id(item)) != data->draggedIds->end()) {
		return true;
	}
	if (obs_sceneitem_locked(item) || !obs_sceneitem_visible(item) || !SceneItemHasVideo(item)) {
		return true;
	}

	matrix4 boxTransform;
	if (!SceneItems::ItemBoxToCanvas(item, boxTransform)) {
		return true;
	}

	const std::array<vec3, 4> t = BoxCorners(boxTransform);

	vec3 tl, br;
	vec3_copy(&tl, &t[0]);
	vec3_copy(&br, &t[0]);
	for (const vec3 &v : t) {
		vec3_min(&tl, &tl, &v);
		vec3_max(&br, &br, &v);
	}

	// Snap the dragged box's edges to this item's edges. l/r select which edge of
	// each box is compared; x/y select the axis (the other axis must overlap).
#define EDGE_SNAP(l, r, x, y)                                                                                 \
	do {                                                                                                  \
		double dist = fabsf(l.x - data->r.x);                                                         \
		if (dist < data->clampDist && fabsf(data->offset.x) < kSnapEpsilon && data->tl.y < br.y &&    \
		    data->br.y > tl.y && (fabsf(data->offset.x) > dist || data->offset.x < kSnapEpsilon)) {    \
			data->offset.x = l.x - data->r.x;                                                     \
		}                                                                                             \
	} while (false)

	EDGE_SNAP(tl, br, x, y);
	EDGE_SNAP(tl, br, y, x);
	EDGE_SNAP(br, tl, x, y);
	EDGE_SNAP(br, tl, y, x);
#undef EDGE_SNAP

	return true;
}

// Returns the snap adjustment (canvas space) to add to the proposed move offset
// so the dragged bounding box [tl,br] aligns to canvas edges, canvas center
// lines, or other items' edges. Ports the legacy GetSnapOffset (edges/center)
// and SnapItemMovement (source) combine logic. snapDistance is already in canvas
// space here, so unlike the legacy we do NOT divide by the surface scale.
vec3 CanvasSnapOffset(const GeneralSettings &gs, obs_scene_t *scene, const std::vector<int64_t> &draggedIds,
		      const vec3 &tl, const vec3 &br, float baseW, float baseH)
{
	vec3 clampOffset;
	vec3_zero(&clampOffset);

	const float clampDist = float(gs.snapDistance);
	const bool screenSnap = gs.snapToEdge;
	const bool centerSnap = gs.snapToCenter;
	const float centerX = br.x - (br.x - tl.x) / 2.0f;
	const float centerY = br.y - (br.y - tl.y) / 2.0f;

	// Left canvas edge.
	if (screenSnap && fabsf(tl.x) < clampDist) {
		clampOffset.x = -tl.x;
	}
	// Right canvas edge.
	if (screenSnap && fabsf(clampOffset.x) < kSnapEpsilon && fabsf(baseW - br.x) < clampDist) {
		clampOffset.x = baseW - br.x;
	}
	// Horizontal center.
	if (centerSnap && fabsf(baseW - (br.x - tl.x)) > clampDist && fabsf(baseW / 2.0f - centerX) < clampDist) {
		clampOffset.x = baseW / 2.0f - centerX;
	}

	// Top canvas edge.
	if (screenSnap && fabsf(tl.y) < clampDist) {
		clampOffset.y = -tl.y;
	}
	// Bottom canvas edge.
	if (screenSnap && fabsf(clampOffset.y) < kSnapEpsilon && fabsf(baseH - br.y) < clampDist) {
		clampOffset.y = baseH - br.y;
	}
	// Vertical center.
	if (centerSnap && fabsf(baseH - (br.y - tl.y)) > clampDist && fabsf(baseH / 2.0f - centerY) < clampDist) {
		clampOffset.y = baseH / 2.0f - centerY;
	}

	if (!gs.snapToSource) {
		return clampOffset;
	}

	SnapAccum acc;
	acc.clampDist = clampDist;
	acc.draggedIds = &draggedIds;
	vec3_copy(&acc.tl, &tl);
	vec3_copy(&acc.br, &br);
	vec3_copy(&acc.offset, &clampOffset);

	obs_scene_enum_items(scene, SourceSnapCb, &acc);

	if (fabsf(acc.offset.x) > kSnapEpsilon || fabsf(acc.offset.y) > kSnapEpsilon) {
		return acc.offset;
	}
	return clampOffset;
}

} // namespace

void PreviewSurface::SetCursorShape(const wchar_t *idc)
{
	if (state_->cursorShape == idc) {
		return;
	}
	state_->cursorShape = idc;
	SetCursor(LoadCursorW(nullptr, idc));
}

void PreviewSurface::ClearHoverItem()
{
	std::lock_guard<std::mutex> lock(state_->stateMutex);
	state_->hovered.Clear();
}

void PreviewSurface::ClearHover()
{
	ClearHoverItem();
	// Reset the remembered shape but do NOT call SetCursor: the callers run when the
	// pointer is no longer over this surface (it left, or the surface was hidden
	// under it), and SetCursor is process-global, so applying here would stomp the
	// cursor of whatever window the pointer is actually over. The overlay's own
	// WM_SETCURSOR applies this shape again the next time the pointer is here.
	state_->cursorShape = IDC_ARROW;
}

void PreviewSurface::UpdateHover(int mx, int my)
{
	// Both of these short-circuit the hit-test because the cursor must never
	// advertise a gesture other than the one a press would start. With space held
	// over a zoomed preview that gesture is the pan; with the preview locked there
	// is no gesture at all, so neither a resize cursor nor the hover outline -- each
	// of which reads as "press to grab here" -- may be shown.
	if (PanModifierHeld() && FixedScaling()) {
		ClearHoverItem();
		SetCursorShape(IDC_SIZEALL);
		return;
	}
	if (Locked()) {
		ClearHoverItem();
		SetCursorShape(IDC_ARROW);
		return;
	}

	// One tail for every outcome: a surface with no frame yet or no scene bound has
	// nothing to hover and takes the plain arrow, same as empty canvas does.
	const wchar_t *cursor = IDC_ARROW;
	int64_t hoveredId = -1;
	obs_source_t *sceneSource = nullptr;

	vec2 canvasPos;
	if (ClientToCanvas(state_, mx, my, canvasPos)) {
		sceneSource = AcquireSurfaceSceneSource(targetCanvas_);
	}
	if (sceneSource) {
		obs_scene_t *scene = obs_scene_from_source(sceneSource);
		const char *sceneUuid = obs_source_get_uuid(sceneSource);
		std::vector<SceneItemKey> selected;
		{
			std::lock_guard<std::mutex> lock(state_->stateMutex);
			selected = state_->selected.Resolve(sceneUuid);
		}
		// The cycle is armed here too, so the hover outline previews what a click
		// would actually select rather than the item on top of it.
		const GestureAtPos gesture = ResolveGestureAtPos(scene, selected, canvasPos, CurrentScale(state_));

		if (gesture.handle != ItemHandle::None) {
			cursor = CursorForHandle(gesture.item, gesture.groupItem, gesture.handle);
		} else if (gesture.bodyId >= 0) {
			cursor = IDC_SIZEALL;
			hoveredId = gesture.bodyId;
		}
	}

	{
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		state_->hovered.Set(sceneSource, SceneItemKey(hoveredId));
	}
	if (sceneSource) {
		obs_source_release(sceneSource);
	}
	SetCursorShape(cursor);
}

void PreviewSurface::OnMouseMove(int mx, int my)
{
	// Ahead of the item-drag branch: a pan and an item gesture are mutually
	// exclusive (OnLeftDown returns before it can start one after starting the
	// other), and the pan is tracked in raw client px, so it needs neither the
	// transform nor a scene.
	if (state_->panning) {
		PanBy(mx - state_->panFrom.x, my - state_->panFrom.y);
		state_->panFrom = POINT{mx, my};
		return;
	}
	// Nothing in flight and no press waiting on a decision: this is a plain hover.
	if (state_->drag.mode == DragMode::None && !state_->box.active && !state_->pressPending) {
		UpdateHover(mx, my);
		return;
	}
	vec2 canvasPos;
	if (!ClientToCanvas(state_, mx, my, canvasPos)) {
		return;
	}

	obs_source_t *sceneSource = AcquireSurfaceSceneSource(targetCanvas_);
	if (!sceneSource) {
		return;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);

	// The first movement after a press decides what the press actually was. A press on
	// an item already in the selection drags the WHOLE selection and leaves the set
	// alone; anything else applies its click first (which may have just selected the
	// item under the pointer) and then drags that, or -- on empty canvas -- starts a
	// rubber band.
	if (state_->pressPending) {
		state_->pressPending = false;
		bool overSelected = state_->pressOverSelected;
		if (!overSelected) {
			ApplyPressClick(sceneSource, scene, true);
			const char *uuid = obs_source_get_uuid(sceneSource);
			std::vector<SceneItemKey> keys;
			{
				std::lock_guard<std::mutex> lock(state_->stateMutex);
				keys = state_->selected.Resolve(uuid);
			}
			overSelected = !Locked() && SelectedItemAtPos(scene, keys, state_->pressCanvasPos);
		}
		if (overSelected) {
			BeginMove(sceneSource, scene);
		} else {
			BeginBox();
		}
	}

	if (state_->box.active) {
		{
			std::lock_guard<std::mutex> lock(state_->stateMutex);
			state_->box.current = canvasPos;
		}
		// Outside the lock: releasing a source can run libobs teardown, and this
		// file's rule is that the state mutex is never held across a libobs call.
		obs_source_release(sceneSource);
		return;
	}

	if (state_->drag.mode == DragMode::None) {
		obs_source_release(sceneSource);
		return;
	}

	// A scene switch since mousedown resolves to nothing and leaves the rest of the
	// gesture inert, rather than applying it to the new scene's item of that id.
	const char *dragSceneUuid = obs_source_get_uuid(sceneSource);
	obs_sceneitem_t *item = state_->drag.id.Find(scene, dragSceneUuid);
	if (item && !obs_sceneitem_locked(item) && !AnyHeldGroupLocked(state_->drag)) {
		state_->drag.moved = true;
		if (state_->drag.mode == DragMode::Move) {
			float offX = canvasPos.x - state_->drag.startCanvasPos.x;
			float offY = canvasPos.y - state_->drag.startCanvasPos.y;

			// Snap the move to canvas edges/center and other items' edges, unless
			// disabled in General settings or temporarily suppressed with Ctrl.
			// The snap runs ONCE, against the selection's combined box -- a
			// multi-item drag moves as a unit, so a per-item snap would pull the
			// members apart.
			const GeneralSettings &gs = ObsBootstrap::General();
			const bool ctrlHeld = ReadModifiers().ctrl;
			obs_video_info ovi;
			if (gs.snapEnabled && !ctrlHeld && state_->drag.hasStartBounds &&
			    SurfaceVideoInfo(targetCanvas_, ovi) && ovi.base_width && ovi.base_height) {
				// The start box translated by the proposed offset. pos is a pure
				// translation, so this stays correct for rotated/bounds-scaled
				// items without re-deriving any box transform.
				vec3 tl, br;
				vec3_copy(&tl, &state_->drag.startBoundsTl);
				vec3_copy(&br, &state_->drag.startBoundsBr);
				tl.x += offX;
				br.x += offX;
				tl.y += offY;
				br.y += offY;

				vec3 snap = CanvasSnapOffset(gs, scene, state_->drag.snapExcludeIds, tl, br,
							     float(ovi.base_width), float(ovi.base_height));
				offX += snap.x;
				offY += snap.y;
			}

			// Absolute from each member's start position, never incremental: a
			// snapped offset re-applied against the item's live position would
			// accumulate the rounding below over the gesture and drift.
			//
			// The offset is the one canvas offset for the whole gesture, carried into the
			// space each member's position is written in -- the canvas's for a top-level
			// item, its group's for a child -- so a mixed selection travels as one
			// formation whatever its members' groups do.
			vec2 canvasOffset;
			vec2_set(&canvasOffset, offX, offY);
			for (const DragItem &d : state_->drag.items) {
				obs_sceneitem_t *member = d.id.Find(scene, dragSceneUuid);
				vec2 memberOffset;
				if (!member || obs_sceneitem_locked(member) ||
				    !SceneItems::CanvasToOwnerVector(d.ownerToCanvas, canvasOffset, memberOffset)) {
					continue;
				}
				vec2 newPos;
				newPos.x = std::round(d.startItemPos.x + memberOffset.x);
				newPos.y = std::round(d.startItemPos.y + memberOffset.y);
				obs_sceneitem_set_pos(member, &newPos);
			}
		} else if (state_->drag.mode == DragMode::Resize) {
			// Every other gesture runs in the anchor's own space, so the pointer crosses
			// into it once here and the math below is the same one a top-level item takes.
			const vec2 ownerPos = ToOwnerSpace(state_->drag, canvasPos);
			const GeneralSettings &gs = ObsBootstrap::General();
			const Modifiers mods = ReadModifiers();
			if (mods.alt) {
				CropItem(state_->drag, item, ownerPos);
			} else {
				obs_video_info ovi;
				float snapBaseW = 0.0f, snapBaseH = 0.0f;
				if (gs.snapEnabled && !mods.ctrl && SurfaceVideoInfo(targetCanvas_, ovi) &&
				    ovi.base_width && ovi.base_height) {
					snapBaseW = float(ovi.base_width);
					snapBaseH = float(ovi.base_height);
				}
				StretchItem(state_->drag, item, ownerPos, scene, gs, snapBaseW, snapBaseH, mods);
			}
		} else if (state_->drag.mode == DragMode::Rotate) {
			RotateItem(state_->drag, item, ToOwnerSpace(state_->drag, canvasPos), ReadModifiers());
		}
	}
	obs_source_release(sceneSource);
}

// Start moving the whole current selection. Every member records the position it
// started at, so each frame writes an absolute position rather than accumulating
// deltas, and the one batch undo payload is captured here -- before any geometry math
// runs -- for exactly the set the gesture will write to.
void PreviewSurface::BeginMove(obs_source_t *sceneSource, obs_scene_t *scene)
{
	const char *sceneUuid = obs_source_get_uuid(sceneSource);
	std::vector<SceneItemKey> keys;
	{
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		keys = state_->selected.Resolve(sceneUuid);
	}

	state_->drag.Reset();

	// A group and one of its own children both selected: the group wins, and the child is
	// dropped. The group's move already carries its children across the canvas, so writing the
	// child's own position on top of that would move it twice. Matches sceneItems.nudge.
	const std::vector<int64_t> selectedTopLevelIds = SceneItems::TopLevelIds(keys);
	const auto ownGroupAlsoSelected = [&](const GestureTarget &target) {
		return target.groupItem &&
		       std::find(selectedTopLevelIds.begin(), selectedTopLevelIds.end(),
				 obs_sceneitem_get_id(target.groupItem)) != selectedTopLevelIds.end();
	};

	// Resolved first and in full, because the holds below have to be in place before any
	// member's start position is read, and this pass is the only thing that says which members
	// there are. A per-member refusal excludes that member from the gesture without cancelling
	// it: dragging a selection that happens to contain one locked source, or one child of a
	// bounded group, should move the rest rather than refuse (see ItemTakesGesture).
	std::vector<GestureTarget> targets;
	std::vector<SceneItemKey> targetKeys;
	for (const SceneItemKey &key : keys) {
		const GestureTarget target = ResolveGestureTarget(scene, key);
		if (!target.editable || ownGroupAlsoSelected(target)) {
			continue;
		}
		targets.push_back(target);
		targetKeys.push_back(key);
	}
	if (targets.empty()) {
		return;
	}
	// Held before a single start position is recorded, and for the whole gesture: our move
	// writes an absolute position from each member's start, and libobs's re-fit shifts every
	// sibling, so a re-fit landing between the read and the first frame -- or between two
	// frames -- would move those starts out from under the drag. The pending updates are
	// consumed under the hold for the same reason, so the start bounds read below cannot leave
	// a re-fit flagged behind it.
	for (const GestureTarget &target : targets) {
		SceneItems::HoldGroup(state_->drag.groupHolds, target.groupItem);
	}
	for (const GestureTarget &target : targets) {
		SceneItems::ApplyPendingChildUpdate(target.item);
	}

	std::vector<obs_sceneitem_t *> items;
	std::vector<SceneItemKey> movingKeys;
	for (size_t i = 0; i < targets.size(); ++i) {
		if (!AddDragItem(state_->drag, sceneSource, targets[i].item, targets[i].groupItem)) {
			continue;
		}
		items.push_back(targets[i].item);
		movingKeys.push_back(targetKeys[i]);
	}
	if (state_->drag.items.empty()) {
		// Reset rather than return: the holds above are already taken, and leaving them on
		// a gesture that never opens would freeze the group's re-fitting until the next one.
		state_->drag.Reset();
		return;
	}

	state_->drag.mode = DragMode::Move;
	state_->drag.moved = false;
	state_->drag.id = state_->drag.items.back().id;
	// The PRESS position, not the position of the move that triggered this: the offset
	// every frame applies is measured from where the gesture began.
	state_->drag.startCanvasPos = state_->pressCanvasPos;
	state_->drag.hasStartBounds =
		SelectionBounds(scene, movingKeys, state_->drag.startBoundsTl, state_->drag.startBoundsBr);
	state_->drag.undoBefore = CaptureDragUndoState(targetCanvas_, sceneSource, items);
}

// Start a rubber band from the press position.
void PreviewSurface::BeginBox()
{
	std::lock_guard<std::mutex> lock(state_->stateMutex);
	state_->box.active = true;
	state_->box.start = state_->pressCanvasPos;
	state_->box.current = state_->pressCanvasPos;
}

// Drop a band without committing it. Idempotent, like every other terminator here, so
// the capture-ending paths can call it unconditionally.
void PreviewSurface::CancelBox()
{
	std::lock_guard<std::mutex> lock(state_->stateMutex);
	state_->box.active = false;
	state_->box.preSelection.clear();
	state_->box.preSceneUuid.clear();
}

// Commit a rubber band into the selection. Returns whether a band was in flight, so a
// button-up can tell which gesture it just ended.
//
// The four modifier outcomes: none replaces, Shift adds, Ctrl XORs against the
// press-time snapshot, Alt subtracts (legacy reference:
// frontend_old/widgets/OBSBasicPreview.cpp:768-793). The snapshot was taken at the
// PRESS, unconditionally, and the modifier is read HERE at the release -- so a modifier
// pressed part-way through the sweep still combines against the set the gesture
// started from, which is the point of splitting the two.
bool PreviewSurface::FinishBox()
{
	bool active;
	vec2 start, current;
	std::vector<SceneItemKey> preSelection;
	std::string preSceneUuid;
	{
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		active = state_->box.active;
		start = state_->box.start;
		current = state_->box.current;
		preSelection = state_->box.preSelection;
		preSceneUuid = state_->box.preSceneUuid;
		state_->box.active = false;
		state_->box.preSelection.clear();
		state_->box.preSceneUuid.clear();
	}
	if (!active) {
		return false;
	}

	obs_source_t *sceneSource = AcquireSurfaceSceneSource(targetCanvas_); // addref'd
	if (!sceneSource) {
		return true;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	const char *sceneUuid = obs_source_get_uuid(sceneSource);

	// Shift gets no branch of its own below: additive IS the default pass over a
	// non-empty snapshot, and mods.Any() is what decides whether that snapshot is kept.
	const Modifiers mods = ReadModifiers();

	const std::vector<int64_t> boxed = BoxItems(scene, start, current);

	// The snapshot only counts for the scene it was taken in: a scene switch mid-band
	// would otherwise re-select ids belonging to items that are no longer on screen.
	std::vector<SceneItemKey> next;
	if (mods.Any() && sceneUuid && preSceneUuid == sceneUuid) {
		next = WithoutStaleChildren(scene, preSelection);
	}

	for (const int64_t id : boxed) {
		const SceneItemKey key(id);
		const auto at = std::find(next.begin(), next.end(), key);
		if (mods.alt) {
			if (at != next.end()) {
				next.erase(at);
			}
		} else if (mods.ctrl) {
			if (at != next.end()) {
				next.erase(at);
			} else {
				next.push_back(key);
			}
		} else if (at == next.end()) {
			next.push_back(key);
		}
	}

	{
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		state_->selected.Set(sceneSource, next);
	}
	SelectSet(scene, next);
	EmitSelection(targetCanvas_, next);
	HostLog("[preview] box select boxed=" + std::to_string(boxed.size()) +
		" selection=" + std::to_string(next.size()));

	obs_source_release(sceneSource);
	return true;
}

bool PreviewSurface::FinishDrag()
{
	const bool dragged = state_->drag.mode != DragMode::None;
	const bool moved = state_->drag.moved;
	const bool resized = state_->drag.mode == DragMode::Resize;
	const SceneItemRef draggedRef = state_->drag.id;
	std::vector<DragItem> draggedItems;
	draggedItems.swap(state_->drag.items);
	std::string undoBefore;
	undoBefore.swap(state_->drag.undoBefore);
	// The re-fit holds the gesture took, moved out of the drag state so Reset() below does not
	// end them: the AFTER capture has to read the group as the gesture left it, and ending a
	// hold flags the re-fit that would move it. This local owns them for the rest of the
	// function and releases them on EVERY path out of it, the early-return-free shape being
	// what makes that true -- a leaked hold would freeze that group's re-fitting for the rest
	// of the session. This is also the one drag-end path, reached from the button-up, a
	// right-click, a lost capture, a lock applied mid-drag, a press that interrupts a live
	// gesture and the surface's own teardown, so there is no other path to leak from.
	std::vector<SceneItems::GroupResizeDeferral> holds;
	holds.swap(state_->drag.groupHolds);
	if (dragged) {
		HostLog("[preview] drag end id=" + std::to_string(draggedRef.key.id) + " items=" +
			std::to_string(draggedItems.size()) + " groupHolds=" + std::to_string(holds.size()));
	}
	// Everything the rest of this function needs is copied or swapped out above, so the
	// gesture clears through the one resetter rather than a second field-by-field list
	// that can drift from it.
	state_->drag.Reset();

	// Every route that ends a gesture lands here, and a gesture can end with the
	// pointer anywhere: a button-up outside the preview arrives only through the
	// capture, and a capture lost to Alt-Tab or a foreground change carries no
	// pointer position at all. Drop the hover outline -- the next move recomputes it
	// -- or it paints on the just-dragged item the moment a bridge-driven selection
	// moves elsewhere. The cursor shape is deliberately left alone (see ClearHover).
	ClearHoverItem();

	// A braidcast_overlay source renders its page at the size in its settings and the
	// item then scales that bitmap, so a resize alone would magnify pixels rather than
	// re-lay-out the widget. Make the page follow the box instead. Resize only: a move or
	// a rotation leaves the box's size alone, while an Alt-crop changes how much page the
	// box needs, so both stretch and crop drags (which share DragMode::Resize) commit.
	if (resized && moved) {
		obs_source_t *sceneSource = AcquireSurfaceSceneSource(targetCanvas_); // addref'd
		if (sceneSource) {
			// Same scene scoping as the drag itself: a switch since mousedown must
			// not commit this overlay's layout onto the new scene's same-id item.
			obs_sceneitem_t *item =
				draggedRef.Find(obs_scene_from_source(sceneSource), obs_source_get_uuid(sceneSource));
			obs_source_t *itemSource = item ? obs_sceneitem_get_source(item) : nullptr;
			if (Overlay::IsOverlaySource(itemSource)) {
				// Saves unconditionally, including on an additional-canvas
				// surface, unlike the Default-only save in OnLeftUp: the pin and
				// the page size are collection state, and one scene-collection
				// file holds every canvas's scenes, so skipping it there would
				// simply lose them.
				Overlay::CommitForSource(itemSource);
			}
			obs_source_release(sceneSource);
		}
	}

	// One undo step for the whole gesture, however many mouse-move frames it took.
	// Recorded here rather than in OnLeftUp because this is the one drag-end path: a
	// right-click or a lost capture also ends a gesture that already moved the item, and
	// that has to be reversible too. Below the overlay commit so the AFTER state carries
	// the box it left.
	//
	// `moved` only means the gesture got as far as trying (see its declaration), so it
	// is the cheap gate that skips the AFTER capture entirely for a select-click.
	// Whether an entry is actually pushed is decided by RecordItemTransformsUndo, which
	// compares the two payloads -- a drag mode can refuse every frame and leave the
	// geometry untouched while `moved` is true. Over a multi-item selection that
	// comparison is ALL-OR-NOTHING: if any one member moved, the batch is recorded for
	// every member, including the ones whose geometry is unchanged. That is deliberate
	// -- the gesture is one action over one selection, and it is what keeps the whole
	// drag a single Ctrl+Z -- but it does differ from the single-item behaviour a
	// reader may be carrying over. The empty check covers a press that resolved no
	// item; a press cannot inherit a live gesture's payload, because OnLeftDown ends
	// any gesture still in flight before it records its own.
	if (moved && !undoBefore.empty()) {
		obs_source_t *dragScene = AcquireDragScene(draggedRef); // addref'd
		obs_scene_t *scene = dragScene ? obs_scene_from_source(dragScene) : nullptr;
		// Re-resolved against the scene the gesture STARTED in, which is what
		// AcquireDragScene hands back -- the same scoping the drag itself used.
		std::vector<obs_sceneitem_t *> items =
			scene ? ResolveDragItems(scene, draggedItems, obs_source_get_uuid(dragScene))
			      : std::vector<obs_sceneitem_t *>{};
		if (!items.empty()) {
			// AFTER is read while the holds are still in hand, so it describes the
			// group as the gesture left it rather than as the re-fit their release
			// flags would; the release then happens on the way out of this function.
			const std::string undoAfter = CaptureDragUndoState(targetCanvas_, dragScene, items);
			holds.clear();
			Bridge::RecordItemTransformsUndo(items.data(), items.size(), undoBefore, undoAfter);
		}
		if (dragScene) {
			obs_source_release(dragScene);
		}
	}
	return moved;
}

void PreviewSurface::OnLeftUp()
{
	// Finish the gesture BEFORE releasing the capture. ReleaseCapture() sends
	// WM_CAPTURECHANGED synchronously to this same overlay HWND, which routes straight
	// back into CancelDrag() -- so any drag state read after the release has already
	// been cleared, and every branch keyed on it is dead.
	// A pan ends here and goes no further: it has no item, so none of the save or
	// undo tail below applies to it.
	if (EndPan()) {
		ReleaseCapture();
		return;
	}

	// A press that never moved is a plain click, and this is where it is applied --
	// which is also what makes a second click in the same spot step one item further
	// down the z-stack, since the cycle reads the selection the first click left.
	if (state_->pressPending) {
		state_->pressPending = false;
		ApplyPressClickOnCurrentScene();
	}

	// A band commits before the item gesture is finished: the two are mutually
	// exclusive (OnMouseMove starts one or the other, never both), and FinishDrag is a
	// no-op for a band.
	FinishBox();

	const bool moved = FinishDrag();
	ReleaseCapture();

	// A move/resize on the Default surface mutates the global scene's layout;
	// persist it once at drag-end (never per-mousemove, and only when the drag
	// actually changed geometry — a bare select-click moves nothing). Additional-
	// canvas surfaces are persisted per-canvas later.
	if (moved && targetCanvas_ == nullptr) {
		SceneCollection::Save();
	}
}

void PreviewSurface::OnRightUp(int mx, int my)
{
	// A right-click during a pan cancels the pan and stops there. The gesture in
	// flight was a pan, so the press that ends it is a cancellation, not a new
	// selection -- running the rest would let a pan change which item is selected
	// and open a menu the user was not asking for.
	//
	// Deliberately not the legacy behaviour, which cancels on the right PRESS and
	// opens its menu on the release, so it never swallows a menu. This surface acts
	// only on the release (the press is only announced as preview.pointerDown), so the
	// two cannot both happen here. Exact parity would cancel on WM_RBUTTONDOWN and let
	// this release run through unchanged.
	if (EndPan()) {
		return;
	}

	vec2 canvasPos;
	if (!ClientToCanvas(state_, mx, my, canvasPos)) {
		return;
	}
	obs_source_t *sceneSource = AcquireSurfaceSceneSource(targetCanvas_); // addref'd
	if (!sceneSource) {
		return;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	const char *sceneUuid = obs_source_get_uuid(sceneSource);
	std::vector<SceneItemKey> selected;
	{
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		selected = state_->selected.Resolve(sceneUuid);
	}

	// No cycle on a right-click: the menu must describe what is visibly under the
	// cursor, and walking the stack would open it on something else.
	const int64_t hitId = HitTestItemId(scene, canvasPos);

	// Right-click selects (or clears) the item under the cursor before the menu opens,
	// matching OBS and keeping the selection box + dock lists in sync -- EXCEPT when
	// the click lands inside an existing multi-selection, which is left intact because
	// collapsing it would silently discard work the user did to build it, and opening a
	// menu is not a request to change what is selected.
	const bool insideSelection = hitId >= 0 && selected.size() > 1 && ContainsKey(selected, SceneItemKey(hitId));
	if (!insideSelection) {
		{
			std::lock_guard<std::mutex> lock(state_->stateMutex);
			state_->selected.SetOne(sceneSource, SceneItemKey(hitId));
			selected = state_->selected.keys;
		}
		SelectSet(scene, selected);
	}
	// A right-click abandons an open band rather than committing it: the gesture the
	// user ended was the menu, not the selection sweep.
	CancelBox();
	state_->pressPending = false;
	FinishDrag();
	if (!insideSelection) {
		EmitSelection(targetCanvas_, selected);
	}
	EmitContextMenu(targetCanvas_, windowId_, scene, hitId, mx, my);

	obs_source_release(sceneSource);
}

void PreviewSurface::CancelDrag()
{
	// A stolen capture still ends a gesture that already mutated the item, so it takes
	// the same finish as a button-up -- otherwise a resize interrupted mid-drag would
	// leave an overlay's page sized for a box it no longer has. Idempotent, which is
	// what makes the WM_CAPTURECHANGED that OnLeftUp's own ReleaseCapture() sends a
	// harmless second call.
	FinishDrag();
	EndPan();
	// A lost capture abandons a band rather than committing it: no release happened,
	// so there is no modifier state to read and no user intent to honour.
	CancelBox();
	state_->pressPending = false;
}

// The pan gesture's terminator, and the reason it is not folded into FinishDrag:
// that function's whole body is item state -- the undo payload it hands back, the
// collection save its return value gates, the hover outline it drops -- and a pan
// has none of those. Kept idempotent for the same reason FinishDrag is, so every
// capture-ending path can call both unconditionally. Returns whether a pan was in
// flight, so a button-up can tell which gesture it just ended.
bool PreviewSurface::EndPan()
{
	if (!state_->panning) {
		return false;
	}
	state_->panning = false;
	SetCursorShape(IDC_ARROW);
	return true;
}

// The same end, for the paths where the pointer is no longer over this surface --
// the overlay was hidden under it, or the surface is being destroyed. Those must
// not call SetCursor, which is process-global and would repaint the cursor for
// whatever the pointer is over now; they reset the remembered shape alone so the
// next WM_SETCURSOR over a reshown surface starts from the arrow. Exactly the
// split, and the reason for it, that ClearHover already has against
// ClearHoverItem.
// Retract the pointer-over flag, gated on this surface having actually claimed it.
// Every hide and teardown path calls it unconditionally, so without the guard a
// dock-resize burst would post a retraction for every surface on every hide. The
// flag doubles as the leave-tracking state, which is exactly the "is the pointer
// here" bit, so there is no second flag to keep in step.
void PreviewSurface::RetractPointerOver()
{
	if (!state_->mouseTracked) {
		return;
	}
	state_->mouseTracked = false;
	EmitPointerOver(targetCanvas_, windowId_, false);
}

void PreviewSurface::EndPanOffSurface()
{
	state_->panning = false;
	state_->cursorShape = IDC_ARROW;
}

bool PreviewSurface::Locked()
{
	std::lock_guard<std::mutex> lock(state_->stateMutex);
	return state_->view.locked;
}

bool PreviewSurface::FixedScaling()
{
	std::lock_guard<std::mutex> lock(state_->stateMutex);
	return state_->view.fixed;
}

// Zoom by `levelDelta` notches, keeping the canvas point currently under the
// client pixel (px, py) under it afterwards. That anchor is the whole trick, and
// getting it wrong is the usual way wheel zoom feels broken: read the canvas point
// from the transform the last frame published, then pick the pan that puts that
// same canvas point back on that same screen pixel at the new scale. Solving
//     px == canvasX * newScale + drawX,  drawX == (cx - baseCX * newScale) / 2 + scrollX
// for scrollX gives the assignment below. Leaving fit mode uses the same formula
// -- the old transform is read the same way whichever mode produced it -- so the
// first notch grows the view around the pointer instead of jumping to the centered
// fixed-mode origin.
void PreviewSurface::ZoomAt(int px, int py, int levelDelta)
{
	std::lock_guard<std::mutex> lock(state_->stateMutex);
	const PreviewTransform &t = state_->transform;
	if (t.scale <= 0.0f || t.baseCX <= 0.0f || t.baseCY <= 0.0f) {
		return; // no frame yet, so there is no point on screen to anchor to
	}

	// Rebuild the whole "before" side from the view, never from the last published
	// frame, because the two disagree whenever a command has landed since that frame
	// was drawn -- two wheel notches inside one frame, which a high-resolution wheel
	// or a fast flick produces routinely. Mixing them pairs a fresh level with a
	// stale scale and throws the anchor by half the base times the scale delta.
	//
	// The origin is rebuilt in float for a second, independent reason: the transform
	// stores it as the int the viewport is actually set to -- which is right, since a
	// float there would let hit-testing address a half-pixel the frame was never
	// drawn at -- but feeding that truncation back in makes every notch inherit the
	// last one's rounding.
	//
	// Fit mode has neither a scale nor an origin of its own to rebuild from, so it
	// reads the frame. That is sound because the first notch leaves fit mode, so
	// nothing it rounds can compound.
	const float oldScale = PendingScale(state_->view, t);
	const float drawXf = state_->view.fixed
				     ? (float(t.surfaceCX) - t.baseCX * oldScale) * 0.5f + state_->view.scrollX
				     : float(t.drawX);
	const float drawYf = state_->view.fixed
				     ? (float(t.surfaceCY) - t.baseCY * oldScale) * 0.5f + state_->view.scrollY
				     : float(t.drawY);

	const float canvasX = (float(px) - drawXf) / oldScale;
	const float canvasY = (float(py) - drawYf) / oldScale;

	const int oldLevel = state_->view.fixed ? state_->view.zoomLevel : ZoomLevelForAmount(oldScale);
	const int newLevel = std::clamp(oldLevel + levelDelta, -kZoomMaxLevel, kZoomMaxLevel);
	const float newScale = FixedZoomScale(newLevel, t.baseCX, t.baseCY);

	state_->view.fixed = true;
	state_->view.zoomLevel = newLevel;
	state_->view.scrollX = float(px) - canvasX * newScale - (float(t.surfaceCX) - t.baseCX * newScale) * 0.5f;
	state_->view.scrollY = float(py) - canvasY * newScale - (float(t.surfaceCY) - t.baseCY * newScale) * 0.5f;
	// The bound wins over the anchor: at the edge of the pannable range the point
	// under the cursor does drift, which is the correct trade -- holding it there
	// would mean scrolling the canvas out of the surface.
	ClampScroll(state_->view, t.baseCX * newScale, t.baseCY * newScale, t.surfaceCX, t.surfaceCY);
}

void PreviewSurface::PanBy(int dx, int dy)
{
	std::lock_guard<std::mutex> lock(state_->stateMutex);
	const PreviewTransform &t = state_->transform;
	if (!state_->view.fixed || t.scale <= 0.0f) {
		return;
	}
	// Same pending-scale rule as ZoomAt, for a smaller reason. The draw callback
	// re-clamps authoritatively every frame, so a bound computed from a stale scale
	// is corrected almost immediately -- but the clamp WRITES BACK, so a bound that
	// was briefly too tight has already trimmed the offset and the next frame cannot
	// restore it, which reads as the edge of the pan sticking for a frame after a
	// zoom. The correct scale is one call away and shared with ZoomAt, so deriving
	// it here costs nothing.
	const float scale = PendingScale(state_->view, t);
	state_->view.scrollX += float(dx);
	state_->view.scrollY += float(dy);
	ClampScroll(state_->view, t.baseCX * scale, t.baseCY * scale, t.surfaceCX, t.surfaceCY);
}

bool PreviewSurface::ApplyViewAction(const std::string &token)
{
	ViewAction action;
	if (!ViewActionFromToken(token, action)) {
		return false;
	}

	switch (action) {
	case ViewAction::ZoomIn:
	case ViewAction::ZoomOut: {
		// A menu command carries no pointer, so the step is anchored at the surface
		// center -- the one point the user is certainly looking at, and the only
		// choice that keeps repeated zoom-ins from wandering.
		int px;
		int py;
		{
			std::lock_guard<std::mutex> lock(state_->stateMutex);
			px = state_->transform.surfaceCX / 2;
			py = state_->transform.surfaceCY / 2;
		}
		ZoomAt(px, py, action == ViewAction::ZoomIn ? 1 : -1);
		return true;
	}
	case ViewAction::ScaleToWindow: {
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		state_->view.ResetZoom();
		return true;
	}
	case ViewAction::ScaleToCanvas: {
		// 1:1 -- one canvas pixel per device pixel, centered with no pan.
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		state_->view.ResetZoom();
		state_->view.fixed = true;
		return true;
	}
	}
	return false;
}

void PreviewSurface::SetLocked(bool locked)
{
	{
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		state_->view.locked = locked;
	}
	// A lock applied mid-gesture ends it where it stands rather than letting the
	// held button keep editing an item the preview now refuses to edit.
	FinishDrag();
}

PreviewViewState PreviewSurface::GetView()
{
	const PreviewOverlays overlays = g_overlays.Load();
	std::lock_guard<std::mutex> lock(state_->stateMutex);
	// The scale the next frame will draw at, not the one the last frame did. A menu
	// is built right after the command that opened it, so reporting the published
	// scale would answer with the percentage the preview is leaving.
	const int zoomPercent = int(std::lround(PendingScale(state_->view, state_->transform) * 100.0f));
	return PreviewViewState{state_->view.fixed, zoomPercent, state_->view.locked, overlays};
}

void PreviewSurface::SetRect(int x, int y, int cx, int cy)
{
	overlay_.SetRect(x, y, cx, cy);
}

void PreviewSurface::Hide()
{
	overlay_.Hide();
}

void PreviewSurface::OnOverlayHidden()
{
	// Every route that hides the overlay lands here, including the two inside
	// OverlaySurface::SetRect that never reach this class's own Hide(). The surface
	// can be shown again without the pointer ever moving over it, which would redraw
	// a hover outline for wherever the cursor last was; drop it, and re-arm
	// leave-tracking so the next move over the reshown surface starts clean.
	RetractPointerOver();
	ClearHover();
	EndPanOffSurface();
}

void PreviewSurface::Destroy()
{
	// Closing the surface under a held button ends the gesture, and it is the one end
	// no message can deliver: OverlaySurface::Destroy clears the HWND's GWLP_USERDATA
	// before its DestroyWindow, so nothing the destruction sends can route back into this
	// surface's WndProc -- CancelDrag included. Ahead of overlay_.Destroy() so the gesture
	// finishes while the surface is whole; every Destroy path is already the
	// window-owning thread, because the DestroyWindow it reaches must be.
	FinishDrag();
	EndPanOffSurface();
	// A surface torn down with the pointer still over it must retract the flag, or
	// the web view keeps suppressing the pan key's default action for a surface
	// that no longer exists. WM_MOUSELEAVE cannot deliver this -- the HWND is about
	// to stop routing messages to this object at all.
	RetractPointerOver();

	// The display dies first (OverlaySurface removes the draw callback), so nothing
	// the render thread reads outlives it -- including the graphics and labels below.
	overlay_.Destroy();
	std::vector<gs_vertbuffer_t **> buffers = {&state_->boxBuffer, &state_->circleBuffer};
	for (gs_vertbuffer_t *&buffer : state_->safeAreaBuffers) {
		buffers.push_back(&buffer);
	}
	const bool anyBuffer = std::any_of(buffers.begin(), buffers.end(),
					   [](gs_vertbuffer_t **buffer) { return *buffer != nullptr; });
	if (anyBuffer || state_->overflowTexture) {
		obs_enter_graphics();
		for (gs_vertbuffer_t **buffer : buffers) {
			gs_vertexbuffer_destroy(*buffer);
			*buffer = nullptr;
		}
		if (state_->overflowTexture) {
			gs_texture_destroy(state_->overflowTexture);
			state_->overflowTexture = nullptr;
		}
		obs_leave_graphics();
	}
	for (SpacingLabel &label : state_->spacingLabels) {
		obs_source_release(label.source);
		label = SpacingLabel{};
	}
}

std::optional<std::vector<SceneItemKey>> PreviewSurface::SelectFromBridge(const std::string &scene,
									  const std::vector<SceneItemKey> &keys)
{
	obs_source_t *sceneSource = AcquireSurfaceSceneSource(targetCanvas_);
	if (!sceneSource) {
		return std::nullopt;
	}
	// Ignore a foreign scene name to keep "preview shows the surface's scene" intact.
	if (!scene.empty()) {
		const char *n = obs_source_get_name(sceneSource);
		if (!n || scene != n) {
			obs_source_release(sceneSource);
			return std::nullopt;
		}
	}
	obs_scene_t *sc = obs_scene_from_source(sceneSource);
	const std::vector<SceneItemKey> applied = WithoutStaleChildren(sc, keys);

	// A press still open when the docks drive a selection has nothing left to decide:
	// applying its click afterwards would overwrite what the dock just asked for.
	CancelBox();
	state_->pressPending = false;

	{
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		state_->selected.Set(sceneSource, applied);
	}
	SelectSet(sc, applied);
	obs_source_release(sceneSource);

	EmitSelection(targetCanvas_, applied);
	return applied;
}

int64_t PreviewSurface::HitTestForTest(float canvasX, float canvasY)
{
	obs_source_t *sceneSource = AcquireSurfaceSceneSource(targetCanvas_);
	if (!sceneSource) {
		return -1;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	vec2 pos;
	vec2_set(&pos, canvasX, canvasY);
	const int64_t id = HitTestItemId(scene, pos);
	obs_source_release(sceneSource);
	return id;
}

namespace {

// The grab point of one scripted gesture, in canvas space, plus what the press is expected to
// resolve to and which modifier it needs.
struct TestGrab {
	vec2 pos = {};
	ItemHandle handle = ItemHandle::None; // None means the body, i.e. a move
	Modifiers mods;
};

bool ResolveTestGrab(obs_sceneitem_t *item, const matrix4 &box, PreviewTestGesture gesture, TestGrab &out)
{
	const auto at = [&box](float u, float v) {
		const vec3 point = GetTransformedPos(u, v, box);
		vec2 result;
		vec2_set(&result, point.x, point.y);
		return result;
	};
	out.mods = Modifiers{};
	switch (gesture) {
	case PreviewTestGesture::Move:
		out.pos = at(0.5f, 0.5f);
		out.handle = ItemHandle::None;
		out.mods.ctrl = true;
		return true;
	case PreviewTestGesture::MoveSnapping:
		out.pos = at(0.5f, 0.5f);
		out.handle = ItemHandle::None;
		return true;
	case PreviewTestGesture::ResizeBottomRight:
		out.pos = at(1.0f, 1.0f);
		out.handle = ItemHandle::BottomRight;
		out.mods.ctrl = true;
		// Shift for free aspect, so the grabbed corner tracks the pointer on both axes
		// instead of along a constrained line. That is what lets a case say where the
		// corner should end up rather than only that the item changed size.
		out.mods.shift = true;
		return true;
	case PreviewTestGesture::CropLeft:
		out.pos = at(0.0f, 0.5f);
		out.handle = ItemHandle::CenterLeft;
		out.mods.alt = true;
		return true;
	case PreviewTestGesture::Rotate: {
		float edgeY = 0.0f;
		vec2 standoff;
		RotHandleStandoff(item, box, edgeY, standoff);
		// The surface is seeded at 1:1 (see DragForTest), so a screen-px stand-off is a
		// canvas-px one and the disc centre is exactly where FindHandleAtPos looks.
		out.pos = at(0.5f, edgeY);
		out.pos.x += standoff.x * kRotHandleDistance;
		out.pos.y += standoff.y * kRotHandleDistance;
		out.handle = ItemHandle::Rot;
		out.mods.ctrl = true;
		return true;
	}
	}
	return false;
}

} // namespace

bool PreviewSurface::DragForTest(const SceneItemKey &key, PreviewTestGesture gesture, float dx, float dy, vec2 *outGrab)
{
	obs_source_t *sceneSource = AcquireSurfaceSceneSource(targetCanvas_); // addref'd
	if (!sceneSource) {
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	// `editable` is checked HERE rather than left to the press, because a press on an item
	// that takes no gesture is not a no-op: with no drill-in, the body hit-test answers with
	// the top-level item under the pointer, so the press would select and drag that instead.
	// A scripted gesture asks about one item, so a refusal is reported as one and nothing is
	// pressed.
	const GestureTarget target = ResolveGestureTarget(scene, key);
	matrix4 box;
	TestGrab grab;
	const bool resolved = target.editable && SceneItems::ItemBoxThroughGroup(target.item, target.groupItem, box) &&
			      ResolveTestGrab(target.item, box, gesture, grab);
	obs_source_release(sceneSource);
	if (!resolved) {
		return false;
	}

	// 1:1 and unshifted, so a canvas coordinate IS a client pixel for the whole gesture. The
	// next drawn frame overwrites this; a surface the UI never sized never draws one.
	{
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		state_->transform.scale = 1.0f;
		state_->transform.drawX = 0;
		state_->transform.drawY = 0;
	}

	const ScopedTestModifiers held(grab.mods);
	// The ROUNDED press point, which is where the pointer went: OnLeftDown takes client pixels,
	// so a case measuring against grab.pos itself would be off by up to half a pixel per axis.
	const int fromX = int(std::lround(grab.pos.x));
	const int fromY = int(std::lround(grab.pos.y));
	if (outGrab) {
		vec2_set(outGrab, float(fromX), float(fromY));
	}
	OnLeftDown(fromX, fromY);
	const bool grabbedBody = gesture == PreviewTestGesture::Move || gesture == PreviewTestGesture::MoveSnapping;
	const bool pressedRight = grabbedBody ? (state_->pressPending && state_->pressOverSelected)
					      : (state_->drag.mode != DragMode::None &&
						 state_->drag.handle == grab.handle && state_->drag.id.key == key);
	OnMouseMove(int(std::lround(grab.pos.x + dx)), int(std::lround(grab.pos.y + dy)));
	const bool startedRight =
		pressedRight && state_->drag.id.key == key &&
		(grabbedBody ? state_->drag.mode == DragMode::Move : state_->drag.mode != DragMode::None) &&
		state_->drag.moved;
	OnLeftUp();
	return startedRight;
}

int64_t PreviewSurface::SelectedIdForTest()
{
	// The recorded ANCHOR id, not scene-resolved: the isolation self-test asserts what
	// this surface holds, and it holds it against its own scene.
	std::lock_guard<std::mutex> lock(state_->stateMutex);
	return state_->selected.Anchor().id;
}

bool PreviewSurface::OnVideoReset()
{
	if (!overlay_.HasDisplay()) {
		// No display yet (UI never sized this surface). The next SetRect creates it
		// fresh against the new base resolution; nothing to re-validate.
		HostLog("[preview] OnVideoReset: no display yet (will create lazily)");
		return false;
	}

	// The base resolution changed, so the cached letterbox transform is stale.
	// Reset it; RenderPreview recomputes it from the surface's video info on the
	// next frame. ClientToCanvas treats scale<=0 as "no frame yet" and ignores
	// mouse input until that recompute lands, avoiding a one-frame mis-mapped drag.
	// The zoom goes with it. A level and a pan describe a view of a canvas at the
	// old base resolution; at a new one they name a framing the user never chose, so
	// the predictable state is the one the surface opens in. The lock is a separate
	// choice and survives.
	{
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		state_->transform = PreviewTransform{};
		state_->view.ResetZoom();
	}

	// Nudge a redraw at the current size so the new mix is presented promptly.
	obs_display_update_color_space(static_cast<obs_display_t *>(overlay_.Display()));
	HostLog("[preview] OnVideoReset: display alive, letterbox transform invalidated");
	return true;
}

bool PreviewSurface::OnOverlayMessage(UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg) {
	case WM_LBUTTONDOWN:
		EmitPointerDown(targetCanvas_, windowId_);
		OnLeftDown(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
		return true;
	case WM_RBUTTONDOWN:
		// The right-click itself acts on release (OnRightUp), so the press is only announced
		// and otherwise left to the default handling it had before.
		EmitPointerDown(targetCanvas_, windowId_);
		return false;
	case WM_MOUSEMOVE: {
		// TME_LEAVE is one-shot, so it is re-armed after each WM_MOUSELEAVE;
		// without it the hover outline and cursor keep the last in-surface value
		// once the pointer moves away.
		HWND hwnd = overlay_.Hwnd();
		if (!state_->mouseTracked && hwnd) {
			TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
			state_->mouseTracked = TrackMouseEvent(&tme) != FALSE;
			if (state_->mouseTracked) {
				// The arming edge IS the enter edge: mouseTracked is false
				// exactly until the first move after the pointer arrives, and
				// WM_MOUSELEAVE is what clears it again.
				EmitPointerOver(targetCanvas_, windowId_, true);
			}
		}
		OnMouseMove(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
		return true;
	}
	case WM_MOUSELEAVE:
		RetractPointerOver();
		// Capture does not suppress leave notifications, so a drag that pulls the
		// pointer past the surface edge lands here every move. A gesture keeps the
		// cursor it started with for its whole duration, and its outline is the
		// selection box, not the hover one -- so only a leave with no drag clears.
		if (state_->drag.mode == DragMode::None) {
			ClearHover();
		}
		return true;
	case WM_SETCURSOR:
		// Client area only; every other hit-test code keeps DefWindowProc's
		// handling. Reporting this handled is what makes OverlayWndProc answer TRUE,
		// which per WM_SETCURSOR's contract halts the processing that would
		// otherwise restore the window class's arrow cursor on every mouse move.
		if (LOWORD(lparam) != HTCLIENT) {
			return false;
		}
		SetCursor(LoadCursorW(nullptr, state_->cursorShape));
		return true;
	case WM_LBUTTONUP:
		OnLeftUp();
		return true;
	case WM_RBUTTONUP:
		OnRightUp(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
		return true;
	case WM_MOUSEWHEEL: {
		// Unlike every button and move message, WM_MOUSEWHEEL carries the pointer in
		// SCREEN coordinates, so it has to be mapped into this HWND's client space
		// before it can anchor anything.
		HWND hwnd = overlay_.Hwnd();
		POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
		if (!hwnd || !ScreenToClient(hwnd, &pt)) {
			return false;
		}
		// Sign only, magnitude discarded -- one notch per message, matching the
		// legacy preview (frontend_old/widgets/OBSBasicPreview.cpp:553-567), which
		// tests the sign of angleDelta().y() and steps one level either way. If an
		// accumulator is ever wanted it has to land here AND in the web view's
		// forwarder in the same change, or the two routings give different zoom for
		// the same physical gesture.
		const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
		if (delta != 0) {
			ZoomAt(pt.x, pt.y, delta > 0 ? 1 : -1);
		}
		return true;
	}
	case WM_CAPTURECHANGED:
		CancelDrag();
		return true;
	default:
		return false;
	}
}

// One managed surface: its (windowId, canvas uuid) key ("" uuid for the Default
// surface) and the owned PreviewSurface. unique_ptr so the surface keeps a stable
// address while the list grows (the WndProc maps an HWND to a surface by pointer).
struct ManagedSurface {
	int windowId;     // 0 = main window
	std::string uuid; // "" => Default surface
	std::unique_ptr<PreviewSurface> surface;
};

// pimpl body: the surface list + per-window host HWND map, kept out of the header
// so it stays free of <vector>/<memory> and the PreviewSurface definition.
struct PreviewManager::Impl {
	std::vector<ManagedSurface> surfaces;
	std::vector<std::pair<int, HWND>> windowHosts; // windowId -> host HWND
};

namespace {

// Destroy one managed surface and erase it, then balance its CanvasRuntime
// preview ref. RemovePreview runs after Destroy() removed the draw callback
// (teardown order): "" balances the Default consumer count, a uuid the canvas
// mix ref. Returns the iterator following the erased element. Shared by
// the single-surface, per-window, and per-canvas reap paths (NOT DestroyAll,
// which deliberately skips RemovePreview at shutdown).
std::vector<ManagedSurface>::iterator ReapSurface(std::vector<ManagedSurface> &surfaces,
						  std::vector<ManagedSurface>::iterator it)
{
	it->surface->Destroy();
	const std::string closedKey = it->uuid; // "" => Default surface
	it = surfaces.erase(it);
	ObsBootstrap::CanvasRuntime().RemovePreview(closedKey);
	return it;
}

} // namespace

PreviewManager::PreviewManager(HWND host, HINSTANCE instance) : impl_(new Impl()), host_(host), instance_(instance) {}

PreviewManager::~PreviewManager()
{
	DestroyAll();
	delete impl_;
}

void PreviewManager::RegisterWindow(int windowId, HWND host)
{
	for (auto &h : impl_->windowHosts) {
		if (h.first == windowId) {
			h.second = host;
			return;
		}
	}
	impl_->windowHosts.push_back({windowId, host});
	HostLog("[preview] RegisterWindow id=" + std::to_string(windowId));
}

void PreviewManager::UnregisterWindow(int windowId)
{
	for (auto it = impl_->windowHosts.begin(); it != impl_->windowHosts.end(); ++it) {
		if (it->first == windowId) {
			impl_->windowHosts.erase(it);
			HostLog("[preview] UnregisterWindow id=" + std::to_string(windowId));
			return;
		}
	}
}

PreviewSurface *PreviewManager::FindSurface(int windowId, const std::string &canvasUuid)
{
	const std::string key = IsDefaultCanvasUuid(canvasUuid) ? std::string() : canvasUuid;
	for (ManagedSurface &s : impl_->surfaces) {
		if (s.windowId == windowId && s.uuid == key) {
			return s.surface.get();
		}
	}
	return nullptr;
}

PreviewSurface *PreviewManager::SurfaceFor(int windowId, const std::string &canvasUuid)
{
	if (PreviewSurface *existing = FindSurface(windowId, canvasUuid)) {
		return existing;
	}

	const bool isDefault = IsDefaultCanvasUuid(canvasUuid);
	const std::string key = isDefault ? std::string() : canvasUuid;

	// Resolve the host HWND for this window: a registered detached window's host,
	// else the constructor's host_ (windowId 0 / main, or an unregistered window).
	HWND host = host_;
	for (const auto &h : impl_->windowHosts) {
		if (h.first == windowId) {
			host = h.second;
			break;
		}
	}

	// First use of this (window, canvas): bind the surface to the right mix.
	// Default => null targetCanvas (global mix); otherwise activate the canvas
	// (build its mix if it was inert) then resolve it. An unknown non-Default uuid
	// gets no surface.
	obs_canvas_t *targetCanvas = nullptr;
	if (!isDefault) {
		ObsBootstrap::CanvasRuntime().AddPreview(canvasUuid); // build mix before render
		targetCanvas = ObsBootstrap::CanvasRuntime().Find(canvasUuid);
		if (!targetCanvas) {
			ObsBootstrap::CanvasRuntime().RemovePreview(canvasUuid); // balance: no surface created
			return nullptr;
		}
	} else {
		// The Default canvas has no Entry and no mix to build; the count this keeps
		// is what holds the main composite ungated while the surface is open,
		// balanced by the RemovePreview in teardown.
		ObsBootstrap::CanvasRuntime().AddPreview(canvasUuid);
	}

	impl_->surfaces.push_back(ManagedSurface{
		windowId, key, std::make_unique<PreviewSurface>(host, instance_, targetCanvas, windowId)});
	HostLog("[preview] surface created window=" + std::to_string(windowId) +
		(key.empty() ? " canvas=Default" : " canvas=" + key));
	return impl_->surfaces.back().surface.get();
}

void PreviewManager::SetRect(int windowId, const std::string &canvasUuid, int x, int y, int cx, int cy)
{
	PreviewSurface *surface = SurfaceFor(windowId, canvasUuid);
	if (surface) {
		surface->SetRect(x, y, cx, cy);
	}
}

void PreviewManager::Hide(int windowId, const std::string &canvasUuid)
{
	const std::string key = IsDefaultCanvasUuid(canvasUuid) ? std::string() : canvasUuid;
	for (ManagedSurface &s : impl_->surfaces) {
		if (s.windowId == windowId && s.uuid == key) {
			s.surface->Hide();
			return;
		}
	}
}

void PreviewManager::Destroy(int windowId, const std::string &canvasUuid)
{
	const std::string key = IsDefaultCanvasUuid(canvasUuid) ? std::string() : canvasUuid;
	for (auto it = impl_->surfaces.begin(); it != impl_->surfaces.end(); ++it) {
		if (it->windowId == windowId && it->uuid == key) {
			ReapSurface(impl_->surfaces, it);
			return;
		}
	}
}

void PreviewManager::DestroyForCanvas(const std::string &canvasUuid)
{
	// Canvas-removal reap: the same canvas can have a surface on the main window
	// AND on detached windows (each minted its own windowId), so sweep every
	// windowId for this uuid -- a per-window Destroy would leave a detached
	// surface's obs_display rendering the mix after it is freed.
	const std::string key = IsDefaultCanvasUuid(canvasUuid) ? std::string() : canvasUuid;
	int destroyed = 0;
	for (auto it = impl_->surfaces.begin(); it != impl_->surfaces.end();) {
		if (it->uuid == key) {
			it = ReapSurface(impl_->surfaces, it);
			++destroyed;
		} else {
			++it;
		}
	}
	HostLog("[preview] DestroyForCanvas(" + (key.empty() ? std::string("Default") : key) + ") destroyed " +
		std::to_string(destroyed) + " surface(s)");
}

void PreviewManager::DestroyAll()
{
	// Teardown path (~PreviewManager / shutdown before CanvasRuntime::ClearAll):
	// preview counts are NOT decremented because ClearAll drops every mix
	// regardless, and calling back into a tearing-down CanvasRuntime risks
	// ordering hazards.
	for (ManagedSurface &s : impl_->surfaces) {
		s.surface->Destroy();
	}
	impl_->surfaces.clear();
}

void PreviewManager::DestroyWindow(int windowId)
{
	// Tear down + erase every surface owned by windowId. Destroy() kills the
	// obs_display before this returns, so it runs (per the UAF rule) before the
	// window's browser closes and before its canvas mix is freed.
	int destroyed = 0;
	for (auto it = impl_->surfaces.begin(); it != impl_->surfaces.end();) {
		if (it->windowId == windowId) {
			it = ReapSurface(impl_->surfaces, it);
			++destroyed;
		} else {
			++it;
		}
	}
	HostLog("[preview] DestroyWindow(windowId=" + std::to_string(windowId) + ") destroyed " +
		std::to_string(destroyed) + " surface(s)");
}

void PreviewManager::OnVideoResetAll()
{
	for (ManagedSurface &s : impl_->surfaces) {
		s.surface->OnVideoReset();
	}
}

void PreviewManager::OnVideoResetForCanvas(const std::string &canvasUuid)
{
	// Sweeps every windowId for this uuid, the same shape as DestroyForCanvas and
	// for the same reason: one canvas can own a surface on the main window and on
	// each detached one, and a per-window reset would leave the others pinned at a
	// zoom chosen for the old base resolution.
	const std::string key = IsDefaultCanvasUuid(canvasUuid) ? std::string() : canvasUuid;
	for (ManagedSurface &s : impl_->surfaces) {
		if (s.uuid == key) {
			s.surface->OnVideoReset();
		}
	}
}

namespace Preview {

void SetInstance(PreviewManager *pm)
{
	g_instance = pm;
}

PreviewManager *Instance()
{
	return g_instance;
}

std::optional<std::vector<SceneItemKey>> SelectFromBridge(const std::string &canvas, const std::string &scene,
							  const std::vector<SceneItemKey> &keys, int windowId)
{
	if (!g_instance) {
		return std::nullopt;
	}
	PreviewSurface *surface = g_instance->SurfaceFor(windowId, canvas);
	if (!surface) {
		return std::nullopt;
	}
	return surface->SelectFromBridge(scene, keys);
}

int64_t HitTestForTest(const std::string &canvas, float canvasX, float canvasY, int windowId)
{
	if (!g_instance) {
		return -1;
	}
	PreviewSurface *surface = g_instance->SurfaceFor(windowId, canvas);
	if (!surface) {
		return -1;
	}
	return surface->HitTestForTest(canvasX, canvasY);
}

bool DragForTest(const std::string &canvas, const SceneItemKey &key, PreviewTestGesture gesture, float dx, float dy,
		 int windowId, vec2 *outGrab)
{
	if (!g_instance) {
		return false;
	}
	// FindSurface, not SurfaceFor: a scripted gesture has to run on the surface the test's
	// own preview.select already stood up, never stand one up as a side effect.
	PreviewSurface *surface = g_instance->FindSurface(windowId, canvas);
	return surface && surface->DragForTest(key, gesture, dx, dy, outGrab);
}

void OnVideoReset()
{
	if (g_instance) {
		g_instance->OnVideoResetAll();
	}
}

void OnCanvasVideoReset(const std::string &canvasUuid)
{
	if (g_instance) {
		g_instance->OnVideoResetForCanvas(canvasUuid);
	}
}

bool ApplyViewAction(const std::string &canvas, const std::string &token, int windowId)
{
	if (!g_instance) {
		return false;
	}
	PreviewSurface *surface = g_instance->SurfaceFor(windowId, canvas);
	return surface && surface->ApplyViewAction(token);
}

bool SetLocked(const std::string &canvas, bool locked, int windowId)
{
	if (!g_instance) {
		return false;
	}
	PreviewSurface *surface = g_instance->SurfaceFor(windowId, canvas);
	if (!surface) {
		return false;
	}
	surface->SetLocked(locked);
	return true;
}

// Reads must not allocate. SurfaceFor creates a surface (and its HWND) on a miss,
// which is right for the commands -- they are asking for that surface -- but a
// query that can conjure a window is a seam nobody expects, and it would make the
// caller's "no such surface" error describe a case that could never occur.
std::optional<PreviewViewState> GetView(const std::string &canvas, int windowId)
{
	if (!g_instance) {
		return std::nullopt;
	}
	PreviewSurface *surface = g_instance->FindSurface(windowId, canvas);
	if (!surface) {
		return std::nullopt;
	}
	return surface->GetView();
}

std::optional<PreviewOverlays> OverlaysFromSettings(const GeneralSettings &settings)
{
	PreviewOverflowMode overflow;
	if (!OverflowModeFromToken(settings.previewOverflow, overflow)) {
		return std::nullopt;
	}
	return PreviewOverlays(overflow, settings.previewOverflowInvisible, settings.previewSafeAreas,
			       settings.previewSpacingHelpers);
}

void OverlaysToSettings(const PreviewOverlays &overlays, GeneralSettings &settings)
{
	settings.previewOverflow = OverflowModeToken(overlays.overflow);
	settings.previewOverflowInvisible = overlays.overflowInvisible;
	settings.previewSafeAreas = overlays.safeAreas;
	settings.previewSpacingHelpers = overlays.spacingHelpers;
}

void LoadOverlays(GeneralSettings &settings)
{
	std::optional<PreviewOverlays> overlays = OverlaysFromSettings(settings);
	if (!overlays) {
		HostLog("[preview] unknown stored overflow mode '" + settings.previewOverflow + "', using the default");
		settings.previewOverflow = kDefaultPreviewOverflow;
		overlays = OverlaysFromSettings(settings);
	}
	g_overlays.Store(overlays.value());
}

const char *OverflowModeToken(PreviewOverflowMode mode)
{
	for (const OverflowModeEntry &e : kOverflowModes) {
		if (e.mode == mode) {
			return e.token;
		}
	}
	return kOverflowModes[0].token;
}

bool OverflowModeFromToken(const std::string &token, PreviewOverflowMode &out)
{
	for (const OverflowModeEntry &e : kOverflowModes) {
		if (token == e.token) {
			out = e.mode;
			return true;
		}
	}
	return false;
}

bool ZoomAt(const std::string &canvas, int x, int y, int levelDelta, int windowId)
{
	if (!g_instance) {
		return false;
	}
	PreviewSurface *surface = g_instance->FindSurface(windowId, canvas);
	if (!surface) {
		return false;
	}
	surface->ZoomAt(x, y, levelDelta);
	return true;
}

} // namespace Preview
