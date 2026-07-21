#pragma once

#include <windows.h>

namespace NativeTheme {

// Force the window's residual native chrome (DWM caption/border/system menu) dark.
// Call once, right after CreateWindowExW, on every owned top-level window that has
// a native caption or a visible border. Best-effort per attribute: OS builds that
// predate an attribute silently no-op. Unconditional -- Braidcast is always-dark,
// so there is no OS-theme listener and no light path.
void ApplyDark(HWND hwnd);

// Count of successful ApplyDark calls this process. Read by the headless self-test
// to confirm the host chrome was darkened at startup (the only owned window up at
// smoke time). UI thread only.
int AppliedCount();

} // namespace NativeTheme
