#pragma once

#include <windows.h>

namespace WindowDpi {

// Handle WM_DPICHANGED for an owned top-level window: resize to the OS-suggested
// rect (lparam -> RECT*). The resize fires WM_SIZE, so the window's existing layout
// path re-runs automatically -- callers need no extra relayout here. Call from the
// window's wndproc and return 0 for the message.
void HandleDpiChanged(HWND hwnd, WPARAM wparam, LPARAM lparam);

} // namespace WindowDpi
