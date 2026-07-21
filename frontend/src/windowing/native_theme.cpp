#include "native_theme.hpp"

#include <dwmapi.h>

// Defined in the Windows 11 SDK's dwmapi.h; fall back to the documented literals so
// the build works against an older SDK (mirrors window_chrome.cpp's DWMWA_* guards).
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
// Windows 10 1809-1909 shipped the attribute under ordinal 19 before 20H1 settled
// on 20. Try 20 first, then 19.
#define DWMWA_USE_IMMERSIVE_DARK_MODE_PRE_20H1 19
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

namespace NativeTheme {
namespace {

// Mirrors the web chrome token `--color-rail` (frontend/web/src/app.css), the
// TitleBar strip's background that the native caption/border sits against. There is
// no shared native/web color source, so keep this in sync by hand if that token
// changes. COLORREF is 0x00BBGGRR.
constexpr COLORREF kChromeBg = RGB(0x0E, 0x0E, 0x11); // = #0e0e11 (--color-rail)

int g_appliedCount = 0;

} // namespace

void ApplyDark(HWND hwnd)
{
	if (!hwnd) {
		return;
	}

	BOOL dark = TRUE;
	if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark)))) {
		DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_PRE_20H1, &dark, sizeof(dark));
	}

	// Win11 build 22000+ only; a silent no-op on older builds, where immersive dark
	// mode alone still darkens the caption.
	const COLORREF bg = kChromeBg;
	DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &bg, sizeof(bg));
	DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &bg, sizeof(bg));

	g_appliedCount++;
}

int AppliedCount()
{
	return g_appliedCount;
}

} // namespace NativeTheme
