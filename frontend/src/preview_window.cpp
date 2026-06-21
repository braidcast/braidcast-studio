#include "preview_window.hpp"

#include <obs.h>

#include <string>

#include "log.hpp"

namespace {

constexpr wchar_t kPreviewClassName[] = L"ObsMultiStreamPreview";

obs_display_t *g_display = nullptr;
PreviewWindow *g_instance = nullptr;

// Draw callback: fired by libobs once per frame on the render thread. cx/cy are
// the display (HWND) pixel size. Fit the base canvas into it with letterboxing
// so the composited scene keeps its aspect ratio instead of stretching. Mirrors
// the legacy preview's GetScaleAndCenterPos math (fit-and-center).
void RenderPreview(void *, uint32_t cx, uint32_t cy)
{
	obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		return;
	}

	const float baseCX = float(ovi.base_width);
	const float baseCY = float(ovi.base_height);
	if (baseCX <= 0.0f || baseCY <= 0.0f || cx == 0 || cy == 0) {
		return;
	}

	const float scale = (float(cx) / baseCX < float(cy) / baseCY) ? float(cx) / baseCX : float(cy) / baseCY;
	const int drawCX = int(baseCX * scale);
	const int drawCY = int(baseCY * scale);
	const int drawX = (int(cx) - drawCX) / 2;
	const int drawY = (int(cy) - drawCY) / 2;

	gs_viewport_push();
	gs_projection_push();

	gs_ortho(0.0f, baseCX, 0.0f, baseCY, -100.0f, 100.0f);
	gs_set_viewport(drawX, drawY, drawCX, drawCY);

	obs_render_main_texture();

	gs_projection_pop();
	gs_viewport_pop();
}

// The overlay HWND uses a no-background class: the obs_display swapchain paints
// every pixel (the canvas plus its black letterbox bars), so a WM_ERASEBKGND
// fill would only flicker against it.
ATOM RegisterPreviewClass(HINSTANCE instance)
{
	static ATOM atom = 0;
	if (atom) {
		return atom;
	}
	WNDCLASSEXW wc = {0};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = DefWindowProcW;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
	wc.lpszClassName = kPreviewClassName;
	atom = RegisterClassExW(&wc);
	return atom;
}

} // namespace

PreviewWindow::PreviewWindow(HWND host, HINSTANCE instance) : host_(host), instance_(instance) {}

void PreviewWindow::EnsureCreated()
{
	if (hwnd_) {
		return;
	}

	RegisterPreviewClass(instance_);

	// Borderless child sibling of the CEF browser HWND. WS_CLIPSIBLINGS keeps the
	// browser from overdrawing into it (the browser HWND also sets it). Starts
	// hidden; SetRect shows it after positioning so no frame flashes at 0,0.
	hwnd_ = CreateWindowExW(0, kPreviewClassName, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 16, 16, host_, nullptr,
				instance_, nullptr);
	if (!hwnd_) {
		HostLog("[obs] preview overlay HWND create FAILED");
		return;
	}
	HostLog("[obs] preview overlay HWND created");

	gs_init_data init = {};
	init.cx = 16;
	init.cy = 16;
	init.format = GS_BGRA;
	init.zsformat = GS_ZS_NONE;
	init.window.hwnd = hwnd_; // child HWND passthrough (gs_window.hwnd is void*)

	g_display = obs_display_create(&init, 0x000000);
	HostLog(std::string("[obs] obs_display_create -> ") + (g_display ? "OK" : "NULL"));
	if (g_display) {
		obs_display_add_draw_callback(g_display, RenderPreview, nullptr);
		HostLog("[obs] preview draw callback registered");
	}
}

void PreviewWindow::SetRect(int x, int y, int cx, int cy)
{
	if (cx <= 0 || cy <= 0) {
		Hide();
		return;
	}

	EnsureCreated();
	if (!hwnd_) {
		return;
	}

	// Position in host-client device pixels and keep above the CEF browser HWND
	// (HWND_TOP raises within the sibling z-order). SWP_SHOWWINDOW reveals it on
	// the first sized call.
	SetWindowPos(hwnd_, HWND_TOP, x, y, cx, cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);

	if (g_display) {
		obs_display_resize(g_display, uint32_t(cx), uint32_t(cy));
	}
}

void PreviewWindow::Hide()
{
	if (hwnd_) {
		ShowWindow(hwnd_, SW_HIDE);
	}
}

void PreviewWindow::Destroy()
{
	if (g_display) {
		obs_display_remove_draw_callback(g_display, RenderPreview, nullptr);
		obs_display_destroy(g_display);
		g_display = nullptr;
		HostLog("[obs] preview display destroyed");
	}
	if (hwnd_) {
		DestroyWindow(hwnd_);
		hwnd_ = nullptr;
	}
}

namespace Preview {
void SetInstance(PreviewWindow *pw)
{
	g_instance = pw;
}
PreviewWindow *Instance()
{
	return g_instance;
}
} // namespace Preview
