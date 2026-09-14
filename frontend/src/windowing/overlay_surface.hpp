#ifndef OBS_MULTISTREAM_FRONTEND_OVERLAY_SURFACE_HPP_
#define OBS_MULTISTREAM_FRONTEND_OVERLAY_SURFACE_HPP_

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>

// A borderless child HWND of a host window -- a sibling of the CEF browser HWND,
// z-ordered above it -- with an obs_display attached to it. The obs_display is its
// own D3D11 swapchain libobs steps independently of the browser's renderer, so the
// video it presents lands over the DOM region the UI reports rather than inside it.
//
// This is the mechanism every in-page native surface shares: the main preview
// (PreviewSurface) and the Filters dialog's source preview (FilterPreview). What
// differs between them is only the draw callback and what, if anything, they do
// with input -- so both live outside this class.
//
// The HWND and the display are created lazily on the first SetRect, so the UI owns
// the geometry and a surface that is never shown costs nothing.
//
// Threading: every method here runs on the host UI thread. The registered draw
// callback runs on the libobs render thread; keeping `drawData` alive across the
// display's lifetime is the owner's job.
class OverlaySurface {
public:
	// libobs' draw-callback signature, respelled so this header stays free of obs.h.
	using DrawFn = void (*)(void *param, uint32_t cx, uint32_t cy);

	// Window messages the overlay HWND receives that this class does not consume
	// itself (it consumes only its resize-settle and warm-up messages). Implemented by an owner
	// that needs input off the surface; null for a display-only surface. WM_TIMER ids from
	// kWarmupTimerIdBase up are the warm-up's and never reach the sink.
	class MessageSink {
	public:
		virtual ~MessageSink() = default;

		// Return true when the message was handled -- the WndProc then replies
		// itself instead of falling through to DefWindowProc (0, except for
		// WM_SETCURSOR, whose contract makes TRUE the "stop here" reply). UI thread.
		virtual bool OnOverlayMessage(UINT msg, WPARAM wparam, LPARAM lparam) = 0;

		// The overlay HWND was hidden, by any route: Hide(), a zero-size SetRect, or
		// the mid-resize burst. Sinks holding state derived from the pointer being
		// over the surface drop it here, since the surface can be shown again with
		// the pointer never having moved. UI thread. Optional.
		virtual void OnOverlayHidden() {}
	};

	// host: the top-level window the overlay is parented to. draw/drawData: the
	// libobs draw callback registered on the display, and its param. sink: optional
	// input owner. tag: short subsystem name for the lifecycle log lines
	// ("preview", "filter-preview").
	OverlaySurface(HWND host, HINSTANCE instance, DrawFn draw, void *drawData, MessageSink *sink, std::string tag);
	~OverlaySurface();

	OverlaySurface(const OverlaySurface &) = delete;
	OverlaySurface &operator=(const OverlaySurface &) = delete;

	// Position/size the overlay (device pixels, host-client coords) and resize the
	// obs_display. Creates the HWND + display on first call. A zero/negative size
	// hides instead.
	void SetRect(int x, int y, int cx, int cy);

	// Hide the overlay HWND and drop its swapchain, keeping the HWND for the next
	// SetRect.
	void Hide();

	// Remove the draw callback, destroy the obs_display, destroy the HWND. Must run
	// while libobs is up and before anything the draw callback reads is freed.
	// Idempotent.
	void Destroy();

	HWND Hwnd() const { return hwnd_; }
	bool HasDisplay() const { return display_ != nullptr; }

	// The attached obs_display_t*, null until the first SetRect (and again after a
	// Hide/Destroy). Opaque here so this header stays free of obs.h; an owner that
	// needs to drive the display directly casts it back.
	void *Display() const { return display_; }

	// Dispatch one window message for this surface: the resize-settle and warm-up
	// messages are consumed here, everything else is offered to the sink. Returns true when the
	// message was handled. Public only so the shared WndProc (in the .cpp) can reach
	// it; nothing else calls it.
	bool HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);

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
	// the HWND intact. Shared by Hide() (drop swapchain on suspend) and Destroy().
	void TeardownDisplay();

	// Position/size the HWND + resize its obs_display to (cx,cy) and show it. The
	// synchronous SetWindowPos keeps the HWND tracking the DOM; the obs_display
	// resize + present lag is what SetRect's debounce hides during a resize burst.
	// When this creates the display, the HWND is shown beneath the web view and raised by
	// EndWarmup once a frame has presented, or when the warm-up timeout fires -- which can
	// raise it before its first frame, showing the class brush until one lands. When the
	// display cannot be created, the HWND is left hidden.
	void ApplyRect(int x, int y, int cx, int cy);

	// Raise a warming HWND to the top of the sibling z-order: the fresh display has
	// presented (DrawAndCount posted), or the warm-up timeout fired. No-op when not
	// warming. HandleMessage drops either message when its generation is not the
	// current display's.
	void EndWarmup();

	// Stop a warm-up without raising (its timer too). Returns whether one was running.
	bool CancelWarmup();

	// The draw callback actually registered on the display: runs the owner's draw_,
	// then counts calls so the UI thread learns when the first frame has presented.
	// Render thread.
	static void DrawAndCount(void *param, uint32_t cx, uint32_t cy);

	// Settle-timer callback (WM_TIMER on the overlay HWND): apply the last pending
	// rect from a rapid-resize burst and re-show the surface. See SetRect.
	void OnResizeSettled();

	// Tell the sink the HWND just went hidden. Called from every route that hides
	// it, so a sink's teardown is written once rather than per route.
	void NotifyHidden();

	HWND host_;
	HINSTANCE instance_;
	DrawFn draw_;
	void *drawData_;
	MessageSink *sink_;
	std::string tag_;

	HWND hwnd_ = nullptr;     // overlay child HWND; null until first SetRect
	void *display_ = nullptr; // obs_display_t* (opaque here)

	// Rapid-resize debounce. During a drag-resize the DOM fires a rect every frame;
	// the obs_display swapchain resize lags a frame or backlogs, so the letterboxed
	// video visibly trails the HWND. We hide the surface for the duration of the
	// burst and snap it to the final rect once the rects stop (the settle timer
	// fires). lastCx_/lastCy_ are the size last applied via ApplyRect (0 = never
	// applied, so the first rect shows immediately with no hide/delay).
	int lastCx_ = 0;
	int lastCy_ = 0;
	int pendingX_ = 0;
	int pendingY_ = 0;
	int pendingCx_ = 0;
	int pendingCy_ = 0;
	bool resizeDeferred_ = false;

	// Warm-up of a fresh display (see ApplyRect). warming_ is UI-thread state;
	// drawCalls_ is bumped on the render thread and reset per display; displayGen_
	// tags the posted "presented" message and the timeout's timer id so either one from a
	// torn-down display is dropped.
	bool warming_ = false;
	std::atomic<uint32_t> drawCalls_{0};
	std::atomic<uint32_t> displayGen_{0};

	// The size whose display create last failed, so a retry at that size is not logged
	// again. 0x0 once a create succeeds.
	int failedCx_ = 0;
	int failedCy_ = 0;
};

#endif // OBS_MULTISTREAM_FRONTEND_OVERLAY_SURFACE_HPP_
