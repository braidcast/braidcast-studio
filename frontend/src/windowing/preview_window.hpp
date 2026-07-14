#ifndef OBS_MULTISTREAM_FRONTEND_PREVIEW_WINDOW_HPP_
#define OBS_MULTISTREAM_FRONTEND_PREVIEW_WINDOW_HPP_

#include <windows.h>

#include <cstdint>
#include <string>

struct obs_canvas;
typedef struct obs_canvas obs_canvas_t;

// One native preview surface: a borderless child HWND of the host (sibling above
// the CEF browser HWND, z-ordered on top) plus an obs_display attached to it. The
// obs_display is its own D3D11 swapchain libobs steps independently of the CEF
// browser's renderer, rendering one canvas's program scene with aspect-correct
// letterboxing.
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
class PreviewSurface {
public:
	// host: the top-level host window the overlay is parented to. targetCanvas:
	// the canvas mix this surface renders+edits, or null for the Default surface
	// (global mix + output channel 0). The canvas pointer is borrowed (owned by
	// CanvasRuntime); the surface's display must be destroyed before that mix.
	// windowId: the owning window id (0 = main), carried in preview.contextMenu so
	// JS filters the broadcast to the originating window.
	PreviewSurface(HWND host, HINSTANCE instance, obs_canvas_t *targetCanvas, int windowId);
	~PreviewSurface();

	PreviewSurface(const PreviewSurface &) = delete;
	PreviewSurface &operator=(const PreviewSurface &) = delete;

	// Position/size the overlay (device pixels, host-client coords) and resize the
	// obs_display. Creates the overlay HWND + display on first call. Zero/negative
	// size hides instead.
	void SetRect(int x, int y, int cx, int cy);

	// Hide the overlay HWND (keeps it + the display alive for the next SetRect).
	void Hide();

	// Destroy the obs_display (must run while libobs is up, before this surface's
	// canvas mix is freed) and the overlay HWND.
	void Destroy();

	// The overlay HWND, or null until the first SetRect. Used to map a window
	// message back to its owning surface.
	HWND Hwnd() const { return hwnd_; }

	// Drive selection from JS without a mouse event (the SourcesPanel). `scene` is
	// validated against the surface's current scene name (a mismatch is ignored);
	// when `hasId` is false the selection is cleared. Emits sceneItem.selected.
	bool SelectFromBridge(const std::string &scene, int64_t id, bool hasId);

	// Hit-test at a canvas-space coordinate against this surface's scene; returns
	// the topmost matching scene-item id, or -1. Used by the smoke self-test.
	int64_t HitTestForTest(float canvasX, float canvasY);

	// The currently-selected scene-item id on this surface (-1 when none). Used by
	// the isolation self-test to prove an edit on one surface leaves another's
	// selection untouched. Reads under the surface's own lock.
	int64_t SelectedIdForTest();

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

	// Right-button-up: hit-test + select the item under the cursor (or clear), then
	// emit preview.contextMenu so JS can open a DOM context menu. UI thread.
	void OnRightUp(int mx, int my);

	// Settle-timer callback (WM_TIMER on the overlay HWND): apply the last pending
	// rect from a rapid-resize burst and re-show the surface. UI thread. See SetRect.
	void OnResizeSettled();

	// Per-surface impl state (selection + letterbox transform shared with the
	// render thread, drag state, box buffer). Defined in the .cpp so this header
	// stays free of libobs + graphics types; declared here only so the .cpp's
	// render/mouse helpers can name it. Incomplete outside that TU.
	struct State;

private:
	// Create the overlay child HWND (no display) on first use. Idempotent.
	void EnsureCreated();

	// (Re)create the obs_display + its swapchain at (cx,cy) and register the draw
	// callback, when none exists yet. Hide() destroys the display so a suspended
	// overlay drops its swapchain; this rebuilds a fresh one on the next SetRect,
	// so a hidden-then-shown flip-model swapchain never resurfaces black (an
	// unchanged-size reshow would otherwise skip gs_resize and keep the occluded
	// buffers). No-op while a display already exists or before the HWND is created.
	void EnsureDisplay(int cx, int cy);

	// Remove the draw callback + destroy the obs_display (the swapchain), leaving
	// the overlay HWND intact. Shared by Hide() (drop swapchain on suspend) and
	// Destroy() (full teardown).
	void TeardownDisplay();

	// Position/size the overlay HWND + resize its obs_display to (cx,cy) and show it.
	// The synchronous SetWindowPos keeps the HWND tracking the DOM; the obs_display
	// resize + present lag is what SetRect's debounce hides during a resize burst.
	void ApplyRect(int x, int y, int cx, int cy);

	State *state_;

	HWND host_;
	HINSTANCE instance_;
	obs_canvas_t *targetCanvas_; // null = Default surface (global mix, output 0)
	int windowId_ = 0;           // owning window id (0 = main); carried in preview.contextMenu
	HWND hwnd_ = nullptr;        // overlay child HWND; null until first SetRect
	void *display_ = nullptr;    // obs_display_t* (opaque here)

	// Rapid-resize debounce (UI thread only). During a drag-resize the DOM fires a
	// rect every frame; the obs_display swapchain resize lags a frame or backlogs,
	// so the letterboxed video visibly trails the HWND. We hide the surface for the
	// duration of the burst and snap it to the final rect once the rects stop (the
	// settle timer fires). lastCx_/lastCy_ are the size last applied via ApplyRect
	// (0 = never applied → first rect shows immediately, no hide/delay).
	int lastCx_ = 0;
	int lastCy_ = 0;
	int pendingX_ = 0;
	int pendingY_ = 0;
	int pendingCx_ = 0;
	int pendingCy_ = 0;
	bool resizeDeferred_ = false;
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
	void Destroy(const std::string &canvasUuid) { Destroy(0, canvasUuid); }

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
	PreviewSurface *SurfaceFor(const std::string &canvasUuid) { return SurfaceFor(0, canvasUuid); }

	// Apply OnVideoReset to every live surface (global-mix reset). Runs on the UI
	// thread.
	void OnVideoResetAll();

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
// "preview shows the current scene" intact). When `hasId` is false the selection
// is cleared. Emits sceneItem.selected like a mouse-driven change. Runs on the UI
// thread. windowId defaults to 0 (main window).
bool SelectFromBridge(const std::string &canvas, const std::string &scene, int64_t id, bool hasId, int windowId = 0);

// Hit-test at a canvas-space coordinate against the surface for (windowId, canvas)
// (empty canvas => the Default surface); returns the topmost matching scene-item
// id, or -1. Used by the smoke self-tests to prove hit-testing without a real
// cursor. windowId defaults to 0 (main window).
int64_t HitTestForTest(const std::string &canvas, float canvasX, float canvasY, int windowId = 0);

// Re-validate every live surface after an obs_reset_video of the global mix. The
// obs_display swapchains + draw callbacks survive a video reset (it rebuilds the
// video mix, not the graphics device), so this just clears each cached letterbox
// transform so the next frame recomputes it against the new base resolution, and
// nudges a redraw. Runs on the UI thread.
void OnVideoReset();
} // namespace Preview

#endif // OBS_MULTISTREAM_FRONTEND_PREVIEW_WINDOW_HPP_
