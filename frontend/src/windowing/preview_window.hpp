#ifndef OBS_MULTISTREAM_FRONTEND_PREVIEW_WINDOW_HPP_
#define OBS_MULTISTREAM_FRONTEND_PREVIEW_WINDOW_HPP_

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "overlay_surface.hpp"
#include "scene/scene_items.hpp"

struct obs_canvas;
typedef struct obs_canvas obs_canvas_t;
struct obs_scene;
typedef struct obs_scene obs_scene_t;
struct obs_source;
typedef struct obs_source obs_source_t;
struct GeneralSettings;

// Which scene items the preview draws overflow for -- the part of an item that lies
// outside the canvas: none, the selected ones, or every one.
enum class PreviewOverflowMode : uint8_t { Hidden, Selection, Always };

// The preview's guide overlays. One set for the whole process, not one per surface:
// the legacy frontend kept them as user-wide preferences, so a choice made from any
// preview's menu applies to every preview. GeneralSettings owns the persisted values
// and their defaults; this is the decoded form the previews draw from. It has no default
// constructor, so no instance of it can stand in for those defaults: each one is decoded
// from GeneralSettings or read back from the render-side state that decode loaded.
struct PreviewOverlays {
	PreviewOverlays(PreviewOverflowMode overflow, bool overflowInvisible, bool safeAreas, bool spacingHelpers)
		: overflow(overflow),
		  overflowInvisible(overflowInvisible),
		  safeAreas(safeAreas),
		  spacingHelpers(spacingHelpers)
	{
	}

	PreviewOverflowMode overflow;
	// Draw overflow for items whose visibility is off, too.
	bool overflowInvisible;
	bool safeAreas;
	bool spacingHelpers;
};

// The gestures a scripted drag can start, one per grab point PreviewSurface::DragForTest
// knows how to aim at: the item's body, its bottom-right resize handle, its rotation disc, and
// its left edge under Alt (a crop). Exists for the smoke self-test, which has no cursor and no
// keyboard.
//
// MoveSnapping is Move with the keyboard's snap suppression NOT held, so the drag runs through
// CanvasSnapOffset the way an ordinary one does; every other gesture suppresses it, because a
// snap would move the item somewhere other than the offset the case asked for.
enum class PreviewTestGesture { Move, MoveSnapping, ResizeBottomRight, Rotate, CropLeft };

// What a preview's context menu renders from: whether the surface is at a fixed scale
// rather than fitted, the scale the next frame draws at as a percentage, the edit lock,
// and the overlays.
struct PreviewViewState {
	bool fixed;
	int zoomPercent;
	bool locked;
	PreviewOverlays overlays;
};

// One native preview surface: an OverlaySurface (a borderless child HWND of the
// host, sibling above the CEF browser HWND, plus its own obs_display) rendering one
// canvas's program scene with aspect-correct letterboxing, and the scene editing
// driven off that HWND's mouse input.
//
// A surface is bound to a `targetCanvas` at construction. A null targetCanvas is
// the Default surface: it renders the global mix (obs_get_video_info +
// obs_render_main_texture) and edits output channel 0's scene, byte-identical to
// the original single-preview path. A non-null targetCanvas renders that canvas's
// mix (obs_canvas_get_video_info + obs_canvas_render) and edits the canvas's
// current scene (CanvasRuntime::CurrentScene), so each additional canvas's panel
// drags/selects/transforms within its own scene.
//
// Each surface owns its editing state (selection, drag, letterbox transform)
// behind its own mutex, so an edit on one surface never bleeds into another. The
// render callback runs on the libobs render thread; SetRect/Hide/Destroy + the
// mouse WndProc run on the host UI thread. The mutex guards only state shared
// across those two threads; it is never held across an obs_display/render call.
//
// The overlay HWND + display are created lazily on the first SetRect so the UI
// drives the geometry. Lives between its owner's creation and Destroy().
class PreviewSurface : public OverlaySurface::MessageSink {
public:
	// host: the top-level host window the overlay is parented to. targetCanvas:
	// the canvas mix this surface renders+edits, or null for the Default surface
	// (global mix + output channel 0). The canvas pointer is borrowed (owned by
	// CanvasRuntime); the surface's display must be destroyed before that mix.
	// windowId: the owning window id (0 = main), carried in preview.contextMenu so
	// JS filters the broadcast to the originating window.
	PreviewSurface(HWND host, HINSTANCE instance, obs_canvas_t *targetCanvas, int windowId);
	~PreviewSurface() override;

	PreviewSurface(const PreviewSurface &) = delete;
	PreviewSurface &operator=(const PreviewSurface &) = delete;

	// Position/size the overlay (device pixels, host-client coords) and resize the
	// obs_display. Creates the overlay HWND + display on first call. Zero/negative
	// size hides instead.
	void SetRect(int x, int y, int cx, int cy);

	// Hide the overlay HWND and drop its swapchain, keeping the HWND for the next
	// SetRect.
	void Hide();

	// Destroy the obs_display (must run while libobs is up, before this surface's
	// canvas mix is freed) and the overlay HWND.
	void Destroy();

	// The overlay HWND, or null until the first SetRect.
	HWND Hwnd() const { return overlay_.Hwnd(); }

	// Drive selection from JS without a mouse event (the SourcesPanel). `scene` is
	// validated against the surface's current scene name (a mismatch is ignored); an
	// empty `keys` clears. The whole set is replaced, so a dock's modifier click lands
	// here as the set it produced rather than as a sequence of single picks. A group's
	// child whose group or item does not resolve in the scene is dropped. Emits
	// sceneItem.selected, and returns the set now selected, or nothing on a mismatch.
	std::optional<std::vector<SceneItemKey>> SelectFromBridge(const std::string &scene,
								  const std::vector<SceneItemKey> &keys);

	// Zoom by `levelDelta` notches about the client pixel (px, py), which stays over
	// the same canvas point. Public because the wheel arrives by two routes -- the
	// overlay's own WM_MOUSEWHEEL and a forward from the web view -- and both land
	// on this one implementation. UI thread.
	void ZoomAt(int px, int py, int levelDelta);

	// Apply one Scale-menu command by token ("zoomIn", "zoomOut", "scaleToWindow",
	// "scaleToCanvas"); false for a token this surface does not recognize. The token
	// list lives in the .cpp beside the behaviour it names. UI thread.
	bool ApplyViewAction(const std::string &token);

	// Block (or allow) editing gestures. Selection, the context menu and the view
	// commands keep working while locked; an in-flight gesture is ended. UI thread.
	void SetLocked(bool locked);

	// The view state the context menu renders from. UI thread.
	PreviewViewState GetView();

	// Hit-test at a canvas-space coordinate against this surface's scene; returns
	// the topmost matching scene-item id, or -1. Used by the smoke self-test.
	int64_t HitTestForTest(float canvasX, float canvasY);

	// Drive one whole editing gesture on `key` for the smoke self-test: grab what `gesture`
	// names on that item, drag by (dx, dy) canvas px in one move, and release, through the
	// same OnLeftDown/OnMouseMove/OnLeftUp the mouse takes. `key` must already be selected,
	// since only a selected item offers handles and only a selected item's body starts a
	// move. Ctrl stands in for "suppress snapping" on every gesture but the crop, which takes
	// Alt, because a scripted drag has to land where it asked to and both modifiers are
	// sampled from a keyboard a headless run does not have. Seeds the letterbox transform to
	// 1:1 first: a surface the UI never sized has drawn no frame to read one from, and every
	// mouse entry point refuses input without it. Returns false when the item does not
	// resolve, its box is degenerate, the press grabbed something other than what `gesture`
	// named, or the drag changed nothing. `outGrab`, when given, receives the CANVAS point the
	// press landed on, so a case can state what the gesture should have done in terms of where
	// the pointer actually went rather than re-deriving the grab geometry. UI thread.
	bool DragForTest(const SceneItemKey &key, PreviewTestGesture gesture, float dx, float dy,
			 vec2 *outGrab = nullptr);

	// The ANCHOR scene-item id of this surface's selection (-1 when none). Used by the
	// isolation self-test to prove an edit on one surface leaves another's selection
	// untouched. Reads under the surface's own lock.
	int64_t SelectedIdForTest();

	// This surface's whole selection, in order, for the smoke self-test: the anchor id
	// alone cannot say which owner a member belongs to. Reads under the surface's own lock.
	std::vector<SceneItemKey> SelectedKeysForTest();

	// The uuid of the group source this surface is drilled into, empty when it is in none.
	// RESOLVED against the scene the surface shows now, so a drill-in that any exit rule has
	// invalidated reports as left even before a pointer event notices. UI thread.
	std::string EnteredGroupForTest();

	// Drive a whole left click at a canvas point for the smoke self-test, through the same
	// OnLeftDown/OnLeftUp the mouse takes. `doubleClick` appends the second press as
	// WM_LBUTTONDBLCLK does, which is the only form Windows sends on this window class --
	// so a scripted double click exercises the same routing a real one does. Seeds the
	// letterbox transform to 1:1 first (see DragForTest) and holds no modifier. UI thread.
	void ClickForTest(float canvasX, float canvasY, bool doubleClick);

	// The same, for a rubber band: press at (fromX, fromY), one move to (toX, toY), release.
	// UI thread.
	void BandForTest(float fromX, float fromY, float toX, float toY);

	// Re-validate the surface after a video reset of its mix: clear the cached
	// letterbox transform so the next frame recomputes it against the new base
	// resolution, and nudge a redraw. Returns false when no display exists yet.
	bool OnVideoReset();

	// Mouse editing entry points, invoked once a window message is mapped to this
	// surface. All run on the UI thread.
	void OnLeftDown(int mx, int my);
	void OnMouseMove(int mx, int my);
	void OnLeftUp();
	void CancelDrag();

	// The second press of a double click (WM_LBUTTONDBLCLK). The overlay's window class
	// sets CS_DBLCLKS, so this message REPLACES the WM_LBUTTONDOWN that press would
	// otherwise be. It enters ("drills into") the top-level group under the pointer, and
	// hands every other case straight to OnLeftDown -- which is what keeps the
	// click-through cycle, driven entirely by repeated presses, working. UI thread.
	void OnLeftDblClk(int mx, int my);

	// Leave the drilled-into group and select it: what Esc does, arriving over the bridge
	// because the overlay never receives keys. Returns whether one was entered. Idempotent.
	// The other ways out are a click or right-click outside the group's box, and the
	// implicit rules (scene switch, scene-collection change, the group removed, ungrouped or
	// no longer a group) that the surface applies whenever it resolves the drill-in. UI thread.
	bool ExitGroup();

	// Re-apply the implicit exit rules against the scene this surface shows now, emitting
	// sceneItem.selected when they ended the drill-in (that event is how the docks learn).
	// The one caller is the scene-list announcement: a switch driven from a hotkey or a dock
	// reaches no pointer handler, so nothing else would resolve the drill-in until the next
	// event over the preview. Safe on every scenes.changed -- a create or a rename leaves the
	// entered group's scene intact, so the resolve is a no-op -- and free when no group is
	// entered. UI thread.
	void RefreshEnteredGroup();

	// The one drag-end path, shared by every way a gesture can finish: the button-up,
	// a right-click that interrupts it, and a lost mouse capture. Clears the drag state
	// and the hover outline (a gesture can end with the pointer anywhere), and, when
	// the gesture resized an overlay item, re-lays-out that overlay's page to the box
	// it now occupies. Returns whether the drag had actually moved anything, so the
	// caller can gate its save on it. Idempotent. UI thread.
	bool FinishDrag();

	// Right-button-up: hit-test + select the item under the cursor (or clear), then
	// emit preview.contextMenu so JS can open a DOM context menu. UI thread.
	void OnRightUp(int mx, int my);

	// Mouse input off the overlay HWND, routed here by OverlaySurface's WndProc.
	bool OnOverlayMessage(UINT msg, WPARAM wparam, LPARAM lparam) override;

	// The overlay was hidden by any of OverlaySurface's routes: drop the hover
	// outline and cursor, which describe a pointer position that is no longer over
	// anything. UI thread.
	void OnOverlayHidden() override;

	// Per-surface impl state (selection/hover + letterbox transform shared with the
	// render thread, drag + cursor state, box buffer). Defined in the .cpp so this header
	// stays free of libobs + graphics types; declared here only so the .cpp's
	// render/mouse helpers can name it. Incomplete outside that TU.
	struct State;

private:
	// Apply the press deferred by OnLeftDown as a selection change. A press does not
	// select: pressing on an item that is already selected has to be able to drag the
	// WHOLE selection, so which of select / move / rubber-band the press meant is
	// settled by the first mouse-move or by the release. The first-move path already
	// holds the scene; the release path resolves it through the ...OnCurrentScene
	// overload. Both re-run the hit-test rather than reusing the press's, because the
	// click-through cycle reads the live selection. `bandPending` marks the first-move
	// call, where an empty hit under an additive modifier must NOT clear -- FinishBox
	// will set the selection, and clearing first would emit a second, empty change.
	// UI thread.
	void ApplyPressClick(obs_source_t *sceneSource, obs_scene_t *scene, bool bandPending);
	void ApplyPressClickOnCurrentScene();

	// Emit sceneItem.selected for `keys`, tagged with the group this surface is drilled
	// into. Every selection change goes through it so none of them can drop the tag.
	void EmitSelectionChange(const std::vector<SceneItemKey> &keys);

	// Drill into `groupItem` and select the topmost of its children under `canvasPos`, or
	// nothing when none is there. The double click's own press already selected the group.
	void EnterGroup(obs_source_t *sceneSource, obs_scene_t *scene, obs_sceneitem_t *groupItem,
			const vec2 &canvasPos);

	// Seed the letterbox transform at 1:1 for a scripted pointer sequence; shared by
	// DragForTest, ClickForTest and BandForTest.
	void SeedTestTransform();

	// Post one mouse message through OnOverlayMessage, the way the window procedure does,
	// so a scripted click exercises the real message routing and not just the handler
	// behind it. `msg` is a WM_* value (UINT, spelled as uint32_t so this header needs no
	// windows.h ordering assumption beyond the one it already has).
	void SendTestMouseMessage(uint32_t msg, int x, int y);

	// Begin dragging the whole current selection, recording each member's start
	// position and the one batch undo payload. No-op when nothing movable resolves.
	void BeginMove(obs_source_t *sceneSource, obs_scene_t *scene);

	// The rubber band's whole lifecycle: start one at the press position, abandon one
	// without committing, or commit one into the selection (returning whether a band
	// was in flight). The two enders are idempotent, so every capture-ending path can
	// call them unconditionally. FinishBox reads Shift/Ctrl/Alt at the RELEASE against a snapshot
	// taken at the press -- see its definition. UI thread.
	void BeginBox();
	void CancelBox();
	bool FinishBox();

	// Affordance feedback for a mouse position with no drag in progress: resolve
	// the gesture a press there would start, remember the item to outline, and
	// apply the matching cursor. ClearHoverItem drops the outline alone; ClearHover
	// drops it and also resets the remembered cursor shape without touching the
	// live cursor -- that second half only for the paths where the pointer is no
	// longer over the surface (it left, or the surface was hidden under it).
	// SetCursorShape applies a shape and remembers it, so WM_SETCURSOR can re-apply
	// that instead of the window class's arrow. UI thread.
	void UpdateHover(int mx, int my);
	void ClearHoverItem();
	void ClearHover();
	void SetCursorShape(const wchar_t *idc);

	// Zoom by `levelDelta` notches about the client pixel (px, py), which stays over
	// the same canvas point; switches the surface to a fixed scale. PanBy offsets an
	// already-fixed view by a client-px delta. Both clamp so the canvas cannot leave
	// the surface. EndPan is the pan gesture's terminator, the counterpart to
	// FinishDrag for item gestures -- idempotent, returns whether one was in flight.
	// Locked/FixedScaling read the view under the surface's lock. UI thread.
	void PanBy(int dx, int dy);
	bool EndPan();
	void EndPanOffSurface();
	void RetractPointerOver();
	bool Locked();
	bool FixedScaling();

	State *state_;

	obs_canvas_t *targetCanvas_; // null = Default surface (global mix, output 0)
	int windowId_ = 0;           // owning window id (0 = main); carried in preview.contextMenu
	OverlaySurface overlay_;
};

// Owns the native preview surfaces, keyed by (windowId, canvasUuid). windowId 0 is
// the main window; future additional windows carry windowId > 0. The
// empty/Default canvas uuid maps to a null-targetCanvas surface (global mix +
// output 0), byte-identical to the original single-preview behavior; any other
// uuid is resolved to its obs_canvas_t mix via CanvasRuntime on first use. The
// single live PreviewManager is reachable process-wide via Preview::Instance().
//
// Each window registers its host HWND (RegisterWindow); a surface's overlay HWND
// is parented to its window's host. windowId 0 falls back to the constructor's
// host_, so the main window need not register (main.cpp registers it anyway for
// symmetry). The windowId params default to 0, so every existing single-window
// caller is unchanged and the flag-off build is byte-identical.
//
// SetRect/Hide/Destroy run on the host UI thread (the bridge router callback
// thread). DestroyAll runs at teardown, while libobs is up and BEFORE the canvas
// mixes are freed -- an additional surface's display renders a canvas mix, so its
// obs_display must die before that mix. DestroyWindow(windowId) is the per-window
// teardown primitive WindowManager calls before a detached window's browser closes
// and before its canvas mix is freed.
class PreviewManager {
public:
	PreviewManager(HWND host, HINSTANCE instance);
	~PreviewManager();

	PreviewManager(const PreviewManager &) = delete;
	PreviewManager &operator=(const PreviewManager &) = delete;

	// Register/unregister a window's host HWND so this window's surfaces parent to
	// the right top-level window. windowId 0 (main) is optional -- it falls back to
	// the constructor's host_ when not registered.
	void RegisterWindow(int windowId, HWND host);
	void UnregisterWindow(int windowId);

	// Position/size the surface for (windowId, canvasUuid) (empty/Default uuid =>
	// the Default surface). Lazily creates the surface on first use; an unknown
	// non-Default uuid (no live canvas mix) is a no-op. The single-arg overloads
	// target the main window (windowId 0) so every existing caller is unchanged
	// (windowId can't be a defaulted leading param, so these are overloads).
	void SetRect(int windowId, const std::string &canvasUuid, int x, int y, int cx, int cy);
	void Hide(int windowId, const std::string &canvasUuid);
	void Destroy(int windowId, const std::string &canvasUuid);
	void SetRect(const std::string &canvasUuid, int x, int y, int cx, int cy)
	{
		SetRect(0, canvasUuid, x, y, cx, cy);
	}
	void Hide(const std::string &canvasUuid) { Hide(0, canvasUuid); }

	// Tear down this canvas's surface on EVERY window (display + overlay HWND),
	// erasing each from the registry. A detached window carries its own windowId,
	// so a canvas being removed can have surfaces beyond windowId 0; all of them
	// render the canvas mix and must die before that mix is freed (the UAF rule).
	void DestroyForCanvas(const std::string &canvasUuid);

	// Destroy every surface's display + overlay HWND. Called at teardown before the
	// canvas mixes (CanvasRuntime::ClearAll) and obs_shutdown.
	void DestroyAll();

	// Tear down every surface owned by one window (display + overlay HWND), erasing
	// each from the registry. WindowManager calls this for a detached windowId
	// before that window's browser closes and before its canvas mix is freed,
	// preserving the UAF rule (display dies before its mix). windowId 0 is the main
	// window (never passed here).
	void DestroyWindow(int windowId);

	// The surface for (windowId, canvasUuid), creating it if absent. Empty/Default
	// uuid => the Default surface (null targetCanvas). Returns null for a
	// non-Default uuid with no live canvas mix. Used by the bridge
	// select/hit-test/reset paths. The single-arg overload targets the main window.
	PreviewSurface *SurfaceFor(int windowId, const std::string &canvasUuid);

	// The same lookup WITHOUT the lazy creation, for readers: returns null rather
	// than standing up a surface and its HWND as the side effect of a query.
	PreviewSurface *FindSurface(int windowId, const std::string &canvasUuid);
	PreviewSurface *SurfaceFor(const std::string &canvasUuid) { return SurfaceFor(0, canvasUuid); }

	// Apply OnVideoReset to every live surface (global-mix reset). Runs on the UI
	// thread.
	void OnVideoResetAll();

	// The same re-validation limited to one canvas's surfaces, for a resolution or
	// color change applied to a single non-Default canvas -- which never touches the
	// global video info and so never reaches OnVideoResetAll.
	void OnVideoResetForCanvas(const std::string &canvasUuid);

	// PreviewSurface::RefreshEnteredGroup on every surface showing this canvas, across
	// windows for the reason DestroyForCanvas sweeps them. Runs on the UI thread.
	void RefreshEnteredGroupForCanvas(const std::string &canvasUuid);

	// The main window's top-level host HWND (windowId 0), set at construction. Used
	// to parent native modal dialogs (e.g. the file picker) to the app window.
	HWND MainHostHwnd() const { return host_; }

private:
	struct Impl;
	Impl *impl_; // pimpl: the surface list, kept out of this header
	HWND host_;
	HINSTANCE instance_;
};

// Process-wide accessor to the single live PreviewManager so the bridge methods
// (preview.setRect / preview.hide / preview.select) can reach it without threading
// a pointer through the dispatch registry. Set by main.cpp after libobs is up and
// cleared before teardown; both bridge and main run on the same UI thread.
namespace Preview {
void SetInstance(PreviewManager *pm);
PreviewManager *Instance();

// Drive selection from JS (the SourcesPanel) on the surface for (windowId, canvas)
// (empty canvas => the Default surface, output channel 0). `scene` is validated
// against that surface's current scene name (a mismatch is ignored, keeping
// "preview shows the current scene" intact). An empty `keys` clears. Emits
// sceneItem.selected like a mouse-driven change, and returns the set now selected
// (see PreviewSurface::SelectFromBridge), or nothing when there is no such surface or
// the scene does not match. Runs on the UI thread. windowId defaults to 0 (main window).
std::optional<std::vector<SceneItemKey>> SelectFromBridge(const std::string &canvas, const std::string &scene,
							  const std::vector<SceneItemKey> &keys, int windowId = 0);

// Hit-test at a canvas-space coordinate against the surface for (windowId, canvas)
// (empty canvas => the Default surface); returns the topmost matching scene-item
// id, or -1. Used by the smoke self-tests to prove hit-testing without a real
// cursor. windowId defaults to 0 (main window).
int64_t HitTestForTest(const std::string &canvas, float canvasX, float canvasY, int windowId = 0);

// PreviewSurface::DragForTest on the surface for (windowId, canvas) (empty canvas => the
// Default surface). False when no such surface exists yet -- this never creates one -- or as
// PreviewSurface::DragForTest is. windowId defaults to 0 (main window).
bool DragForTest(const std::string &canvas, const SceneItemKey &key, PreviewTestGesture gesture, float dx, float dy,
		 int windowId = 0, vec2 *outGrab = nullptr);

// Leave the group the surface for (windowId, canvas) is drilled into, selecting it. False
// when there is no such surface (none is created) or it was not in a group. This is what
// preview.exitGroup calls, which is how Esc reaches the preview: the overlay HWND never
// takes the keyboard focus, so the key is seen by the page and forwarded. UI thread.
bool ExitGroup(const std::string &canvas, int windowId = 0);

// Re-resolve the drill-in on every surface showing `canvas` (empty => the Default surface),
// which is how a scene switch reaches the preview: the implicit exit rules live in the
// surface's resolve, and a switch made from a hotkey or a dock touches no pointer handler.
// A no-op when there is no manager or no surface holds a drill-in. UI thread.
void RefreshEnteredGroupForCanvas(const std::string &canvas);

// Smoke self-test entry points, each on the surface for (windowId, canvas) and each a no-op
// (false / empty) when there is none -- they never create one. ClickForTest/BandForTest
// drive real pointer sequences; EnteredGroupForTest and SelectedKeysForTest read back what
// they left. UI thread.
bool ClickForTest(const std::string &canvas, float canvasX, float canvasY, bool doubleClick, int windowId = 0);
bool BandForTest(const std::string &canvas, float fromX, float fromY, float toX, float toY, int windowId = 0);
std::string EnteredGroupForTest(const std::string &canvas, int windowId = 0);
std::vector<SceneItemKey> SelectedKeysForTest(const std::string &canvas, int windowId = 0);

// Drive the preview's view (the Scale submenu) and its edit lock from JS, on the
// surface for (windowId, canvas) (empty canvas => the Default surface). Each
// returns false when no such surface exists, and ApplyViewAction also when the
// token is not one it knows. GetView fills the menu's checkmarks. UI thread.
bool ApplyViewAction(const std::string &canvas, const std::string &token, int windowId = 0);
bool SetLocked(const std::string &canvas, bool locked, int windowId = 0);
// Empty when there is no surface for this canvas and window.
std::optional<PreviewViewState> GetView(const std::string &canvas, int windowId = 0);

// The guide overlays as GeneralSettings holds them. FromSettings is empty when the
// stored overflow mode is not a token it knows; ToSettings writes all four fields.
std::optional<PreviewOverlays> OverlaysFromSettings(const GeneralSettings &settings);
void OverlaysToSettings(const PreviewOverlays &overlays, GeneralSettings &settings);

// Hand the overlays in `settings` to every surface: the only writer of what the draw
// callbacks read, which is held in atomics rather than behind any one surface's mutex.
// An unknown stored overflow mode is reset to the default first. Called once at startup
// after the settings load, and by the General-settings commit after every change. UI
// thread.
void LoadOverlays(GeneralSettings &settings);

// The wire tokens for PreviewOverflowMode ("hidden", "selection", "always"). FromToken
// is false for a token it does not know.
const char *OverflowModeToken(PreviewOverflowMode mode);
bool OverflowModeFromToken(const std::string &token, PreviewOverflowMode &out);

// Zoom about a point in the surface's own device-px client space. The second entry
// point into the one anchor implementation: the overlay handles the wheel natively
// (which is what keeps zoom working mid-drag, since a window holding the mouse
// capture is sent the wheel whatever the shell's routing setting says), while the
// web view forwards the wheel it receives through here (which is what makes zoom
// work at all when that setting routes to the focused window instead). Neither
// creates a surface.
bool ZoomAt(const std::string &canvas, int x, int y, int levelDelta, int windowId = 0);

// Re-validate every live surface after an obs_reset_video of the global mix. The
// obs_display swapchains + draw callbacks survive a video reset (it rebuilds the
// video mix, not the graphics device), so this just clears each cached letterbox
// transform so the next frame recomputes it against the new base resolution, and
// nudges a redraw. Runs on the UI thread.
void OnVideoReset();

// The per-canvas counterpart, for a non-Default canvas whose own mix was reset.
// Called from CanvasRuntime::ResetVideo so that every route to a canvas resolution
// change reaches the preview, and so a named canvas answers the change the same way
// the Default one does. Runs on the UI thread.
void OnCanvasVideoReset(const std::string &canvasUuid);
} // namespace Preview

#endif // OBS_MULTISTREAM_FRONTEND_PREVIEW_WINDOW_HPP_
