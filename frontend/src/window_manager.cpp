#include "window_manager.hpp"
#include "event_names.hpp"

#include "include/cef_browser.h"

#include "app_icon.hpp"
#include "bridge.hpp"
#include "client.hpp"
#include "log.hpp"
#include "preview_window.hpp"
#include "window_chrome.hpp"

namespace {
constexpr wchar_t kDetachedClassName[] = L"BraidcastDetached";
WindowManager *g_wm = nullptr;

ATOM RegisterDetachedClass(HINSTANCE instance)
{
	static ATOM atom = 0;
	if (atom) {
		return atom;
	}
	WNDCLASSEXW wc = {0};
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = WindowManager::WndProc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	wc.lpszClassName = kDetachedClassName;
	ApplyAppIcon(wc, instance);
	atom = RegisterClassExW(&wc);
	return atom;
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
	const DWORD style = WS_OVERLAPPEDWINDOW;
	AdjustWindowRect(&rc, style, FALSE);
	HWND hwnd = CreateWindowExW(0, kDetachedClassName, L"Braidcast — Detached", style, CW_USEDEFAULT, CW_USEDEFAULT,
				    rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, instance_, nullptr);
	if (!hwnd) {
		HostLog("[window] CreateWindowExW FAILED for windowId=" + std::to_string(windowId));
		return 0;
	}
	// Same frameless command-deck chrome as the main shell, with a smaller size floor.
	WindowChrome::Init(hwnd, WindowChrome::kDetachedWindow);
	ShowWindow(hwnd, SW_SHOW);
	UpdateWindow(hwnd);

	const std::string url = bundleBaseUrl_ + "?window=" + std::to_string(windowId) + "&dock=" + dockId;

	CefBrowserSettings browser_settings;
	CefWindowInfo window_info;
	RECT client_rc;
	GetClientRect(hwnd, &client_rc);
	window_info.SetAsChild(hwnd, CefRect(0, 0, client_rc.right - client_rc.left, client_rc.bottom - client_rc.top));

	CefRefPtr<CefBrowser> browser =
		CefBrowserHost::CreateBrowserSync(window_info, client, url, browser_settings, nullptr, nullptr);
	if (!browser) {
		HostLog("[window] CreateBrowserSync FAILED for windowId=" + std::to_string(windowId));
		::DestroyWindow(hwnd);
		return 0;
	}
	// Clip the browser HWND against its sibling preview overlay so neither
	// overdraws the boundary -- mirrors the main window's wiring.
	if (HWND bh = browser->GetHost()->GetWindowHandle()) {
		SetWindowLongPtrW(bh, GWL_STYLE, GetWindowLongPtrW(bh, GWL_STYLE) | WS_CLIPSIBLINGS);
	}

	// Register this window's host HWND so its preview surfaces parent to THIS
	// top-level window (its overlay HWND becomes a child of hwnd, z-ordered above
	// the child CEF browser), not the main window.
	if (PreviewManager *pm = Preview::Instance()) {
		pm->RegisterWindow(windowId, hwnd);
	}

	windows_.push_back(Window{windowId, dockId, hwnd, browser});
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
		// returns rather than after a posted WM_CLOSE drains. Tear surfaces down
		// FIRST (while libobs is up, before the browser dies) so the obs_display
		// is gone before its canvas mix could be freed; drop the host-HWND
		// registration once the surfaces are gone.
		if (PreviewManager *pm = Preview::Instance()) {
			pm->DestroyWindow(windowId);
			pm->UnregisterWindow(windowId);
		}
		if (it->browser) {
			it->browser->GetHost()->CloseBrowser(true);
		}
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
	for (const Window &w : windows_) {
		if (w.windowId == windowId) {
			return w.hwnd;
		}
	}
	return nullptr;
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
		if (PreviewManager *pm = Preview::Instance()) {
			pm->DestroyWindow(w.windowId);
			pm->UnregisterWindow(w.windowId);
		}
		if (w.browser) {
			w.browser->GetHost()->CloseBrowser(true);
		}
		if (w.hwnd) {
			::DestroyWindow(w.hwnd);
		}
	}
	windows_.clear();
}

LRESULT CALLBACK WindowManager::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	WindowManager *wm = g_wm;

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
	case WM_NCDESTROY:
		WindowChrome::Discard(hwnd);
		break;
	case WM_CLOSE:
		if (wm) {
			if (Window *w = wm->FindByHwnd(hwnd)) {
				const int windowId = w->windowId;
				const std::string dockId = w->dockId;
				const bool announce = !wm->closingAll_;
				// Surfaces must die before the browser/window (UAF discipline);
				// drop the host-HWND registration once they are gone.
				if (PreviewManager *pm = Preview::Instance()) {
					pm->DestroyWindow(windowId);
					pm->UnregisterWindow(windowId);
				}
				if (w->browser) {
					w->browser->GetHost()->CloseBrowser(true);
				}
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
