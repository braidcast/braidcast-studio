#include "overlay_surface.hpp"

#include <obs.h>

#include "log.hpp"

namespace {

constexpr wchar_t kOverlayClassName[] = L"BraidcastOverlay";

// Rapid-resize debounce (SetRect): while a drag-resize keeps changing the rect the
// overlay is hidden; the surface snaps to the final rect this long after the last
// rect arrives. ~100ms reliably reads as "resize stopped" against the DOM's ~16ms
// per-frame cadence. The timer is per-overlay-HWND, which has no other timers.
constexpr UINT_PTR kResizeSettleTimerId = 1;
constexpr UINT kResizeSettleMs = 100;

// Route a window message to the surface that owns the HWND (stashed in
// GWLP_USERDATA at creation). Null for a foreign HWND.
OverlaySurface *SurfaceFromHwnd(HWND hwnd)
{
	return reinterpret_cast<OverlaySurface *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	OverlaySurface *surface = SurfaceFromHwnd(hwnd);
	if (surface && surface->HandleMessage(msg, wparam, lparam)) {
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// The overlay HWND uses a no-background class: the obs_display swapchain paints
// every pixel (the video plus its black letterbox bars), so a WM_ERASEBKGND fill
// would only flicker against it.
ATOM RegisterOverlayClass(HINSTANCE instance)
{
	static ATOM atom = 0;
	if (atom) {
		return atom;
	}
	WNDCLASSEXW wc = {0};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = OverlayWndProc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
	wc.lpszClassName = kOverlayClassName;
	atom = RegisterClassExW(&wc);
	return atom;
}

} // namespace

bool OverlaySurface::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam)
{
	if (msg == WM_TIMER && wparam == kResizeSettleTimerId) {
		OnResizeSettled();
		return true;
	}
	return sink_ && sink_->OnOverlayMessage(msg, wparam, lparam);
}

OverlaySurface::OverlaySurface(HWND host, HINSTANCE instance, DrawFn draw, void *drawData, MessageSink *sink,
			       std::string tag)
	: host_(host),
	  instance_(instance),
	  draw_(draw),
	  drawData_(drawData),
	  sink_(sink),
	  tag_(std::move(tag))
{
}

OverlaySurface::~OverlaySurface()
{
	Destroy();
}

void OverlaySurface::EnsureCreated()
{
	if (hwnd_) {
		return;
	}

	RegisterOverlayClass(instance_);

	// Borderless child sibling of the CEF browser HWND. WS_CLIPSIBLINGS keeps the
	// browser from overdrawing into it (the browser HWND also sets it). Starts
	// hidden; SetRect shows it after positioning so no frame flashes at 0,0. The
	// surface pointer is stashed in GWLP_USERDATA so the shared WndProc can map this
	// HWND back to its surface without a global table.
	hwnd_ = CreateWindowExW(0, kOverlayClassName, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 16, 16, host_, nullptr,
				instance_, nullptr);
	if (!hwnd_) {
		HostLog("[" + tag_ + "] overlay HWND create FAILED");
		return;
	}
	SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
	HostLog("[" + tag_ + "] overlay HWND created");
}

void OverlaySurface::EnsureDisplay(int cx, int cy)
{
	if (display_ || !hwnd_) {
		return;
	}

	// Create the swapchain at the target size so the fresh display presents at the
	// right resolution immediately (a following obs_display_resize to the same size
	// is a no-op). ApplyRect always feeds a positive rect here (zero/negative sizes
	// hide in SetRect before reaching ApplyRect).
	gs_init_data init = {};
	init.cx = uint32_t(cx);
	init.cy = uint32_t(cy);
	init.format = GS_BGRA;
	init.zsformat = GS_ZS_NONE;
	init.window.hwnd = hwnd_; // child HWND passthrough (gs_window.hwnd is void*)

	obs_display_t *display = obs_display_create(&init, 0x000000);
	display_ = display;
	HostLog("[" + tag_ + "] obs_display_create -> " + (display ? "OK" : "NULL"));
	if (display) {
		obs_display_add_draw_callback(display, draw_, drawData_);
		HostLog("[" + tag_ + "] draw callback registered");
	}
}

void OverlaySurface::ApplyRect(int x, int y, int cx, int cy)
{
	if (!hwnd_) {
		return;
	}
	// Position in host-client device pixels and keep above the CEF browser HWND
	// (HWND_TOP raises within the sibling z-order). SWP_SHOWWINDOW reveals it on
	// the first sized call.
	SetWindowPos(hwnd_, HWND_TOP, x, y, cx, cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);

	// Rebuild the swapchain if a prior Hide() dropped it, then size it to the rect.
	EnsureDisplay(cx, cy);
	if (display_) {
		obs_display_resize(static_cast<obs_display_t *>(display_), uint32_t(cx), uint32_t(cy));
	}
	lastCx_ = cx;
	lastCy_ = cy;
}

void OverlaySurface::SetRect(int x, int y, int cx, int cy)
{
	if (cx <= 0 || cy <= 0) {
		Hide();
		return;
	}

	EnsureCreated();
	if (!hwnd_) {
		return;
	}

	// A pure move (same size), or the very first rect after creation/hide, applies
	// immediately so the surface appears without a debounce delay and a drag keeps
	// the video tracking. Only a size change during a burst is deferred.
	const bool sizeChanged = (cx != lastCx_ || cy != lastCy_);
	const bool firstRect = (lastCx_ == 0 && lastCy_ == 0);
	if (firstRect || !sizeChanged) {
		if (resizeDeferred_) {
			resizeDeferred_ = false;
			KillTimer(hwnd_, kResizeSettleTimerId);
		}
		ApplyRect(x, y, cx, cy);
		return;
	}

	// Mid-resize: hide the overlay so its trailing/half-resized swapchain never shows
	// over the DOM, stash the target rect, and (re)arm the settle timer. The final
	// rect lands in OnResizeSettled once the rects stop for kResizeSettleMs. Hide the
	// HWND directly (not Hide()) so lastCx_/lastCy_ stay as the burst's size baseline.
	if (IsWindowVisible(hwnd_)) {
		ShowWindow(hwnd_, SW_HIDE);
	}
	pendingX_ = x;
	pendingY_ = y;
	pendingCx_ = cx;
	pendingCy_ = cy;
	resizeDeferred_ = true;
	SetTimer(hwnd_, kResizeSettleTimerId, kResizeSettleMs, nullptr);
}

void OverlaySurface::OnResizeSettled()
{
	if (!hwnd_) {
		return;
	}
	KillTimer(hwnd_, kResizeSettleTimerId);
	if (!resizeDeferred_) {
		return;
	}
	resizeDeferred_ = false;
	ApplyRect(pendingX_, pendingY_, pendingCx_, pendingCy_);
}

void OverlaySurface::Hide()
{
	// Cancel any pending resize snap so a settle timer doesn't re-show a surface the
	// caller just hid (modal/suspend/tab-hidden). Reset the size baseline so the next
	// SetRect re-applies immediately (firstRect path) rather than waiting a debounce.
	if (resizeDeferred_ && hwnd_) {
		resizeDeferred_ = false;
		KillTimer(hwnd_, kResizeSettleTimerId);
	}
	lastCx_ = 0;
	lastCy_ = 0;
	if (hwnd_) {
		ShowWindow(hwnd_, SW_HIDE);
	}

	// Drop the swapchain while hidden. A flip-model swapchain presented to an
	// SW_HIDE window latches an occluded/black state that a same-size reshow never
	// clears (render_display_begin skips gs_resize when the size is unchanged, and
	// Present ignores DXGI_STATUS_OCCLUDED). Destroying it here forces ApplyRect to
	// build a fresh one on the next show, mirroring stock OBS's recreate-on-show.
	TeardownDisplay();
}

void OverlaySurface::TeardownDisplay()
{
	if (display_) {
		obs_display_t *display = static_cast<obs_display_t *>(display_);
		obs_display_remove_draw_callback(display, draw_, drawData_);
		obs_display_destroy(display);
		display_ = nullptr;
		HostLog("[" + tag_ + "] display destroyed");
	}
}

void OverlaySurface::Destroy()
{
	TeardownDisplay();
	if (hwnd_) {
		// Kill a pending settle timer before the HWND goes away (defensive; the timer
		// dies with the window regardless), and clear the back-pointer so a WM_DESTROY
		// this triggers cannot re-enter a half-destroyed surface.
		if (resizeDeferred_) {
			KillTimer(hwnd_, kResizeSettleTimerId);
			resizeDeferred_ = false;
		}
		SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
		DestroyWindow(hwnd_);
		hwnd_ = nullptr;
	}
	lastCx_ = 0;
	lastCy_ = 0;
}
