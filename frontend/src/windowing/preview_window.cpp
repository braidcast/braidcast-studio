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

#include <CanvasDefinition.hpp>

#include <obs.h>

#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "log.hpp"

// Item-edit handle bit flags, mirroring the legacy preview so the resize math is
// identical. Rotation is deferred but its slot is kept so corner/edge handles
// keep the same bit layout.
#define ITEM_LEFT (1 << 0)
#define ITEM_RIGHT (1 << 1)
#define ITEM_TOP (1 << 2)
#define ITEM_BOTTOM (1 << 3)
#define ITEM_ROT (1 << 4)

namespace {

constexpr float kHandleRadius = 4.0f;     // handle half-size in screen px
constexpr float kHandleSelRadius = 6.0f;  // hit-test radius (kHandleRadius * 1.5)
constexpr float kBoxLineThickness = 2.0f; // selection outline thickness in screen px

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

// A scene-item id paired with the uuid of the scene it was resolved in. Item ids
// are unique only within one scene and restart at 1 in the next, so an id kept
// across a scene switch names an unrelated item in the new scene; Resolve()
// reports -1 for any scene other than the one Set() recorded. Each id carries its
// own uuid because selection and hover are written at different moments and a
// switch can land between them.
struct SceneItemRef {
	int64_t id = -1;
	std::string sceneUuid;

	// `sceneSource` is the scene `newId` was resolved in; ignored for newId < 0.
	void Set(obs_source_t *sceneSource, int64_t newId)
	{
		id = newId;
		const char *uuid = (newId >= 0 && sceneSource) ? obs_source_get_uuid(sceneSource) : nullptr;
		sceneUuid = uuid ? uuid : std::string();
	}
	void Clear() { Set(nullptr, -1); }
	int64_t Resolve(const char *uuid) const { return (id >= 0 && uuid && sceneUuid == uuid) ? id : int64_t(-1); }
};

// Per-drag state, all captured at mousedown on the UI thread and only touched on
// the UI thread, so a drag is atomic. We store the id (re-resolved each message)
// and the box-transform-derived matrices, never an obs_sceneitem_t*. The id is
// scene-scoped like the selection and hover ids: re-resolving a bare id would let
// a scene switch mid-gesture land the drag's writes -- and the save that follows
// them -- on the new scene's item of the same id.
enum class DragMode { None, Move, Resize };
struct DragState {
	DragMode mode = DragMode::None;
	// Set on the first mouse-move that reaches a resolvable, unlocked item, BEFORE the
	// geometry math runs -- so it means "this gesture got as far as trying", not "the
	// transform changed". A drag mode can still refuse the frame (CropItem returns
	// untouched for an item carrying a bounds type). It gates the save, which is
	// idempotent either way; anything that must not fire on a no-op gesture compares the
	// geometry instead.
	bool moved = false;
	SceneItemRef id;
	vec2 startCanvasPos = {}; // mouse canvas pos at mousedown
	vec2 startItemPos = {};   // item pos at mousedown (move)
	ItemHandle handle = ItemHandle::None;
	matrix4 itemToScreen = {};
	matrix4 screenToItem = {};
	vec2 stretchItemSize = {};
	obs_sceneitem_crop startCrop = {};

	// The item's geometry at mousedown, as the opaque undo payload
	// Bridge::CaptureItemTransformState produces. Empty when no item resolved at
	// mousedown, and cleared whenever a gesture ends, so it is non-empty only while a
	// gesture with something to reverse is in flight.
	std::string undoBefore;
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
};

// Topmost-wins: obs_scene_enum_items yields bottom-to-top, so the last match
// (overwriting `item`) is the topmost hit.
bool FindItemAtPos(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	HitFind *data = static_cast<HitFind *>(param);

	if (!SceneItemHasVideo(item) || obs_sceneitem_locked(item)) {
		return true;
	}

	matrix4 transform;
	matrix4 invTransform;
	vec3 transformedPos;
	vec3 pos3;
	vec3 pos3_;

	vec3_set(&pos3, data->pos.x, data->pos.y, 0.0f);
	obs_sceneitem_get_box_transform(item, &transform);
	matrix4_inv(&invTransform, &transform);
	vec3_transform(&transformedPos, &pos3, &invTransform);
	vec3_transform(&pos3_, &transformedPos, &transform);

	if (CloseFloat(pos3.x, pos3_.x) && CloseFloat(pos3.y, pos3_.y) && transformedPos.x >= 0.0f &&
	    transformedPos.x <= 1.0f && transformedPos.y >= 0.0f && transformedPos.y <= 1.0f) {
		data->item = item;
	}
	return true;
}

// Returns the topmost item id at a canvas-space point, or -1. Caller holds the
// scene alive (the returned id is re-resolved later, never a pointer).
int64_t HitTestItemId(obs_scene_t *scene, const vec2 &canvasPos)
{
	HitFind data{canvasPos, nullptr};
	obs_scene_enum_items(scene, FindItemAtPos, &data);
	return data.item ? obs_sceneitem_get_id(data.item) : int64_t(-1);
}

struct ItemFindById {
	int64_t id;
	obs_sceneitem_t *found;
};

bool FindItemByIdCb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *c = static_cast<ItemFindById *>(param);
	if (obs_sceneitem_get_id(item) == c->id) {
		c->found = item;
		return false;
	}
	return true;
}

// Resolve a scene-item by id within a scene. The returned pointer is owned by the
// scene and valid only while the scene source is held by the caller.
obs_sceneitem_t *FindItemById(obs_scene_t *scene, int64_t id)
{
	ItemFindById ctx{id, nullptr};
	obs_scene_enum_items(scene, FindItemByIdCb, &ctx);
	return ctx.found;
}

// --- handle hit-testing (ported from legacy FindHandleAtPos, no group/rot) ---

vec3 GetTransformedPos(float x, float y, const matrix4 &mat)
{
	vec3 result;
	vec3_set(&result, x, y, 0.0f);
	vec3_transform(&result, &result, &mat);
	return result;
}

// Test the 8 resize handles of `item` against a canvas-space point. `radius` is
// in canvas units (kHandleSelRadius / scale) so the on-screen proximity is fixed.
ItemHandle FindHandleAtPos(obs_sceneitem_t *item, const vec2 &canvasPos, float radius)
{
	matrix4 transform;
	obs_sceneitem_get_box_transform(item, &transform);

	vec3 pos3;
	vec3_set(&pos3, canvasPos.x, canvasPos.y, 0.0f);

	ItemHandle found = ItemHandle::None;
	float closest = radius;

	struct HandleCoord {
		float x, y;
		ItemHandle handle;
	};
	static const HandleCoord kHandles[] = {
		{0.0f, 0.0f, ItemHandle::TopLeft},      {0.5f, 0.0f, ItemHandle::TopCenter},
		{1.0f, 0.0f, ItemHandle::TopRight},     {0.0f, 0.5f, ItemHandle::CenterLeft},
		{1.0f, 0.5f, ItemHandle::CenterRight},  {0.0f, 1.0f, ItemHandle::BottomLeft},
		{0.5f, 1.0f, ItemHandle::BottomCenter}, {1.0f, 1.0f, ItemHandle::BottomRight},
	};
	for (const auto &h : kHandles) {
		vec3 handlePos = GetTransformedPos(h.x, h.y, transform);
		const float dist = vec3_dist(&handlePos, &pos3);
		if (dist < radius && dist < closest) {
			closest = dist;
			found = h.handle;
		}
	}
	return found;
}

// What a press at a canvas-space point would grab.
struct GestureAtPos {
	ItemHandle handle = ItemHandle::None;
	obs_sceneitem_t *item = nullptr; // the selected item, when handle != None
	int64_t bodyId = -1;             // topmost item under the point, else -1
};

// A resize handle of the currently-selected item wins over an item body, and the
// body hit-test is skipped entirely once a handle matches. Shared by OnLeftDown
// and the hover cursor so the cursor cannot advertise a gesture other than the
// one the click starts. `scale` is the letterbox screen-px-per-canvas-unit, so
// the grab zone keeps a fixed kHandleSelRadius screen-px radius at any canvas size.
GestureAtPos ResolveGestureAtPos(obs_scene_t *scene, int64_t selectedId, const vec2 &canvasPos, float scale)
{
	GestureAtPos gesture;
	if (selectedId >= 0 && scale > 0.0f) {
		obs_sceneitem_t *sel = FindItemById(scene, selectedId);
		if (sel && !obs_sceneitem_locked(sel)) {
			const ItemHandle handle = FindHandleAtPos(sel, canvasPos, kHandleSelRadius / scale);
			if (handle != ItemHandle::None) {
				gesture.handle = handle;
				gesture.item = sel;
				return gesture;
			}
		}
	}
	gesture.bodyId = HitTestItemId(scene, canvasPos);
	return gesture;
}

// The directional cursor for a resize handle, ported from the legacy preview's
// UpdateCursor: the handle's edge flags are remapped through the item's rotation
// octant and its negative scales, so the arrow points along the edge the drag
// will actually move rather than along the unrotated one.
const wchar_t *CursorForHandle(obs_sceneitem_t *item, ItemHandle handle)
{
	uint32_t flags = uint32_t(handle);
	if (flags == 0) {
		return IDC_ARROW;
	}
	if (flags & ITEM_ROT) {
		// Unreachable while FindHandleAtPos's table carries only the 8 box handles.
		// The remapping below reads ITEM_LEFT..ITEM_BOTTOM only, so a rotation
		// handle left to fall through it would answer with a resize cursor. The
		// legacy preview answers this case with Qt::OpenHandCursor
		// (OBSBasicPreview.cpp:657-660); Win32 has no stock equivalent, so wiring a
		// rotation gesture means picking one here rather than deleting this branch.
		return IDC_ARROW;
	}

	// The octant and parity tests below index off a rotation in [0,360).
	float rotation = std::fmod(obs_sceneitem_get_rot(item), 360.0f);
	if (rotation < 0.0f) {
		rotation += 360.0f;
	}
	const int octant = int(std::round(rotation / 45.0f));

	vec2 scale;
	obs_sceneitem_get_scale(item, &scale);
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
vec3 CanvasSnapOffset(const GeneralSettings &gs, obs_scene_t *scene, int64_t draggedId, const vec3 &tl, const vec3 &br,
		      float baseW, float baseH);

// Seeds the item-local box corners from the drag-start size (tl at the origin, br
// at drag.stretchItemSize) and moves the live-dragged edge(s) to the mouse's
// current item-local position (canvasPos mapped through drag.screenToItem, the
// same matrix BeginResize captured at mousedown). Shared by StretchItem
// (scale/bounds resize) and CropItem (Alt-crop) so both drag modes read the
// identical canvas->item-local mapping and the identical live-edge selection.
void DragBoxLocal(const DragState &drag, const vec2 &canvasPos, vec3 &tl, vec3 &br, vec3 &pos3)
{
	const uint32_t flags = uint32_t(drag.handle);

	vec3_zero(&tl);
	vec3_set(&br, drag.stretchItemSize.x, drag.stretchItemSize.y, 0.0f);

	vec3_set(&pos3, canvasPos.x, canvasPos.y, 0.0f);
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

// Resize the active drag item to the current mouse canvas pos. Single-select,
// OBS_BOUNDS_NONE (scale) and bounds paths; aspect is preserved on corner and
// edge drags unless shiftHeld requests free aspect.
// Snaps the moving edge(s) to canvas edges/center/other sources, mirroring
// move-drag's CanvasSnapOffset via a per-live-edge probe box (see below).
void StretchItem(const DragState &drag, obs_sceneitem_t *item, const vec2 &canvasPos, obs_scene_t *scene,
		 const GeneralSettings &gs, float snapBaseW, float snapBaseH, bool shiftHeld)
{
	const obs_bounds_type boundsType = obs_sceneitem_get_bounds_type(item);
	const uint32_t flags = uint32_t(drag.handle);

	vec3 tl, br, pos3;
	DragBoxLocal(drag, canvasPos, tl, br, pos3);

	// --- resize-snap ---
	// Only one edge per live axis moves; the opposite edge is a fixed anchor.
	// Build a canvas-space probe box where the anchor edge is collapsed onto the
	// moving edge's coordinate, so CanvasSnapOffset's left/right/center checks all
	// evaluate against the true moving edge. Non-live axes pass the real box and
	// discard the returned offset for that axis.
	const bool xLive = (flags & (ITEM_LEFT | ITEM_RIGHT)) != 0;
	const bool yLive = (flags & (ITEM_TOP | ITEM_BOTTOM)) != 0;
	const bool ctrlHeld = GetKeyState(VK_CONTROL) < 0;
	if (gs.snapEnabled && !ctrlHeld && snapBaseW > 0.0f && snapBaseH > 0.0f && (xLive || yLive)) {
		vec3 canvasTl, canvasBr;
		vec3_transform(&canvasTl, &tl, &drag.itemToScreen);
		vec3_transform(&canvasBr, &br, &drag.itemToScreen);

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

		// The dragged item excludes itself from source-snapping; `item` is what
		// drag.id resolved to, so its own id is that exclusion.
		vec3 snap =
			CanvasSnapOffset(gs, scene, obs_sceneitem_get_id(item), probeTl, probeBr, snapBaseW, snapBaseH);

		// Canvas->item-local is rotation-only for a delta (itemToScreen has no
		// scale component: local and canvas share units, differing by rotation
		// and translation, and translation drops out for a delta).
		vec3 localSnap;
		matrix4 rotationOnly;
		matrix4_identity(&rotationOnly);
		matrix4_rotate_aa4f(&rotationOnly, &rotationOnly, 0.0f, 0.0f, 1.0f, RAD(-obs_sceneitem_get_rot(item)));
		vec3_transform(&localSnap, &snap, &rotationOnly);

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

		if (!shiftHeld) {
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

// Crop the active drag item to the current mouse canvas pos (Alt-drag). Adjusts
// obs_sceneitem_crop's per-edge left/right/top/bottom in source px; unlike
// StretchItem this never touches scale, so the item's on-screen scale is exactly
// what it was before the crop drag started. Never snaps (OBS's crop drag ignores
// snapping outright), so this intentionally skips the CanvasSnapOffset step.
// OBS_BOUNDS_NONE only: in bounds mode the item's scale is auto-derived from the
// (source size - crop) to fill the fixed bounds box, so cropping would implicitly
// rescale it too, breaking the "scale stays put" invariant this function relies
// on; that case is left deferred, same posture as this file's rotation-drag gap.
void CropItem(const DragState &drag, obs_sceneitem_t *item, const vec2 &canvasPos)
{
	if (obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE) {
		return;
	}

	const uint32_t flags = uint32_t(drag.handle);

	vec3 tl, br, pos3;
	DragBoxLocal(drag, canvasPos, tl, br, pos3);

	vec2 scale;
	obs_sceneitem_get_scale(item, &scale);
	if (scale.x == 0.0f || scale.y == 0.0f) {
		return;
	}

	obs_source_t *source = obs_sceneitem_get_source(item);
	const int source_cx = int(obs_source_get_width(source));
	const int source_cy = int(obs_source_get_height(source));
	if (!source_cx || !source_cy) {
		return;
	}

	// Item-local (box-unit) delta -> source-px delta: box-unit and source-px
	// differ only by the item's scale, since box_size = (source_size - crop) *
	// scale (see GetItemSize above).
	obs_sceneitem_crop crop = drag.startCrop;
	if (flags & ITEM_LEFT) {
		crop.left += int(std::round(tl.x / scale.x));
	} else if (flags & ITEM_RIGHT) {
		crop.right += int(std::round((drag.stretchItemSize.x - br.x) / scale.x));
	}
	if (flags & ITEM_TOP) {
		crop.top += int(std::round(tl.y / scale.y));
	} else if (flags & ITEM_BOTTOM) {
		crop.bottom += int(std::round((drag.stretchItemSize.y - br.y) / scale.y));
	}

	// Corner handles touch two edges on two different axes (e.g. top-left ->
	// left+top), never two edges of the same axis, so each live edge clamps
	// independently against its own untouched opposite edge. A 1px sliver of
	// source is kept visible so a drag can never crop past the far edge.
	if (flags & ITEM_LEFT) {
		crop.left = std::clamp(crop.left, 0, std::max(0, source_cx - 1 - crop.right));
	} else if (flags & ITEM_RIGHT) {
		crop.right = std::clamp(crop.right, 0, std::max(0, source_cx - 1 - crop.left));
	}
	if (flags & ITEM_TOP) {
		crop.top = std::clamp(crop.top, 0, std::max(0, source_cy - 1 - crop.bottom));
	} else if (flags & ITEM_BOTTOM) {
		crop.bottom = std::clamp(crop.bottom, 0, std::max(0, source_cy - 1 - crop.top));
	}

	obs_sceneitem_set_crop(item, &crop);

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

// Capture the matrices/sizes a resize drag needs (legacy GetStretchHandleData,
// no-group path) for the chosen item + handle. `sceneSource` is the scene `item`
// belongs to, recorded with its id so a scene switch mid-gesture cannot redirect
// the drag onto the new scene's item of the same id.
void BeginResize(DragState &drag, obs_source_t *sceneSource, obs_sceneitem_t *item, ItemHandle handle,
		 const vec2 &startCanvasPos)
{
	matrix4 boxTransform;
	vec3 itemUL;

	drag.mode = DragMode::Resize;
	drag.moved = false;
	drag.id.Set(sceneSource, obs_sceneitem_get_id(item));
	drag.handle = handle;
	drag.startCanvasPos = startCanvasPos;
	drag.stretchItemSize = GetItemSize(item);

	obs_sceneitem_get_box_transform(item, &boxTransform);
	const float itemRot = obs_sceneitem_get_rot(item);
	vec3_from_vec4(&itemUL, &boxTransform.t);

	matrix4_identity(&drag.itemToScreen);
	matrix4_rotate_aa4f(&drag.itemToScreen, &drag.itemToScreen, 0.0f, 0.0f, 1.0f, RAD(itemRot));
	matrix4_translate3f(&drag.itemToScreen, &drag.itemToScreen, itemUL.x, itemUL.y, 0.0f);

	matrix4_identity(&drag.screenToItem);
	matrix4_translate3f(&drag.screenToItem, &drag.screenToItem, -itemUL.x, -itemUL.y, 0.0f);
	matrix4_rotate_aa4f(&drag.screenToItem, &drag.screenToItem, 0.0f, 0.0f, 1.0f, RAD(-itemRot));

	obs_sceneitem_get_crop(item, &drag.startCrop);
	obs_sceneitem_get_pos(item, &drag.startItemPos);
}

// One end of a drag's undo pair: the item's full geometry plus the keys that re-resolve
// it, addressed by this surface's canvas and by the scene the item was resolved in.
// Empty for a null item.
std::string CaptureDragUndoState(obs_canvas_t *targetCanvas, obs_source_t *sceneSource, obs_sceneitem_t *item)
{
	if (!item || !sceneSource) {
		return std::string();
	}
	const char *canvasUuid = targetCanvas ? obs_canvas_get_uuid(targetCanvas) : nullptr;
	const char *sceneName = obs_source_get_name(sceneSource);
	return Bridge::CaptureItemTransformState(canvasUuid ? canvasUuid : "", sceneName ? sceneName : "", item);
}

// The scene a drag STARTED in, addref'd (caller releases) or null once that scene is
// gone. Deliberately not the surface's current scene, which is what the rest of the drag
// path resolves against: a scene switch mid-gesture makes the remaining frames inert but
// does not un-move what the earlier ones already moved, and the entry that reverses them
// has to name the scene they landed in.
obs_source_t *AcquireDragScene(const SceneItemRef &draggedRef)
{
	if (draggedRef.id < 0) {
		return nullptr;
	}
	return Bridge::AcquireSceneByUuid(draggedRef.sceneUuid); // addref'd
}

// --- selection -------------------------------------------------------------

struct SelectCtx {
	int64_t id;
};

bool SelectOnlyCb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	const SelectCtx *ctx = static_cast<SelectCtx *>(param);
	obs_sceneitem_select(item, obs_sceneitem_get_id(item) == ctx->id);
	return true;
}

// Select exactly `id` in the scene (deselect everything else). id == -1 clears.
void SelectOnly(obs_scene_t *scene, int64_t id)
{
	SelectCtx ctx{id};
	obs_scene_enum_items(scene, SelectOnlyCb, &ctx);
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

// Draw a filled square at a unit-space handle coord. Reads the current matrix --
// in the editing phase, the letterbox scale times the item's box transform -- and
// maps the point through it into that phase's screen-px space, then draws an
// axis-aligned square there off a reset matrix, so `halfSize` is screen px.
void DrawSquareAtPos(float x, float y, float halfSize)
{
	vec3 pos;
	vec3_set(&pos, x, y, 0.0f);
	matrix4 matrix;
	gs_matrix_get(&matrix);
	vec3_transform(&pos, &pos, &matrix);

	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_translate(&pos);
	gs_matrix_translate3f(-halfSize, -halfSize, 0.0f);
	gs_matrix_scale3f(halfSize * 2.0f, halfSize * 2.0f, 1.0f);
	gs_draw(GS_TRISTRIP, 0, 0);
	gs_matrix_pop();
}

// Outline colors: the selection green this file already used, and the legacy
// preview's hover blue (OBSBasic::GetHoverColor's non-override default,
// rgb(0,127,255)).
const vec4 kSelectionColor = {{{0.0f, 1.0f, 0.235f, 1.0f}}};
const vec4 kHoverColor = {{{0.0f, 0.498f, 1.0f, 1.0f}}};

// Draw `item`'s box outline in `color`, plus the 8 resize handles when
// `handleBuffer` is non-null (the shared unit-quad TRISTRIP vertbuffer). Both are
// drawn inside the box-transform matrix in unit space, so they follow the item's
// rotation/scale. Runs in the draw callback's editing phase, whose ortho is screen
// px and whose matrix stack already carries the letterbox scale (see
// RenderPreview). `scale` = letterbox screen-px-per-canvas-unit: boxScale maps
// unit->screen px so the line thickness stays ~constant on screen, and the handle
// half-size is kHandleRadius unscaled because DrawSquareAtPos draws off a reset
// matrix, in this phase's screen px.
void DrawItemBox(obs_sceneitem_t *item, float scale, const vec4 &color, gs_vertbuffer_t *handleBuffer)
{
	if (scale <= 0.0f) {
		return;
	}
	matrix4 boxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);

	vec2 boxScale;
	obs_sceneitem_get_box_scale(item, &boxScale);
	boxScale.x *= scale;
	boxScale.y *= scale;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *colParam = gs_effect_get_param_by_name(solid, "color");

	gs_matrix_push();
	gs_matrix_mul(&boxTransform);

	gs_effect_set_vec4(colParam, &color);
	while (gs_effect_loop(solid, "Solid")) {
		DrawRect(kBoxLineThickness, boxScale);
	}

	if (handleBuffer) {
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

		// Unbind before leaving: the device keeps the last loaded buffer, and
		// nothing downstream of this callback is obliged to load its own.
		gs_load_vertexbuffer(nullptr);
		gs_technique_end_pass(tech);
		gs_technique_end(tech);
	}

	gs_matrix_pop();
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
	SceneItemRef selected;
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

	// Unit-quad TRISTRIP vertbuffer for the selection handles, created lazily on
	// the render thread and destroyed under a graphics context in Destroy().
	gs_vertbuffer_t *boxBuffer = nullptr;

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

// Emit sceneItem.selected to JS for the surface's scene. `id` < 0 ->
// {scene:null,id:null}. Posts to the UI thread internally so it is safe from
// WndProc.
void EmitSelection(obs_canvas_t *targetCanvas, int64_t id)
{
	std::string sceneName;
	if (id >= 0) {
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
	// The addressed canvas uuid, or null for the Default surface (global path), so a
	// per-canvas dock filters selection to its own canvas (scene names collide).
	json canvasField = json(nullptr);
	if (targetCanvas) {
		const char *uuid = obs_canvas_get_uuid(targetCanvas);
		if (uuid) {
			canvasField = json(std::string(uuid));
		}
	}
	json payload = json{
		{"scene", id >= 0 && !sceneName.empty() ? json(sceneName) : json(nullptr)},
		{"id", id >= 0 ? json(id) : json(nullptr)},
		{"canvas", canvasField},
	};
	Bridge::EmitEvent(EventNames::kSceneItemSelected, payload);
}

// Emit preview.contextMenu for a right-click at device-px (mx,my) in the overlay
// client. Carries the hit item's id/source/visible/locked (null id => empty area)
// plus the surface's scene name, addressed canvas uuid (null for Default), and the
// originating windowId, so JS filters to the right window+canvas and builds the
// menu without a round-trip. Posts via EmitEvent (broadcast to all browsers).
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
	json canvasField = json(nullptr);
	if (targetCanvas) {
		const char *uuid = obs_canvas_get_uuid(targetCanvas);
		if (uuid) {
			canvasField = json(std::string(uuid));
		}
	}
	Bridge::EmitEvent(EventNames::kPreviewPointerOver,
			  json{{"canvas", canvasField}, {"window", windowId}, {"over", over}});
}

void EmitContextMenu(obs_canvas_t *targetCanvas, int windowId, obs_scene_t *scene, int64_t id, int mx, int my)
{
	using Bridge::json;
	json canvasField = json(nullptr);
	if (targetCanvas) {
		const char *uuid = obs_canvas_get_uuid(targetCanvas);
		if (uuid) {
			canvasField = json(std::string(uuid));
		}
	}

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
		obs_sceneitem_t *item = FindItemById(scene, id); // borrowed
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
		{"canvas", canvasField},
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
// the display (HWND) pixel size. Two phases: fit the surface's base canvas into
// the display with letterboxing so the composited scene keeps its aspect ratio,
// then switch to a screen-px space covering the whole display for the editing
// overlay -- the hovered item's outline, and the selected item's box + handles,
// each re-resolved by id from the surface's current scene. `data` is the
// PreviewSurface::State.
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

	gs_viewport_push();
	gs_projection_push();

	gs_ortho(0.0f, baseCX, 0.0f, baseCY, -100.0f, 100.0f);
	gs_set_viewport(drawX, drawY, drawCX, drawCY);

	if (targetCanvas) {
		obs_render_canvas_texture(targetCanvas);
	} else {
		obs_render_main_texture();
	}

	// Cheap gate: skip the scene addref entirely when neither id is set. The raw ids
	// are enough here -- whether they still belong to the current scene is settled
	// by Resolve() below, once that scene is in hand.
	bool anyEditId;
	{
		std::lock_guard<std::mutex> lock(state->stateMutex);
		anyEditId = state->selected.id >= 0 || state->hovered.id >= 0;
	}

	obs_source_t *sceneSource = anyEditId ? AcquireSurfaceSceneSource(targetCanvas) : nullptr;
	if (sceneSource) {
		const char *sceneUuid = obs_source_get_uuid(sceneSource);
		int64_t selectedId;
		int64_t hoveredId;
		{
			std::lock_guard<std::mutex> lock(state->stateMutex);
			selectedId = state->selected.Resolve(sceneUuid);
			hoveredId = state->hovered.Resolve(sceneUuid);
		}

		if (selectedId >= 0 || hoveredId >= 0) {
			// Editing phase, in a different space than the video above: ortho
			// measured in screen px with the canvas origin at 0,0, over the whole
			// display rather than the canvas viewport, so an outline or handle that
			// falls in the letterbox is drawn instead of being clipped by that
			// viewport. The matrix scale carries the items' canvas-space box
			// transforms into this space.
			gs_ortho(float(-drawX), float(cx) - float(drawX), float(-drawY), float(cy) - float(drawY),
				 -100.0f, 100.0f);
			gs_reset_viewport();

			gs_matrix_push();
			gs_matrix_scale3f(scale, scale, 1.0f);

			obs_scene_t *scene = obs_scene_from_source(sceneSource);
			// Hover first, so the selected item's box and handles draw over it. An
			// eye-off item is skipped: outlining a source the user has hidden would
			// paint a box on apparently-empty canvas. Diverges from the legacy
			// preview, which hover-outlines invisible items.
			if (hoveredId >= 0 && hoveredId != selectedId) {
				obs_sceneitem_t *item = FindItemById(scene, hoveredId);
				if (item && SceneItemHasVideo(item) && !obs_sceneitem_locked(item) &&
				    obs_sceneitem_visible(item)) {
					DrawItemBox(item, scale, kHoverColor, nullptr);
				}
			}
			// Selection deliberately does NOT take the visible check above: an
			// eye-off source the user selected on purpose still shows its box and
			// handles, which is the only way to see and adjust a hidden item's
			// transform in the preview. Hover is the passive case, selection the
			// asked-for one, so the asymmetry is the intent, not an oversight.
			if (selectedId >= 0) {
				obs_sceneitem_t *item = FindItemById(scene, selectedId);
				if (item && SceneItemHasVideo(item) && !obs_sceneitem_locked(item)) {
					EnsureBoxBuffer(state);
					DrawItemBox(item, scale, kSelectionColor, state->boxBuffer);
				}
			}

			gs_matrix_pop();
		}
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

	if (HWND hwnd = overlay_.Hwnd()) {
		SetCapture(hwnd);
	}

	// Space + left-drag pans instead of touching an item, and only once zoomed:
	// fit mode has nothing to pan, since the whole canvas is already in view. The
	// modifier is sampled here and nowhere else in the gesture, which is what makes
	// pressing space mid-drag harmless -- an item gesture already in flight is never
	// converted to a pan, it just finishes as the move or resize it started as.
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
	int64_t selectedId;
	{
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		selectedId = state_->selected.Resolve(sceneUuid);
	}
	// A locked preview starts no gesture, so it resolves none: passing -1 as the
	// selected id skips the handle test and leaves a plain body hit-test, which is
	// all selection needs. Selection stays live on purpose -- the lock is about
	// editing geometry, not about choosing what the docks show -- so the resize
	// branch below cannot fire and only the move is gated.
	const bool locked = Locked();
	const GestureAtPos gesture =
		ResolveGestureAtPos(scene, locked ? -1 : selectedId, canvasPos, CurrentScale(state_));

	// A handle of the currently-selected item begins a resize.
	if (gesture.handle != ItemHandle::None) {
		BeginResize(state_->drag, sceneSource, gesture.item, gesture.handle, canvasPos);
		state_->drag.undoBefore = CaptureDragUndoState(targetCanvas_, sceneSource, gesture.item);
		HostLog("[preview] resize start id=" + std::to_string(selectedId) +
			" handle=" + std::to_string(uint32_t(gesture.handle)));
		obs_source_release(sceneSource);
		return;
	}

	// Otherwise select/move the hit item (or deselect on empty).
	const int64_t hitId = gesture.bodyId;
	HostLog("[preview] click canvas=(" + std::to_string(int(canvasPos.x)) + "," + std::to_string(int(canvasPos.y)) +
		") hit id=" + std::to_string(hitId));

	if (hitId >= 0) {
		SelectOnly(scene, hitId);

		obs_sceneitem_t *item = FindItemById(scene, hitId);

		{
			std::lock_guard<std::mutex> lock(state_->stateMutex);
			state_->selected.Set(sceneSource, hitId);
		}
		if (!locked) {
			state_->drag.mode = DragMode::Move;
			state_->drag.moved = false;
			state_->drag.id.Set(sceneSource, hitId);
			state_->drag.startCanvasPos = canvasPos;
			if (item) {
				obs_sceneitem_get_pos(item, &state_->drag.startItemPos);
			}
			state_->drag.undoBefore = CaptureDragUndoState(targetCanvas_, sceneSource, item);
		}
		EmitSelection(targetCanvas_, hitId);
	} else {
		SelectOnly(scene, -1);
		{
			std::lock_guard<std::mutex> lock(state_->stateMutex);
			state_->selected.Clear();
		}
		state_->drag.mode = DragMode::None;
		EmitSelection(targetCanvas_, -1);
	}

	obs_source_release(sceneSource);
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
	int64_t draggedId;
	vec3 tl, br, offset;
};

bool SourceSnapCb(obs_scene_t * /* scene */, obs_sceneitem_t *item, void *param)
{
	auto *data = static_cast<SnapAccum *>(param);

	if (obs_sceneitem_get_id(item) == data->draggedId) {
		return true;
	}
	if (obs_sceneitem_locked(item) || !obs_sceneitem_visible(item) || !SceneItemHasVideo(item)) {
		return true;
	}

	matrix4 boxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);

	vec3 t[4] = {
		GetTransformedPos(0.0f, 0.0f, boxTransform),
		GetTransformedPos(1.0f, 0.0f, boxTransform),
		GetTransformedPos(0.0f, 1.0f, boxTransform),
		GetTransformedPos(1.0f, 1.0f, boxTransform),
	};

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
vec3 CanvasSnapOffset(const GeneralSettings &gs, obs_scene_t *scene, int64_t draggedId, const vec3 &tl, const vec3 &br,
		      float baseW, float baseH)
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
	acc.draggedId = draggedId;
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
		int64_t selectedId;
		{
			std::lock_guard<std::mutex> lock(state_->stateMutex);
			selectedId = state_->selected.Resolve(sceneUuid);
		}
		const GestureAtPos gesture = ResolveGestureAtPos(scene, selectedId, canvasPos, CurrentScale(state_));

		if (gesture.handle != ItemHandle::None) {
			cursor = CursorForHandle(gesture.item, gesture.handle);
		} else if (gesture.bodyId >= 0) {
			cursor = IDC_SIZEALL;
			hoveredId = gesture.bodyId;
		}
	}

	{
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		state_->hovered.Set(sceneSource, hoveredId);
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
	if (state_->drag.mode == DragMode::None) {
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

	// A scene switch since mousedown resolves to -1 and leaves the rest of the
	// gesture inert, rather than applying it to the new scene's item of that id.
	const int64_t dragId = state_->drag.id.Resolve(obs_source_get_uuid(sceneSource));
	obs_sceneitem_t *item = dragId >= 0 ? FindItemById(scene, dragId) : nullptr;
	if (item && !obs_sceneitem_locked(item)) {
		state_->drag.moved = true;
		if (state_->drag.mode == DragMode::Move) {
			float offX = canvasPos.x - state_->drag.startCanvasPos.x;
			float offY = canvasPos.y - state_->drag.startCanvasPos.y;

			// Snap the move to canvas edges/center and other items' edges, unless
			// disabled in General settings or temporarily suppressed with Ctrl.
			const GeneralSettings &gs = ObsBootstrap::General();
			const bool ctrlHeld = GetKeyState(VK_CONTROL) < 0;
			obs_video_info ovi;
			if (gs.snapEnabled && !ctrlHeld && SurfaceVideoInfo(targetCanvas_, ovi) && ovi.base_width &&
			    ovi.base_height) {
				// Dragged item's transformed bounding box, translated from its
				// current pos to the proposed pos (pos is a pure translation, so
				// this stays correct for rotated/bounds-scaled items).
				matrix4 boxTransform;
				obs_sceneitem_get_box_transform(item, &boxTransform);
				vec3 corners[4] = {
					GetTransformedPos(0.0f, 0.0f, boxTransform),
					GetTransformedPos(1.0f, 0.0f, boxTransform),
					GetTransformedPos(0.0f, 1.0f, boxTransform),
					GetTransformedPos(1.0f, 1.0f, boxTransform),
				};
				vec3 tl, br;
				vec3_copy(&tl, &corners[0]);
				vec3_copy(&br, &corners[0]);
				for (const vec3 &v : corners) {
					vec3_min(&tl, &tl, &v);
					vec3_max(&br, &br, &v);
				}

				vec2 curPos;
				obs_sceneitem_get_pos(item, &curPos);
				const float shiftX = state_->drag.startItemPos.x + offX - curPos.x;
				const float shiftY = state_->drag.startItemPos.y + offY - curPos.y;
				tl.x += shiftX;
				br.x += shiftX;
				tl.y += shiftY;
				br.y += shiftY;

				vec3 snap = CanvasSnapOffset(gs, scene, dragId, tl, br, float(ovi.base_width),
							     float(ovi.base_height));
				offX += snap.x;
				offY += snap.y;
			}

			vec2 newPos;
			newPos.x = std::round(state_->drag.startItemPos.x + offX);
			newPos.y = std::round(state_->drag.startItemPos.y + offY);
			obs_sceneitem_set_pos(item, &newPos);
		} else if (state_->drag.mode == DragMode::Resize) {
			const GeneralSettings &gs = ObsBootstrap::General();
			const bool ctrlHeld = GetKeyState(VK_CONTROL) < 0;
			const bool shiftHeld = GetKeyState(VK_SHIFT) < 0;
			const bool altHeld = GetKeyState(VK_MENU) < 0;
			if (altHeld) {
				CropItem(state_->drag, item, canvasPos);
			} else {
				obs_video_info ovi;
				float snapBaseW = 0.0f, snapBaseH = 0.0f;
				if (gs.snapEnabled && !ctrlHeld && SurfaceVideoInfo(targetCanvas_, ovi) &&
				    ovi.base_width && ovi.base_height) {
					snapBaseW = float(ovi.base_width);
					snapBaseH = float(ovi.base_height);
				}
				StretchItem(state_->drag, item, canvasPos, scene, gs, snapBaseW, snapBaseH, shiftHeld);
			}
		}
	}
	obs_source_release(sceneSource);
}

bool PreviewSurface::FinishDrag()
{
	const bool dragged = state_->drag.mode != DragMode::None;
	const bool moved = state_->drag.moved;
	const bool resized = state_->drag.mode == DragMode::Resize;
	const SceneItemRef draggedRef = state_->drag.id;
	std::string undoBefore;
	undoBefore.swap(state_->drag.undoBefore);
	if (dragged) {
		HostLog("[preview] drag end id=" + std::to_string(draggedRef.id));
	}
	state_->drag.mode = DragMode::None;
	state_->drag.handle = ItemHandle::None;
	state_->drag.moved = false;

	// Every route that ends a gesture lands here, and a gesture can end with the
	// pointer anywhere: a button-up outside the preview arrives only through the
	// capture, and a capture lost to Alt-Tab or a foreground change carries no
	// pointer position at all. Drop the hover outline -- the next move recomputes it
	// -- or it paints on the just-dragged item the moment a bridge-driven selection
	// moves elsewhere. The cursor shape is deliberately left alone (see ClearHover).
	ClearHoverItem();

	// A braidcast_overlay source renders its page at the size in its settings and the
	// item then scales that bitmap, so a resize alone would magnify pixels rather than
	// re-lay-out the widget. Make the page follow the box instead. Resize only: a move
	// leaves the box alone, while an Alt-crop changes how much page the box needs, so
	// both stretch and crop drags (which share DragMode::Resize) commit.
	if (resized && moved) {
		obs_source_t *sceneSource = AcquireSurfaceSceneSource(targetCanvas_); // addref'd
		if (sceneSource) {
			// Same scene scoping as the drag itself: a switch since mousedown must
			// not commit this overlay's layout onto the new scene's same-id item.
			const int64_t draggedId = draggedRef.Resolve(obs_source_get_uuid(sceneSource));
			obs_sceneitem_t *item =
				draggedId >= 0 ? FindItemById(obs_scene_from_source(sceneSource), draggedId) : nullptr;
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
	// Whether an entry is actually pushed is decided by RecordItemTransformUndo, which
	// compares the two payloads -- a drag mode can refuse every frame and leave the
	// geometry untouched while `moved` is true. The empty check is the mousedown that
	// resolved no item; a press can no longer inherit a live gesture's payload, because
	// OnLeftDown ends any gesture still in flight before it records its own.
	if (moved && !undoBefore.empty()) {
		obs_source_t *dragScene = AcquireDragScene(draggedRef); // addref'd
		obs_scene_t *scene = dragScene ? obs_scene_from_source(dragScene) : nullptr;
		obs_sceneitem_t *item = scene ? FindItemById(scene, draggedRef.id) : nullptr;
		if (item) {
			Bridge::RecordItemTransformUndo(obs_sceneitem_get_source(item), undoBefore,
							CaptureDragUndoState(targetCanvas_, dragScene, item));
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
	// opens its menu on the release, so it never swallows a menu. This surface is
	// only sent the release, so the two cannot both happen here. Kept as-is; exact
	// parity is available by handling WM_RBUTTONDOWN, cancelling there, and letting
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
	const int64_t hitId = HitTestItemId(scene, canvasPos);

	// Right-click selects (or clears) the item under the cursor before the menu
	// opens, matching OBS and keeping the selection box + dock lists in sync.
	SelectOnly(scene, hitId);
	{
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		state_->selected.Set(sceneSource, hitId);
	}
	FinishDrag();
	EmitSelection(targetCanvas_, hitId);
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

void PreviewSurface::GetView(bool &fixed, int &zoomPercent, bool &locked)
{
	std::lock_guard<std::mutex> lock(state_->stateMutex);
	fixed = state_->view.fixed;
	locked = state_->view.locked;
	// The scale the next frame will draw at, not the one the last frame did. A menu
	// is built right after the command that opened it, so reporting the published
	// scale would answer with the percentage the preview is leaving.
	zoomPercent = int(std::lround(PendingScale(state_->view, state_->transform) * 100.0f));
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
	// before DestroyWindow (overlay_surface.cpp:260-261), so nothing the destruction
	// sends can route back into this surface's WndProc -- CancelDrag included. Ahead of
	// overlay_.Destroy() so the gesture finishes while the surface is whole; every
	// Destroy path is already the window-owning thread, because the DestroyWindow it
	// reaches must be.
	FinishDrag();
	EndPanOffSurface();
	// A surface torn down with the pointer still over it must retract the flag, or
	// the web view keeps suppressing the pan key's default action for a surface
	// that no longer exists. WM_MOUSELEAVE cannot deliver this -- the HWND is about
	// to stop routing messages to this object at all.
	RetractPointerOver();

	// The display dies first (OverlaySurface removes the draw callback), so nothing
	// the render thread reads outlives it -- including the box buffer below.
	overlay_.Destroy();
	if (state_->boxBuffer) {
		obs_enter_graphics();
		gs_vertexbuffer_destroy(state_->boxBuffer);
		obs_leave_graphics();
		state_->boxBuffer = nullptr;
	}
}

bool PreviewSurface::SelectFromBridge(const std::string &scene, int64_t id, bool hasId)
{
	obs_source_t *sceneSource = AcquireSurfaceSceneSource(targetCanvas_);
	if (!sceneSource) {
		return false;
	}
	// Ignore a foreign scene name to keep "preview shows the surface's scene" intact.
	if (!scene.empty()) {
		const char *n = obs_source_get_name(sceneSource);
		if (!n || scene != n) {
			obs_source_release(sceneSource);
			return false;
		}
	}
	obs_scene_t *sc = obs_scene_from_source(sceneSource);

	const int64_t newId = hasId ? id : int64_t(-1);
	SelectOnly(sc, newId);
	{
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		state_->selected.Set(sceneSource, newId);
	}
	obs_source_release(sceneSource);

	EmitSelection(targetCanvas_, newId);
	return true;
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

int64_t PreviewSurface::SelectedIdForTest()
{
	// The recorded id, not scene-resolved: the isolation self-test asserts what this
	// surface holds, and it holds it against its own scene.
	std::lock_guard<std::mutex> lock(state_->stateMutex);
	return state_->selected.id;
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
		OnLeftDown(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
		return true;
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

bool SelectFromBridge(const std::string &canvas, const std::string &scene, int64_t id, bool hasId, int windowId)
{
	if (!g_instance) {
		return false;
	}
	PreviewSurface *surface = g_instance->SurfaceFor(windowId, canvas);
	if (!surface) {
		return false;
	}
	return surface->SelectFromBridge(scene, id, hasId);
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
bool GetView(const std::string &canvas, bool &fixed, int &zoomPercent, bool &locked, int windowId)
{
	if (!g_instance) {
		return false;
	}
	PreviewSurface *surface = g_instance->FindSurface(windowId, canvas);
	if (!surface) {
		return false;
	}
	surface->GetView(fixed, zoomPercent, locked);
	return true;
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
