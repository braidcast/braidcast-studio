#ifndef OBS_MULTISTREAM_FRONTEND_WINDOW_CHROME_HPP_
#define OBS_MULTISTREAM_FRONTEND_WINDOW_CHROME_HPP_

#include <windows.h>

#include <vector>

#include "include/cef_browser.h"
#include "include/cef_drag_handler.h"

// Frameless "command-deck" chrome for the top-level host windows (the main shell
// and every torn-out detached window). The stock Windows caption is removed while
// the window stays a normal WS_OVERLAPPEDWINDOW so Aero Snap, maximize/minimize,
// the taskbar clamp, and the DWM drop shadow all keep working natively. The web
// layer paints its own title bar (TitleBar.svelte); its `-webkit-app-region: drag`
// background is fed here via CEF draggable regions so the DOM strip is the grab
// handle.
//
// Technique (rossy/borderless-window + cefclient draggable-region subclassing):
//   * WM_NCCALCSIZE extends the client area over the former caption (edge-to-edge
//     when restored; frame-inset + autohide-taskbar nudge when maximized).
//   * WM_NCHITTEST returns the 8 resize directions for the invisible border band,
//     HTCAPTION inside a draggable region, HTCLIENT elsewhere.
//   * WM_GETMINMAXINFO clamps ptMinTrackSize to a per-window floor.
//   * The browser child HWNDs are subclassed so a WM_NCHITTEST over a drag region
//     returns HTTRANSPARENT, letting the host's HTCAPTION win (the child otherwise
//     eats the click as its own client area).
//
// Single-threaded: every entry point runs on the CEF browser-process UI thread.
namespace WindowChrome {

struct Config {
	int minWidth;  // logical (96-dpi) px; scaled per-window at WM_GETMINMAXINFO
	int minHeight;
};

// Minimum-size floors (logical px). Main is tuned so the nav rail + top/bottom bars
// + one usable dock fit with no clipping; detached is a single-dock floor.
constexpr Config kMainWindow = {960, 600};
constexpr Config kDetachedWindow = {320, 240};

// Register `hwnd` as a frameless host, enable its DWM shadow, and force a frame
// recompute. Call right after CreateWindowExW, before ShowWindow.
void Init(HWND hwnd, const Config &cfg);

// Handle the frameless-chrome messages. Call FIRST in the window's WndProc; if it
// returns true the message was fully handled and `result` holds the return value.
bool HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT &result);

// Forget `hwnd` (free its drag region + config). Call on WM_NCDESTROY.
void Discard(HWND hwnd);

// Store the draggable regions reported by CefDragHandler::OnDraggableRegionsChanged
// for `browser`'s host window and (re)subclass its child HWNDs. Regions arrive in
// DIP; they are scaled to the host's physical client pixels here.
void SetDraggableRegions(CefRefPtr<CefBrowser> browser, const std::vector<CefDraggableRegion> &regions);

} // namespace WindowChrome

#endif // OBS_MULTISTREAM_FRONTEND_WINDOW_CHROME_HPP_
