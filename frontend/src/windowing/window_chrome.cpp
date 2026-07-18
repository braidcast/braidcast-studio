#include "window_chrome.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <unordered_map>

#include "include/cef_browser.h"

// Win11 corner-rounding opt-out. These are defined in the Windows 11 SDK's dwmapi.h;
// fall back to the documented literals so the build works against an older SDK.
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_DONOTROUND
#define DWMWCP_DONOTROUND 1
#endif

namespace WindowChrome {
namespace {

// Per-host state, keyed by the top-level host HWND. UI thread only.
std::unordered_map<HWND, Config> g_configs;
std::unordered_map<HWND, HRGN> g_regions; // combined draggable region in client px

// One subclass id for every browser child window we hook.
constexpr UINT_PTR kChildSubclassId = 0x0B5D; // "OBSD"

int FrameSize(HWND hwnd)
{
	const UINT dpi = GetDpiForWindow(hwnd);
	return GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

// A 1px top frame extension keeps the DWM drop shadow (and smooth snap/maximize
// animations) on a window whose caption we have eaten. When maximized there is no
// shadow, and that band composites see-through at screen-top (a wallpaper sliver),
// so drop the top band while maximized.
void ApplyShadowFrame(HWND hwnd, bool maximized)
{
	const MARGINS shadow = {0, 0, maximized ? 0 : 1, 0};
	DwmExtendFrameIntoClientArea(hwnd, &shadow);
}

// True if an auto-hidden taskbar sits on `edge` of the given monitor. A maximized
// borderless window that exactly covers the monitor must leave a 1px sliver on that
// edge or the taskbar can never be summoned by a mouse-to-edge gesture.
bool HasAutohideAppbar(UINT edge, RECT monitor)
{
	APPBARDATA data = {};
	data.cbSize = sizeof(data);
	data.uEdge = edge;
	data.rc = monitor;
	return SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &data) != 0;
}

void HandleNcCalcSize(HWND hwnd, LPARAM lparam, LRESULT &result)
{
	RECT *rect = reinterpret_cast<RECT *>(lparam);
	const RECT nonclient = *rect;
	DefWindowProcW(hwnd, WM_NCCALCSIZE, TRUE, lparam);
	const RECT client = *rect;

	if (IsZoomed(hwnd)) {
		// Maximized: keep DefWindowProc's L/R/B frame inset (so content isn't
		// clipped off-screen), but reclaim the caption band at the top. The top
		// inset uses FrameSize so it matches the FrameSize-based L/R/B insets.
		rect->left = client.left;
		rect->top = nonclient.top + FrameSize(hwnd);
		rect->right = client.right;
		rect->bottom = client.bottom;

		HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
		MONITORINFO mi = {};
		mi.cbSize = sizeof(mi);
		if (GetMonitorInfoW(mon, &mi) && EqualRect(rect, &mi.rcMonitor)) {
			if (HasAutohideAppbar(ABE_BOTTOM, mi.rcMonitor)) {
				rect->bottom--;
			} else if (HasAutohideAppbar(ABE_LEFT, mi.rcMonitor)) {
				rect->left++;
			} else if (HasAutohideAppbar(ABE_TOP, mi.rcMonitor)) {
				rect->top++;
			} else if (HasAutohideAppbar(ABE_RIGHT, mi.rcMonitor)) {
				rect->right--;
			}
		}
	} else {
		// Restored: client == whole window rect (edge-to-edge, no caption, no
		// visible frame). WS_THICKFRAME still yields resize/snap/shadow; the
		// invisible border band is grabbed via WM_NCHITTEST below.
		*rect = nonclient;
	}
	result = 0;
}

LRESULT HandleNcHitTest(HWND hwnd, LPARAM lparam)
{
	POINT mouse = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
	ScreenToClient(hwnd, &mouse);

	RECT rc;
	GetClientRect(hwnd, &rc);
	const int width = rc.right - rc.left;
	const int height = rc.bottom - rc.top;

	// Resize borders only when restored (a maximized window is not resizable).
	if (!IsZoomed(hwnd)) {
		const int frame = FrameSize(hwnd);
		const int diagonal = frame * 2 + GetSystemMetrics(SM_CXBORDER);
		if (mouse.y < frame) {
			if (mouse.x < diagonal) {
				return HTTOPLEFT;
			}
			if (mouse.x >= width - diagonal) {
				return HTTOPRIGHT;
			}
			return HTTOP;
		}
		if (mouse.y >= height - frame) {
			if (mouse.x < diagonal) {
				return HTBOTTOMLEFT;
			}
			if (mouse.x >= width - diagonal) {
				return HTBOTTOMRIGHT;
			}
			return HTBOTTOM;
		}
		if (mouse.x < frame) {
			return HTLEFT;
		}
		if (mouse.x >= width - frame) {
			return HTRIGHT;
		}
	}

	// Inside a DOM drag region -> caption (drag to move; drag a maximized window to
	// restore-and-move, which the OS handles from HTCAPTION).
	auto it = g_regions.find(hwnd);
	if (it != g_regions.end() && it->second && PtInRegion(it->second, mouse.x, mouse.y)) {
		return HTCAPTION;
	}
	return HTCLIENT;
}

// Subclass over every browser child HWND: a WM_NCHITTEST that the child would treat
// as its own client area but which falls inside the host's drag region returns
// HTTRANSPARENT, so the hit-test walks up to the host (whose HandleNcHitTest returns
// HTCAPTION). Without this the windowed CEF child eats the drag.
LRESULT CALLBACK ChildSubclassProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR)
{
	if (msg == WM_NCDESTROY) {
		RemoveWindowSubclass(hwnd, ChildSubclassProc, id);
		return DefSubclassProc(hwnd, msg, wparam, lparam);
	}
	if (msg == WM_NCHITTEST) {
		const LRESULT hit = DefSubclassProc(hwnd, msg, wparam, lparam);
		if (hit == HTCLIENT) {
			HWND root = GetAncestor(hwnd, GA_ROOT);
			auto it = g_regions.find(root);
			if (it != g_regions.end() && it->second) {
				POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
				ScreenToClient(hwnd, &pt); // child sits at host client origin
				if (PtInRegion(it->second, pt.x, pt.y)) {
					return HTTRANSPARENT;
				}
			}
		}
		return hit;
	}
	return DefSubclassProc(hwnd, msg, wparam, lparam);
}

BOOL CALLBACK SubclassChildProc(HWND child, LPARAM)
{
	SetWindowSubclass(child, ChildSubclassProc, kChildSubclassId, 0);
	return TRUE;
}

} // namespace

void Init(HWND hwnd, const Config &cfg)
{
	g_configs[hwnd] = cfg;

	ApplyShadowFrame(hwnd, IsZoomed(hwnd));

	// Square corners: Windows 11 rounds top-level window corners by default, which
	// clashes with the zero-radius command-deck chrome. Opt out (no-op pre-Win11).
	const DWORD corner = DWMWCP_DONOTROUND;
	DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

	// Recompute the non-client area now that WM_NCCALCSIZE is intercepted.
	SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
		     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT &result)
{
	switch (msg) {
	case WM_NCCALCSIZE:
		// Only the wParam==TRUE shape (lparam = NCCALCSIZE_PARAMS, rgrc[0] first)
		// carries the client-area rect we extend. The wParam==FALSE shape is a bare
		// single RECT; let DefWindowProc handle it (forcing TRUE would read rgrc[1/2]
		// past the buffer).
		if (wparam != TRUE) {
			return false;
		}
		HandleNcCalcSize(hwnd, lparam, result);
		return true;
	case WM_SIZE:
		// Re-apply the shadow frame on maximize/restore so the top band vanishes
		// while maximized. Return false so the host's own WM_SIZE handler still runs
		// (LayoutBrowser + the window-state-changed event).
		ApplyShadowFrame(hwnd, wparam == SIZE_MAXIMIZED);
		return false;
	case WM_NCHITTEST:
		result = HandleNcHitTest(hwnd, lparam);
		return true;
	case WM_GETMINMAXINFO: {
		auto it = g_configs.find(hwnd);
		if (it == g_configs.end()) {
			return false;
		}
		const UINT dpi = GetDpiForWindow(hwnd);
		MINMAXINFO *mmi = reinterpret_cast<MINMAXINFO *>(lparam);
		mmi->ptMinTrackSize.x = MulDiv(it->second.minWidth, int(dpi), 96);
		mmi->ptMinTrackSize.y = MulDiv(it->second.minHeight, int(dpi), 96);
		// Maximize clamps to the work area natively (WS_CAPTION is still set), so
		// ptMaxSize/ptMaxPosition are deliberately left to the default -- overriding
		// them is the usual source of multi-monitor maximize bugs.
		result = 0;
		return true;
	}
	default:
		return false;
	}
}

void Discard(HWND hwnd)
{
	g_configs.erase(hwnd);
	auto it = g_regions.find(hwnd);
	if (it != g_regions.end()) {
		if (it->second) {
			DeleteObject(it->second);
		}
		g_regions.erase(it);
	}
}

void SetDraggableRegions(CefRefPtr<CefBrowser> browser, const std::vector<CefDraggableRegion> &regions)
{
	if (!browser || !browser->GetHost()) {
		return;
	}
	HWND browserHwnd = browser->GetHost()->GetWindowHandle();
	if (!browserHwnd) {
		return;
	}
	HWND host = GetAncestor(browserHwnd, GA_ROOT);
	if (!host) {
		return;
	}

	// Regions arrive in DIP; scale to the host's physical client pixels (the space
	// WM_NCHITTEST's ScreenToClient result lives in).
	const double scale = GetDpiForWindow(host) / 96.0;

	HRGN combined = CreateRectRgn(0, 0, 0, 0);
	for (const CefDraggableRegion &r : regions) {
		HRGN piece = CreateRectRgn(int(r.bounds.x * scale), int(r.bounds.y * scale),
					   int((r.bounds.x + r.bounds.width) * scale),
					   int((r.bounds.y + r.bounds.height) * scale));
		CombineRgn(combined, combined, piece, r.draggable ? RGN_OR : RGN_DIFF);
		DeleteObject(piece);
	}

	HRGN &slot = g_regions[host];
	if (slot) {
		DeleteObject(slot);
	}
	slot = combined;

	// (Re)subclass the browser child tree so drag-region hits fall through to the
	// host. EnumChildWindows recurses through the Chromium widget descendants, and
	// SetWindowSubclass is idempotent, so this safely covers windows created since
	// the last change.
	EnumChildWindows(host, SubclassChildProc, 0);
}

} // namespace WindowChrome
