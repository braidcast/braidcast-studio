#include "window_manager.hpp"
#include "event_names.hpp"

#include "include/cef_browser.h"

#include "app_icon.hpp"
#include "bridge.hpp"
#include "client.hpp"
#include "log.hpp"
#include "native_theme.hpp"
#include "overlay_surface.hpp"
#include "preview_window.hpp"
#include "window_chrome.hpp"

namespace {
constexpr wchar_t kDetachedClassName[] = L"BraidcastDetached";
WindowManager *g_wm = nullptr;

// The paint watchdog on a detached host (see the lifetime rule in the header).
constexpr UINT_PTR kPaintWatchdogTimerId = 1;
// How long a detached page may take to report window.ready before the watchdog logs that
// it has not painted. Past a slow first paint: a warm detach logs its page's first preview
// rect ~0.2 s after the browser is created, and the main window's cold start (renderer
// spawn plus bundle parse) loads in ~1.05 s.
constexpr UINT kPaintWatchdogMs = 1500;

ATOM RegisterDetachedClass(HINSTANCE instance)
{
	static ATOM atom = 0;
	if (atom) {
		return atom;
	}
	WNDCLASSEXW wc = {0};
	wc.cbSize = sizeof(wc);
	// Detached windows host child render surfaces just like the main host window, so the
	// same anti-flash setup applies: no full-window CS_HREDRAW/VREDRAW invalidation (see
	// CreateHostWindow in main.cpp). The brush is the page's own base color, so any client
	// area the page has not painted reads as the app background rather than as whatever
	// the redirection surface last held. The class owns the brush for the process lifetime.
	wc.style = 0;
	wc.lpfnWndProc = WindowManager::WndProc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = CreateSolidBrush(RGB(CefColorGetR(kAppBackgroundColor), CefColorGetG(kAppBackgroundColor),
						CefColorGetB(kAppBackgroundColor)));
	wc.lpszClassName = kDetachedClassName;
	ApplyAppIcon(wc, instance);
	atom = RegisterClassExW(&wc);
	return atom;
}

// Whether this process owns the foreground window, i.e. the user is still working in the
// app. A detached window shown after the user has moved to another app does not take
// activation from it.
bool ForegroundIsOurs()
{
	DWORD pid = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	return pid == GetCurrentProcessId();
}

std::string RectText(HWND hwnd)
{
	RECT rc = {};
	GetWindowRect(hwnd, &rc);
	return std::to_string(rc.left) + "," + std::to_string(rc.top) + " " + std::to_string(rc.right - rc.left) + "x" +
	       std::to_string(rc.bottom - rc.top);
}
} // namespace

WindowManager::WindowManager(HINSTANCE instance, const std::string &bundleBaseUrl)
	: instance_(instance),
	  bundleBaseUrl_(bundleBaseUrl)
{
	RegisterDetachedClass(instance_);
}

WindowManager::~WindowManager()
{
	DestroyAll();
}

WindowManager *WindowManager::Instance()
{
	return g_wm;
}
void WindowManager::SetInstance(WindowManager *wm)
{
	g_wm = wm;
}

WindowManager::Window *WindowManager::FindByHwnd(HWND hwnd)
{
	for (Window &w : windows_) {
		if (w.hwnd == hwnd) {
			return &w;
		}
	}
	return nullptr;
}

WindowManager::Window *WindowManager::FindById(int windowId)
{
	for (Window &w : windows_) {
		if (w.windowId == windowId) {
			return &w;
		}
	}
	return nullptr;
}

void WindowManager::ReleaseWindow(const Window &w)
{
	if (w.hwnd) {
		KillTimer(w.hwnd, kPaintWatchdogTimerId);
	}
	// Surfaces must die before the browser/window (UAF discipline); drop the host-HWND
	// registration once they are gone.
	if (PreviewManager *pm = Preview::Instance()) {
		pm->DestroyWindow(w.windowId);
		pm->UnregisterWindow(w.windowId);
	}
	// Forget the gate with the host: an HWND value can be reused by a later window.
	if (w.hwnd) {
		OverlaySurface::ForgetHostGate(w.hwnd);
	}
	if (w.browser) {
		w.browser->GetHost()->CloseBrowser(true);
	}
}

void WindowManager::OpenGate(Window &w)
{
	if (w.hwnd) {
		KillTimer(w.hwnd, kPaintWatchdogTimerId);
	}
	if (w.ready) {
		return;
	}
	w.ready = true;
	DBG(LogCat::Lifecycle, "[window] gate open id=%d host=%s", w.windowId, RectText(w.hwnd).c_str());
	OverlaySurface::SetHostReady(w.hwnd, true);
	OverlaySurface::LogHostChildren(w.hwnd, "gate open");
}

bool WindowManager::MarkReady(int windowId)
{
	Window *w = closingAll_ ? nullptr : FindById(windowId);
	if (!w) {
		DBG(LogCat::Lifecycle, "[window] ready id=%d ignored (no live detached window)", windowId);
		return false;
	}
	OpenGate(*w);
	return true;
}

bool WindowManager::IsReadyForTest(int windowId) const
{
	const Window *w = FindById(windowId);
	return w && w->ready;
}

void WindowManager::OnRendererTerminated(CefRefPtr<CefBrowser> browser)
{
	for (Window &w : windows_) {
		if (!w.browser || !w.browser->IsSame(browser)) {
			continue;
		}
		if (PreviewManager *pm = Preview::Instance()) {
			pm->DestroyWindow(w.windowId);
		}
		w.ready = false;
		OverlaySurface::SetHostReady(w.hwnd, false);
		SetTimer(w.hwnd, kPaintWatchdogTimerId, kPaintWatchdogMs, nullptr);
		HostLog("[window] renderer terminated id=" + std::to_string(w.windowId) +
			": surfaces dropped, gate closed until the page reports ready");
		return;
	}
}

int WindowManager::Detach(const std::string &dockId)
{
	// The child browser MUST reuse the shared Client so it joins the same
	// browser_list_ + Bridge emit registry as the main window. A fresh Client
	// would carry its own browser_list_ and quit the loop when this window alone
	// closes -- the exact bug the one-Client-N-browsers rule guards against.
	CefRefPtr<Client> client = Client::Shared();
	if (!client) {
		HostLog("[window] Detach FAILED: no shared Client (main browser not created?)");
		return 0;
	}

	const int windowId = nextId_++;

	RECT rc = {0, 0, 960, 540};
	const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
	AdjustWindowRect(&rc, style, FALSE);
	HWND hwnd = CreateWindowExW(0, kDetachedClassName, L"Braidcast — Detached", style, CW_USEDEFAULT, CW_USEDEFAULT,
				    rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, instance_, nullptr);
	if (!hwnd) {
		HostLog("[window] CreateWindowExW FAILED for windowId=" + std::to_string(windowId));
		return 0;
	}
	// Same frameless command-deck chrome as the main shell, with a smaller size floor.
	WindowChrome::Init(hwnd, WindowChrome::kDetachedWindow);
	// Force the residual native border/system menu dark, matching the host shell.
	NativeTheme::ApplyDark(hwnd);
	// Shown at once: the gate, not the window's visibility, keeps the preview off a page
	// that has not painted, and a page in a hidden host may not run the animation frames
	// window.ready waits on. Until it paints, the class brush fills the client area. A detach the user did not
	// start from the foreground app does not take activation from the app they moved to.
	const bool activate = ForegroundIsOurs();
	ShowWindow(hwnd, activate ? SW_SHOW : SW_SHOWNA);
	UpdateWindow(hwnd);
	DBG(LogCat::Lifecycle, "[window] id=%d host shown at %s dpi=%u activated=%d", windowId, RectText(hwnd).c_str(),
	    GetDpiForWindow(hwnd), int(activate));

	const std::string url = bundleBaseUrl_ + "?window=" + std::to_string(windowId) + "&dock=" + dockId;

	CefBrowserSettings browser_settings;
	browser_settings.background_color = kAppBackgroundColor;
	CefWindowInfo window_info;
	InitChildBrowserWindowInfo(window_info, hwnd);

	CefRefPtr<CefBrowser> browser =
		CefBrowserHost::CreateBrowserSync(window_info, client, url, browser_settings, nullptr, nullptr);
	if (!browser) {
		HostLog("[window] CreateBrowserSync FAILED for windowId=" + std::to_string(windowId));
		::DestroyWindow(hwnd);
		return 0;
	}
	const HWND browserHwnd = browser->GetHost()->GetWindowHandle();
	OverlaySurface::LetPaintUnder(browserHwnd);
	// Keyboard input goes to the page, as it would after a click into it.
	if (activate) {
		browser->GetHost()->SetFocus(true);
	}
	DBG(LogCat::Lifecycle, "[window] id=%d browser hwnd=%p at %s visible=%d", windowId,
	    static_cast<void *>(browserHwnd), RectText(browserHwnd).c_str(),
	    browserHwnd ? int(IsWindowVisible(browserHwnd)) : -1);

	// Hold this window's preview surfaces until the page is ready, before the page can
	// send its first rect.
	OverlaySurface::SetHostReady(hwnd, false);

	// Register this window's host HWND so its preview surfaces parent to THIS
	// top-level window (its overlay HWND becomes a child of hwnd, z-ordered above
	// the child CEF browser), not the main window.
	if (PreviewManager *pm = Preview::Instance()) {
		pm->RegisterWindow(windowId, hwnd);
	}

	windows_.push_back(Window{windowId, dockId, hwnd, browser});
	SetTimer(hwnd, kPaintWatchdogTimerId, kPaintWatchdogMs, nullptr);
	HostLog("[window] detached id=" + std::to_string(windowId) + " dock=" + dockId + " url=" + url);
	return windowId;
}

bool WindowManager::Redock(int windowId)
{
	for (auto it = windows_.begin(); it != windows_.end(); ++it) {
		if (it->windowId != windowId) {
			continue;
		}
		HostLog("[window] redock id=" + std::to_string(windowId));
		// Synchronous teardown in the same order as the WM_CLOSE path (surfaces ->
		// browser -> window), so window.list reflects the removal the instant Redock
		// returns rather than after a posted WM_CLOSE drains. Surfaces go FIRST (while
		// libobs is up, before the browser dies) so the obs_display is gone before its
		// canvas mix could be freed.
		ReleaseWindow(*it);
		const HWND hwnd = it->hwnd;
		HostLog("[window] closing id=" + std::to_string(windowId));
		// Erase the tracking entry before destroying the HWND: the WM_CLOSE that
		// ::DestroyWindow triggers then finds no entry (FindByHwnd -> null) and
		// no-ops, so the teardown above is not repeated.
		windows_.erase(it);
		if (hwnd) {
			::DestroyWindow(hwnd);
		}
		return true;
	}
	return false;
}

std::vector<WindowManager::WindowInfo> WindowManager::List() const
{
	std::vector<WindowInfo> out;
	for (const Window &w : windows_) {
		out.push_back(WindowInfo{w.windowId, w.dockId});
	}
	return out;
}

HWND WindowManager::HwndFor(int windowId) const
{
	const Window *w = FindById(windowId);
	return w ? w->hwnd : nullptr;
}

void WindowManager::CloseAll()
{
	// App is shutting down: suppress the per-window "window.closed" redock broadcast
	// each WM_CLOSE would otherwise emit, so the (also-closing) main window does not
	// try to re-dock panels during teardown.
	closingAll_ = true;
	// Post WM_CLOSE to each detached window so its own WndProc drives the uniform
	// surface-teardown + CloseBrowser + entry-erase path. Still inside the message
	// loop here (main window's WM_CLOSE), so these posts are processed before the
	// loop drains to empty.
	for (Window &w : windows_) {
		if (w.hwnd) {
			PostMessageW(w.hwnd, WM_CLOSE, 0, 0);
		}
	}
}

void WindowManager::DestroyAll()
{
	for (Window &w : windows_) {
		ReleaseWindow(w);
		if (w.hwnd) {
			::DestroyWindow(w.hwnd);
		}
	}
	windows_.clear();
}

LRESULT CALLBACK WindowManager::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	WindowManager *wm = g_wm;

	// WindowChrome consumes WM_DPICHANGED below, so it is logged before the dispatch.
	if (msg == WM_DPICHANGED) {
		const RECT *suggested = reinterpret_cast<const RECT *>(lparam);
		DBG(LogCat::Lifecycle, "[window] host %p dpi -> %u, suggested %ld,%ld %ldx%ld",
		    static_cast<void *>(hwnd), UINT(LOWORD(wparam)), suggested->left, suggested->top,
		    suggested->right - suggested->left, suggested->bottom - suggested->top);
	}

	// Frameless chrome (custom caption, resize borders, drag regions, min size).
	LRESULT chromeResult = 0;
	if (WindowChrome::HandleMessage(hwnd, msg, wparam, lparam, chromeResult)) {
		return chromeResult;
	}

	switch (msg) {
	case WM_SIZE:
		if (wm) {
			if (Window *w = wm->FindByHwnd(hwnd)) {
				if (w->browser) {
					RECT rc;
					GetClientRect(hwnd, &rc);
					DBG(LogCat::Lifecycle, "[window] id=%d WM_SIZE type=%u client %ldx%ld",
					    w->windowId, UINT(wparam), rc.right - rc.left, rc.bottom - rc.top);
					if (HWND bh = w->browser->GetHost()->GetWindowHandle()) {
						SetWindowPos(bh, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
							     SWP_NOZORDER | SWP_NOACTIVATE);
					}
					if (wparam == SIZE_MAXIMIZED || wparam == SIZE_RESTORED) {
						Bridge::EmitEvent(
							EventNames::kWindowStateChanged,
							Bridge::json{{"windowId", w->windowId},
								     {"maximized", wparam == SIZE_MAXIMIZED}});
					}
				}
			}
		}
		return 0;
	case WM_TIMER:
		if (wparam == kPaintWatchdogTimerId) {
			KillTimer(hwnd, kPaintWatchdogTimerId);
			const Window *w = wm && !wm->closingAll_ ? wm->FindByHwnd(hwnd) : nullptr;
			if (w && !w->ready) {
				HostLog("[window] id=" + std::to_string(w->windowId) + " sent no window.ready within " +
					std::to_string(kPaintWatchdogMs) +
					" ms: its page has not painted; its preview surfaces stay held until it does");
			}
			return 0;
		}
		break;
	case WM_SHOWWINDOW:
		DBG(LogCat::Lifecycle, "[window] host %p WM_SHOWWINDOW show=%d status=%ld", static_cast<void *>(hwnd),
		    int(wparam), long(lparam));
		break;
	case WM_NCDESTROY:
		WindowChrome::Discard(hwnd);
		break;
	case WM_CLOSE:
		if (wm) {
			if (Window *w = wm->FindByHwnd(hwnd)) {
				const int windowId = w->windowId;
				const std::string dockId = w->dockId;
				const bool announce = !wm->closingAll_;
				ReleaseWindow(*w);
				HostLog("[window] closing id=" + std::to_string(windowId));
				// Erase the tracking entry; the browser's OnBeforeClose still fires
				// to unregister it from the emit set.
				for (auto it = wm->windows_.begin(); it != wm->windows_.end(); ++it) {
					if (it->hwnd == hwnd) {
						wm->windows_.erase(it);
						break;
					}
				}
				// A user-driven OS close (not app shutdown) must return the dock to
				// the main window, so broadcast the same lifecycle event Redock does.
				if (announce) {
					Bridge::EmitEvent(EventNames::kWindowClosed,
							  Bridge::json{{"windowId", windowId}, {"dock", dockId}});
				}
			}
		}
		::DestroyWindow(hwnd);
		return 0;
	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}
