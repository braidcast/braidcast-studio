#ifndef OBS_MULTISTREAM_FRONTEND_OVERLAY_SURFACE_HPP_
#define OBS_MULTISTREAM_FRONTEND_OVERLAY_SURFACE_HPP_

#include <windows.h>

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
	// itself (it consumes only its resize-settle timer). Implemented by an owner
	// that needs input off the surface; null for a display-only surface.
	class MessageSink {
	public:
		virtual ~MessageSink() = default;

		// Return true when the message was handled -- the WndProc then returns 0
		// instead of falling through to DefWindowProc. UI thread.
		virtual bool OnOverlayMessage(UINT msg, WPARAM wparam, LPARAM lparam) = 0;
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

	// Dispatch one window message for this surface: the resize-settle timer is
	// consumed here, everything else is offered to the sink. Returns true when the
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
	void ApplyRect(int x, int y, int cx, int cy);

	// Settle-timer callback (WM_TIMER on the overlay HWND): apply the last pending
	// rect from a rapid-resize burst and re-show the surface. See SetRect.
	void OnResizeSettled();

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
};

#endif // OBS_MULTISTREAM_FRONTEND_OVERLAY_SURFACE_HPP_
