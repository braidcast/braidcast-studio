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
	PreviewSurface(HWND host, HINSTANCE instance, obs_canvas_t *targetCanvas);
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

	// Per-surface impl state (selection + letterbox transform shared with the
	// render thread, drag state, box buffer). Defined in the .cpp so this header
	// stays free of libobs + graphics types; declared here only so the .cpp's
	// render/mouse helpers can name it. Incomplete outside that TU.
	struct State;

private:
	void EnsureCreated();

	State *state_;

	HWND host_;
	HINSTANCE instance_;
	obs_canvas_t *targetCanvas_; // null = Default surface (global mix, output 0)
	HWND hwnd_ = nullptr;        // overlay child HWND; null until first SetRect
	void *display_ = nullptr;    // obs_display_t* (opaque here)
};

// Owns the native preview surfaces, one per canvas, keyed by canvas uuid. The
// empty/Default canvas uuid maps to a null-targetCanvas surface (global mix +
// output 0), byte-identical to the original single-preview behavior; any other
// uuid is resolved to its obs_canvas_t mix via CanvasRuntime on first use. The
// single live PreviewManager is reachable process-wide via Preview::Instance().
//
// SetRect/Hide/Destroy run on the host UI thread (the bridge router callback
// thread). DestroyAll runs at teardown, while libobs is up and BEFORE the canvas
// mixes are freed -- an additional surface's display renders a canvas mix, so its
// obs_display must die before that mix.
class PreviewManager {
public:
	PreviewManager(HWND host, HINSTANCE instance);
	~PreviewManager();

	PreviewManager(const PreviewManager &) = delete;
	PreviewManager &operator=(const PreviewManager &) = delete;

	// Position/size the surface for `canvasUuid` (empty/Default uuid => the Default
	// surface). Lazily creates the surface on first use; an unknown non-Default
	// uuid (no live canvas mix) is a no-op.
	void SetRect(const std::string &canvasUuid, int x, int y, int cx, int cy);
	void Hide(const std::string &canvasUuid);
	void Destroy(const std::string &canvasUuid);

	// Destroy every surface's display + overlay HWND. Called at teardown before the
	// canvas mixes (CanvasRuntime::ClearAll) and obs_shutdown.
	void DestroyAll();

	// The surface for `canvasUuid`, creating it if absent. Empty/Default uuid =>
	// the Default surface (null targetCanvas). Returns null for a non-Default uuid
	// with no live canvas mix. Used by the bridge select/hit-test/reset paths.
	PreviewSurface *SurfaceFor(const std::string &canvasUuid);

	// Apply OnVideoReset to every live surface (global-mix reset). Runs on the UI
	// thread.
	void OnVideoResetAll();

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

// Drive selection from JS (the SourcesPanel) on the surface for `canvas` (empty =>
// the Default surface, output channel 0). `scene` is validated against that
// surface's current scene name (a mismatch is ignored, keeping "preview shows the
// current scene" intact). When `hasId` is false the selection is cleared. Emits
// sceneItem.selected like a mouse-driven change. Runs on the UI thread.
bool SelectFromBridge(const std::string &canvas, const std::string &scene, int64_t id, bool hasId);

// Hit-test at a canvas-space coordinate against the surface for `canvas` (empty =>
// the Default surface); returns the topmost matching scene-item id, or -1. Used by
// the smoke self-tests to prove hit-testing without a real cursor.
int64_t HitTestForTest(const std::string &canvas, float canvasX, float canvasY);

// Re-validate every live surface after an obs_reset_video of the global mix. The
// obs_display swapchains + draw callbacks survive a video reset (it rebuilds the
// video mix, not the graphics device), so this just clears each cached letterbox
// transform so the next frame recomputes it against the new base resolution, and
// nudges a redraw. Runs on the UI thread.
void OnVideoReset();
} // namespace Preview

#endif // OBS_MULTISTREAM_FRONTEND_PREVIEW_WINDOW_HPP_
