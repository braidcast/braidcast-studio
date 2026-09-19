#include "overlay_surface.hpp"

#include <obs.h>

#include "log.hpp"

namespace {

constexpr wchar_t kOverlayClassName[] = L"BraidcastOverlay";

// Rapid-resize debounce (SetRect): while a drag-resize keeps changing the rect the
// overlay is hidden; the surface snaps to the final rect this long after the last
// rect arrives. ~100ms reliably reads as "resize stopped" against the DOM's ~16ms
// per-frame cadence.
constexpr UINT_PTR kResizeSettleTimerId = 1;
constexpr UINT kResizeSettleMs = 100;

// Warm-up after a fresh display (ApplyRect): the HWND is shown beneath the web view
// and raised once the swapchain has presented. The draw callback runs before its
// frame's Present, so the second call is the first proof a frame has landed. The
// timeout raises regardless, so a display the render thread never reaches cannot
// leave the surface buried. The web view holds its still for RELEASE_HOLD_MS
// (previewFreeze.svelte.ts), a margin over this bound rather than a guarantee, since
// the timer starts only once the display exists; raise one and the other must follow.
//
// Both warm-up messages carry the display generation, the timer in its id. KillTimer does
// not withdraw a WM_TIMER already posted, so an untagged expiry dispatched after a Hide
// and a fresh ApplyRect would raise the new display before its first frame.
constexpr uint32_t kWarmupDrawCalls = 2;
constexpr UINT kWarmupTimeoutMs = 250;
constexpr UINT kWarmupPresentedMsg = WM_APP + 1;
constexpr UINT_PTR kWarmupTimerIdBase = 0x100; // above kResizeSettleTimerId

UINT_PTR WarmupTimerId(uint32_t generation)
{
	return kWarmupTimerIdBase + generation;
}

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
		// WM_SETCURSOR is the one message whose "handled" reply is not 0: it takes
		// TRUE to halt further processing, and that halt is what keeps the default
		// proc from restoring the class cursor over the one the sink just set.
		return msg == WM_SETCURSOR ? TRUE : 0;
	}
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// The class brush fills the HWND whenever its swapchain has nothing presented. It is
// black so that, when it does show, it matches the swapchain's own black letterbox
// rather than reading as a different surface. It normally stays covered: ApplyRect
// keeps a fresh HWND beneath the web view until the swapchain has presented. It can
// show on top when the warm-up timeout raises a display that has not presented yet.
// A resize burst's settle also reshows the HWND before the resized frame presents;
// what that frame shows has not been observed.
ATOM RegisterOverlayClass(HINSTANCE instance)
{
	static ATOM atom = 0;
	if (atom) {
		return atom;
	}
	WNDCLASSEXW wc = {0};
	wc.cbSize = sizeof(wc);
	// CS_DBLCLKS is what makes drill-in possible: without it Windows never sends
	// WM_LBUTTONDBLCLK and a second quick press is just another WM_LBUTTONDOWN. With it,
	// that second press arrives as WM_LBUTTONDBLCLK INSTEAD -- so every sink must route
	// that message or the repeated-click behaviour it used to get (the preview's
	// click-through cycle) silently stops working. See PreviewSurface::OnLeftDblClk.
	wc.style = CS_DBLCLKS;
	wc.lpfnWndProc = OverlayWndProc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
	wc.lpszClassName = kOverlayClassName;
	atom = RegisterClassExW(&wc);
	return atom;
}

} // namespace

void OverlaySurface::NotifyHidden()
{
	if (sink_) {
		sink_->OnOverlayHidden();
	}
}

bool OverlaySurface::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam)
{
	if (msg == WM_TIMER && wparam == kResizeSettleTimerId) {
		OnResizeSettled();
		return true;
	}
	const bool warmupTimer = msg == WM_TIMER && wparam >= kWarmupTimerIdBase;
	if (warmupTimer || msg == kWarmupPresentedMsg) {
		// One from a display torn down since is stale; its successor has a new
		// generation and is still warming.
		const uint32_t gen = displayGen_.load();
		if (warmupTimer ? wparam == WarmupTimerId(gen) : wparam == gen) {
			EndWarmup();
		}
		return true;
	}
	return sink_ && sink_->OnOverlayMessage(msg, wparam, lparam);
}

void OverlaySurface::DrawAndCount(void *param, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<OverlaySurface *>(param);
	self->draw_(self->drawData_, cx, cy);
	// hwnd_ is safe to read here: it is set before the display is created and cleared
	// only after TeardownDisplay has removed this callback under libobs' draw mutex.
	if (++self->drawCalls_ == kWarmupDrawCalls) {
		PostMessageW(self->hwnd_, kWarmupPresentedMsg, WPARAM(self->displayGen_.load()), 0);
	}
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
	if (display) {
		HostLog("[" + tag_ + "] obs_display_create -> OK");
		displayGen_.fetch_add(1);
		drawCalls_.store(0);
		obs_display_add_draw_callback(display, DrawAndCount, this);
		HostLog("[" + tag_ + "] draw callback registered");
	}
}

void OverlaySurface::ApplyRect(int x, int y, int cx, int cy)
{
	if (!hwnd_) {
		return;
	}
	// A fresh display has nothing on screen until its first Present, and a raised HWND
	// shows its class brush until then -- a black frame over whatever the web view was
	// holding in its place. So show it at the BOTTOM of the sibling z-order, where the
	// CEF browser HWND covers it, and raise it once a frame has landed. It warms shown
	// rather than hidden because a swapchain presenting into an SW_HIDE window is the
	// case Hide() found coming back black, and it is shown before the display exists
	// because the render thread may present the moment the display is created.
	const bool freshDisplay = !display_;
	if (freshDisplay) {
		warming_ = true;
	}
	SetWindowPos(hwnd_, warming_ ? HWND_BOTTOM : HWND_TOP, x, y, cx, cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);

	// Rebuild the swapchain if a prior Hide() dropped it, then size it to the rect.
	EnsureDisplay(cx, cy);
	if (freshDisplay) {
		if (display_) {
			failedCx_ = 0;
			failedCy_ = 0;
			SetTimer(hwnd_, WarmupTimerId(displayGen_.load()), kWarmupTimeoutMs, nullptr);
		} else {
			// Nothing will ever present into this HWND; raised, it would be a black
			// rectangle over the UI. Leave it hidden. lastCx_/lastCy_ still update
			// below, so the next same-size SetRect lands here again and retries -- on
			// any scroll anywhere in the app, so the failure is logged once per size.
			CancelWarmup();
			ShowWindow(hwnd_, SW_HIDE);
			NotifyHidden();
			if (cx != failedCx_ || cy != failedCy_) {
				failedCx_ = cx;
				failedCy_ = cy;
				HostLog("[" + tag_ + "] display create FAILED at " + std::to_string(cx) + "x" +
					std::to_string(cy) + " (rect " + std::to_string(x) + "," + std::to_string(y) +
					"); overlay left hidden, retries at this size not logged. "
					"obs_display_init's error is in the libobs log");
			}
		}
	}
	if (display_) {
		obs_display_resize(static_cast<obs_display_t *>(display_), uint32_t(cx), uint32_t(cy));
	}
	lastCx_ = cx;
	lastCy_ = cy;
}

bool OverlaySurface::CancelWarmup()
{
	if (!warming_) {
		return false;
	}
	warming_ = false;
	if (hwnd_) {
		KillTimer(hwnd_, WarmupTimerId(displayGen_.load()));
	}
	return true;
}

void OverlaySurface::EndWarmup()
{
	if (!CancelWarmup() || !hwnd_) {
		return;
	}
	// No SWP_SHOWWINDOW: a mid-resize burst may have hidden the HWND meanwhile, and
	// its settle is what shows it again.
	SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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
		NotifyHidden();
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
		NotifyHidden();
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
	CancelWarmup();
	if (display_) {
		obs_display_t *display = static_cast<obs_display_t *>(display_);
		obs_display_remove_draw_callback(display, DrawAndCount, this);
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
