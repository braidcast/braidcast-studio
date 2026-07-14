#pragma once

#include <windows.h>

// The embedded application icon's resource id. Must match the numeric id used in
// cmake/windows/obs.rc.in. Loaded onto each top-level window class so the title-bar
// caption shows the Braidcast mark (the taskbar already resolves the exe's
// icon resource; the caption uses the window class hIcon/hIconSm, which are null
// unless set here).
inline constexpr WORD kAppIconResId = 101;

// Assigns the big (Alt+Tab/taskbar) and small (caption) app icons to a window class
// before it is registered. No-op-safe: if the resource is missing, LoadImageW
// returns null and the class falls back to the default icon.
inline void ApplyAppIcon(WNDCLASSEXW &wc, HINSTANCE instance)
{
	wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kAppIconResId), IMAGE_ICON,
						 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
						 LR_DEFAULTCOLOR));
	wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kAppIconResId), IMAGE_ICON,
						   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
						   LR_DEFAULTCOLOR));
}
