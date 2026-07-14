// bridge.hpp pulls in the CEF headers, whose CefDOMNode declares methods like
// GetNextSibling that <windows.h> would otherwise macro-clobber. Include it (and
// thus CEF) before any Windows header so CEF parses clean.
#include "app_icon.hpp"
#include "event_names.hpp"
#include "bridge.hpp"

#include "projector_window.hpp"

#include "multistream/CanvasRuntime.hpp"
#include "multistream/CanvasStore.hpp"
#include "obs_bootstrap.hpp"
#include "settings/GeneralSettings.hpp"
#include "scene/transitions.hpp"

#include <obs.h>

#include <windowsx.h>

#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include "log.hpp"

namespace {

constexpr wchar_t kProjectorClassName[] = L"BraidcastProjector";

// Default windowed client size before the user resizes it.
constexpr int kWindowedClientW = 1280;
constexpr int kWindowedClientH = 720;

// kind <-> wire string, the single source of truth for both directions.
struct KindName {
	ProjectorKind kind;
	const char *name;
};
const KindName kKindNames[] = {
	{ProjectorKind::Program, "program"}, {ProjectorKind::Scene, "scene"},         {ProjectorKind::Source, "source"},
	{ProjectorKind::Canvas, "canvas"},   {ProjectorKind::Multiview, "multiview"},
};

// How many scene cells each General `multiviewLayout` value maps to. The legacy
// preview/program layouts (horizontal*/vertical*) have no place in this fork's
// scenes-only multiview, so they fall through to an auto grid capped at 25.
struct MvLayoutCells {
	const char *name;
	int cells;
};
const MvLayoutCells kMvLayoutCells[] = {
	{"scenesOnly4", 4},
	{"scenesOnly9", 9},
	{"scenesOnly16", 16},
	{"scenesOnly24", 25},
};

// Cap on cells for an auto/unknown layout (keeps the grid from shrinking cells to
// invisibility when a collection has dozens of scenes).
constexpr int kMvAutoCellCap = 25;

int MaxCellsForLayout(const std::string &layout)
{
	for (const MvLayoutCells &e : kMvLayoutCells) {
		if (layout == e.name) {
			return e.cells;
		}
	}
	return kMvAutoCellCap;
}

// The Multiview refresh timer id (one per projector window; only Multiview arms
// it). WM_TIMER wparam carries this; with a single timer the handler ignores it.
constexpr UINT_PTR kMultiviewRefreshTimer = 1;
constexpr UINT kMultiviewRefreshMs = 500;

// Cell chrome colors (0xAARRGGBB, as gs_effect_set_color consumes them -- matches
// the legacy Multiview palette where programColor 0xFFD00000 renders red).
constexpr uint32_t kMvCellBorder = 0xFF3C3C3C;      // inactive cell outline (dark gray)
constexpr uint32_t kMvActiveBorder = 0xFFD00000;    // active/program cell outline (red)
constexpr uint32_t kMvLabelBackground = 0xCC000000; // translucent black label plate

ProjectorManager *g_instance = nullptr;

// Convert a UTF-8 string to a wide string for Win32 *W APIs (window titles).
std::wstring Utf8ToWide(const std::string &utf8)
{
	if (utf8.empty()) {
		return std::wstring();
	}
	const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), nullptr, 0);
	if (len <= 0) {
		return std::wstring();
	}
	std::wstring out(size_t(len), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), out.data(), len);
	return out;
}

// Convert a wide string (e.g. MONITORINFOEXW.szDevice) to UTF-8 for the bridge.
std::string WideToUtf8(const wchar_t *wide)
{
	if (!wide || !wide[0]) {
		return std::string();
	}
	const int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
	if (len <= 1) {
		return std::string();
	}
	std::string out(size_t(len - 1), '\0'); // len includes the NUL
	WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
	return out;
}

BOOL CALLBACK MonitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM param)
{
	auto *vec = reinterpret_cast<std::vector<MonitorInfo> *>(param);
	MONITORINFOEXW mi = {};
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoW(monitor, &mi)) {
		// A monitor that vanished mid-enumeration: skip it, never crash.
		return TRUE;
	}
	MonitorInfo info;
	info.index = int(vec->size());
	info.name = WideToUtf8(mi.szDevice);
	info.x = mi.rcMonitor.left;
	info.y = mi.rcMonitor.top;
	info.width = mi.rcMonitor.right - mi.rcMonitor.left;
	info.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
	info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
	vec->push_back(std::move(info));
	return TRUE;
}

LRESULT CALLBACK ProjectorWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

ATOM RegisterProjectorClass(HINSTANCE instance)
{
	static ATOM atom = 0;
	if (atom) {
		return atom;
	}
	WNDCLASSEXW wc = {0};
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = ProjectorWndProc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
	wc.lpszClassName = kProjectorClassName;
	ApplyAppIcon(wc, instance);
	atom = RegisterClassExW(&wc);
	return atom;
}

} // namespace

bool KindFromString(const std::string &name, ProjectorKind &out)
{
	for (const KindName &k : kKindNames) {
		if (name == k.name) {
			out = k.kind;
			return true;
		}
	}
	return false;
}

const char *KindToString(ProjectorKind kind)
{
	for (const KindName &k : kKindNames) {
		if (k.kind == kind) {
			return k.name;
		}
	}
	return "program";
}

std::vector<MonitorInfo> EnumerateMonitors()
{
	std::vector<MonitorInfo> out;
	EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&out));
	return out;
}

// Per-window draw-callback state. Only the kind + the borrowed/pinned target
// pointers, all set on the UI thread before the callback is registered and never
// mutated afterward, so the render thread reads them lock-free. The canvas is
// borrowed (owned by CanvasRuntime); the source is addref'd for the projector's
// whole lifetime -- both outlive the display.
struct ProjectorWindow::State {
	ProjectorKind kind = ProjectorKind::Program;
	obs_canvas_t *canvas = nullptr; // borrowed; null unless kind == Canvas/Multiview(non-Default)
	obs_source_t *source = nullptr; // addref'd; null unless kind == Scene/Source

	// Multiview-only snapshot. WRITTEN on the UI thread (RefreshMultiviewSnapshot),
	// READ on the render thread (RenderMultiview), so every access is guarded by
	// mvMutex. The render thread touches ONLY this snapshot -- never the live scene
	// list -- so a scene deleted between refreshes can never UAF (each entry is
	// addref'd + inc_showing'd for the snapshot's lifetime).
	std::mutex mvMutex;
	std::vector<obs_source_t *> mvScenes; // addref'd + obs_source_inc_showing'd
	std::vector<obs_source_t *> mvLabels; // addref'd private text_gdiplus sources, parallel to mvScenes
	std::vector<std::string> mvNames;     // cached scene names (snapshot-change detection)
	int mvActiveIndex = -1;               // index into mvScenes of the active/program scene, or -1
	int mvRows = 0;
	int mvCols = 0;
	bool mvDrawNames = true; // cached General().multiviewDrawNames at last refresh
};

namespace {

// Create a private text_gdiplus label source for one multiview cell. addref'd
// (creation ref); the snapshot owns it and releases it on rebuild/teardown. Ported
// from frontend_old Multiview::CreateLabel, with a fixed font size (the render path
// scales the label to fit its cell, so the absolute size only sets crispness).
obs_source_t *CreateMultiviewLabel(const char *name)
{
	OBSDataAutoRelease settings = obs_data_create();
	OBSDataAutoRelease font = obs_data_create();

	std::string text = " ";
	text += (name ? name : "");
	text += " ";

	obs_data_set_string(font, "face", "Arial");
	obs_data_set_int(font, "flags", 1); // bold
	obs_data_set_int(font, "size", 42);

	obs_data_set_obj(settings, "font", font);
	obs_data_set_string(settings, "text", text.c_str());
	obs_data_set_bool(settings, "outline", true);
	obs_data_set_int(settings, "opacity", 100);

	// Windows registers "text_gdiplus" (verified in plugins/obs-text/gdiplus/obs-text.cpp).
	return obs_source_create_private("text_gdiplus", name, settings);
}

} // namespace

namespace {

// Resolve the projector's base (canvas) size for its kind. Returns false when no
// size is available (then the callback skips the frame). Program/Canvas read
// their pipeline's video info; Scene/Source use the source size, falling back to
// the global base when the source has no intrinsic size yet.
bool ProjectorBaseSize(ProjectorWindow::State *state, float &baseCX, float &baseCY)
{
	switch (state->kind) {
	case ProjectorKind::Program: {
		obs_video_info ovi;
		if (!obs_get_video_info(&ovi)) {
			return false;
		}
		baseCX = float(ovi.base_width);
		baseCY = float(ovi.base_height);
		return true;
	}
	case ProjectorKind::Canvas: {
		obs_video_info ovi;
		if (!state->canvas || !obs_canvas_get_video_info(state->canvas, &ovi)) {
			return false;
		}
		baseCX = float(ovi.base_width);
		baseCY = float(ovi.base_height);
		return true;
	}
	case ProjectorKind::Multiview: {
		// The whole grid letterboxes into the canvas base size (Default reads the
		// global pipeline; a non-Default canvas reads its own mix).
		obs_video_info ovi;
		const bool ok = state->canvas ? obs_canvas_get_video_info(state->canvas, &ovi)
					      : obs_get_video_info(&ovi);
		if (!ok) {
			return false;
		}
		baseCX = float(ovi.base_width);
		baseCY = float(ovi.base_height);
		return true;
	}
	case ProjectorKind::Scene:
	case ProjectorKind::Source: {
		if (!state->source) {
			return false;
		}
		const uint32_t w = obs_source_get_width(state->source);
		const uint32_t h = obs_source_get_height(state->source);
		if (w > 0 && h > 0) {
			baseCX = float(w);
			baseCY = float(h);
			return true;
		}
		// A source with no intrinsic size yet (e.g. a freshly added capture):
		// letterbox into the global base so it still presents.
		obs_video_info ovi;
		if (!obs_get_video_info(&ovi)) {
			return false;
		}
		baseCX = float(ovi.base_width);
		baseCY = float(ovi.base_height);
		return true;
	}
	}
	return false;
}

// Draw the projector's target inside the current ortho/viewport.
void ProjectorRenderTarget(ProjectorWindow::State *state)
{
	switch (state->kind) {
	case ProjectorKind::Program:
		obs_render_main_texture();
		break;
	case ProjectorKind::Canvas:
		if (state->canvas) {
			obs_canvas_render(state->canvas);
		}
		break;
	case ProjectorKind::Scene:
	case ProjectorKind::Source:
		if (state->source) {
			obs_source_video_render(state->source);
		}
		break;
	case ProjectorKind::Multiview:
		// Handled entirely by RenderMultiview (called before this), never here.
		break;
	}
}

// Fill a solid axis-aligned rect at (x,y,w,h) in the current ortho space. Mirrors
// the legacy Multiview drawBox + a matrix translate.
void DrawSolidRect(float x, float y, float w, float h, uint32_t color)
{
	if (w <= 0.0f || h <= 0.0f) {
		return;
	}
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *colorParam = gs_effect_get_param_by_name(solid, "color");
	gs_effect_set_color(colorParam, color);
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	while (gs_effect_loop(solid, "Solid")) {
		gs_draw_sprite(nullptr, 0, uint32_t(w), uint32_t(h));
	}
	gs_matrix_pop();
}

// Draw a `t`-px-thick rectangle outline (4 solid edges) at (x,y,w,h).
void DrawRectOutline(float x, float y, float w, float h, float t, uint32_t color)
{
	DrawSolidRect(x, y, w, t, color);         // top
	DrawSolidRect(x, y + h - t, w, t, color); // bottom
	DrawSolidRect(x, y, t, h, color);         // left
	DrawSolidRect(x + w - t, y, t, h, color); // right
}

// Render one cell's label (a text source) scaled to fit, bottom-left, over a
// translucent plate. Called in the device-pixel overlay pass (ortho = 0..cx,0..cy).
void DrawMultiviewLabel(obs_source_t *label, float cellX, float cellY, float cellW, float cellH)
{
	const float lw = float(obs_source_get_width(label));
	const float lh = float(obs_source_get_height(label));
	if (lw <= 0.0f || lh <= 0.0f) {
		return;
	}
	const float pad = 4.0f;
	float s = (cellH * 0.10f) / lh;        // target ~10% of cell height
	const float maxW = cellW - 2.0f * pad; // never wider than the cell
	if (maxW > 0.0f && lw * s > maxW) {
		s = maxW / lw;
	}
	if (s <= 0.0f) {
		return;
	}
	const float dw = lw * s;
	const float dh = lh * s;
	const float lx = cellX + pad;
	const float ly = cellY + cellH - dh - pad; // anchored to the cell's bottom

	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	DrawSolidRect(lx - 2.0f, ly - 2.0f, dw + 4.0f, dh + 4.0f, kMvLabelBackground);

	gs_matrix_push();
	gs_matrix_translate3f(lx, ly, 0.0f);
	gs_matrix_scale3f(s, s, 1.0f);
	obs_source_video_render(label);
	gs_matrix_pop();
}

// Render a Multiview projector: a labeled grid of every (snapshotted) scene of the
// target canvas, the active/program scene outlined in the accent color. Reads ONLY
// state's addref'd snapshot under mvMutex -- never the live scene list.
void RenderMultiview(ProjectorWindow::State *state, uint32_t cx, uint32_t cy)
{
	float baseCX = 0.0f;
	float baseCY = 0.0f;
	if (!ProjectorBaseSize(state, baseCX, baseCY)) {
		return;
	}
	if (baseCX <= 0.0f || baseCY <= 0.0f || cx == 0 || cy == 0) {
		return;
	}

	std::lock_guard<std::mutex> lock(state->mvMutex);

	const int rows = state->mvRows;
	const int cols = state->mvCols;
	if (rows <= 0 || cols <= 0) {
		return; // no scenes yet; the window class already paints black
	}

	// Letterbox the whole canvas-aspect grid area into the client rect.
	const float scale = (float(cx) / baseCX < float(cy) / baseCY) ? float(cx) / baseCX : float(cy) / baseCY;
	const int drawCX = int(baseCX * scale);
	const int drawCY = int(baseCY * scale);
	const int drawX = (int(cx) - drawCX) / 2;
	const int drawY = (int(cy) - drawCY) / 2;
	const int cellW = drawCX / cols;
	const int cellH = drawCY / rows;
	if (cellW <= 0 || cellH <= 0) {
		return;
	}

	const size_t count = state->mvScenes.size();

	// Pass 1: render each scene into its own cell viewport. The viewport clips the
	// scene to the cell (no bleed into neighbors) and maps the full canvas into it,
	// exactly like the single-target projector's letterbox -- applied per cell.
	for (size_t i = 0; i < count; ++i) {
		obs_source_t *scene = state->mvScenes[i];
		if (!scene) {
			continue;
		}
		const int col = int(i) % cols;
		const int row = int(i) / cols;
		const int vx = drawX + col * cellW;
		const int vy = drawY + row * cellH;

		gs_viewport_push();
		gs_projection_push();
		gs_ortho(0.0f, baseCX, 0.0f, baseCY, -100.0f, 100.0f);
		gs_set_viewport(vx, vy, cellW, cellH);
		obs_source_video_render(scene);
		gs_projection_pop();
		gs_viewport_pop();
	}

	// Pass 2: chrome (cell borders, active highlight, labels) in device-pixel space.
	gs_viewport_push();
	gs_projection_push();
	gs_ortho(0.0f, float(cx), 0.0f, float(cy), -100.0f, 100.0f);
	gs_set_viewport(0, 0, int(cx), int(cy));

	const size_t cellCount = size_t(rows) * size_t(cols);
	for (size_t i = 0; i < cellCount; ++i) {
		const int col = int(i) % cols;
		const int row = int(i) / cols;
		const float vx = float(drawX + col * cellW);
		const float vy = float(drawY + row * cellH);
		const bool active = (int(i) == state->mvActiveIndex) && i < count;
		DrawRectOutline(vx, vy, float(cellW), float(cellH), active ? 3.0f : 1.0f,
				active ? kMvActiveBorder : kMvCellBorder);
	}

	if (state->mvDrawNames) {
		for (size_t i = 0; i < count; ++i) {
			obs_source_t *label = i < state->mvLabels.size() ? state->mvLabels[i] : nullptr;
			if (!label) {
				continue;
			}
			const int col = int(i) % cols;
			const int row = int(i) / cols;
			DrawMultiviewLabel(label, float(drawX + col * cellW), float(drawY + row * cellH), float(cellW),
					   float(cellH));
		}
	}

	gs_projection_pop();
	gs_viewport_pop();
}

// Draw callback: fired by libobs once per frame on the render thread. cx/cy are
// the display (HWND client) pixel size. Fit the target's base size into it with
// letterboxing so it keeps its aspect ratio. `data` is the ProjectorWindow::State,
// which holds only borrowed/pinned pointers that outlive the display.
void RenderProjector(void *data, uint32_t cx, uint32_t cy)
{
	auto *state = static_cast<ProjectorWindow::State *>(data);

	if (state->kind == ProjectorKind::Multiview) {
		RenderMultiview(state, cx, cy);
		return;
	}

	float baseCX = 0.0f;
	float baseCY = 0.0f;
	if (!ProjectorBaseSize(state, baseCX, baseCY)) {
		return;
	}
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

	ProjectorRenderTarget(state);

	gs_projection_pop();
	gs_viewport_pop();
}

} // namespace

ProjectorWindow::ProjectorWindow(int projectorId, ProjectorKind kind, const std::string &name,
				 const std::string &canvasUuid, ProjectorMode mode, int monitorIndex,
				 obs_source_t *source, obs_canvas_t *canvas)
	: projectorId_(projectorId),
	  kind_(kind),
	  name_(name),
	  canvasUuid_(canvasUuid),
	  mode_(mode),
	  monitorIndex_(monitorIndex),
	  state_(new State()),
	  source_(source),
	  canvas_(canvas)
{
	state_->kind = kind;
	state_->canvas = canvas;
	state_->source = source;
}

ProjectorWindow::~ProjectorWindow()
{
	Destroy();
	delete state_;
}

bool ProjectorWindow::Create(HINSTANCE instance, const RECT &monitorRect)
{
	RegisterProjectorClass(instance);

	// A short title from the target label; the window class paints black so the
	// frameless fullscreen case shows no title anyway.
	std::string label;
	switch (kind_) {
	case ProjectorKind::Program:
		label = "Program";
		break;
	case ProjectorKind::Canvas:
		label = canvasUuid_.empty() ? "Canvas" : ("Canvas " + canvasUuid_);
		break;
	case ProjectorKind::Multiview:
		label = canvasUuid_.empty() ? "Multiview" : ("Multiview " + canvasUuid_);
		break;
	case ProjectorKind::Scene:
	case ProjectorKind::Source:
		label = name_;
		break;
	}
	const std::wstring title = Utf8ToWide("Projector - " + label);

	if (mode_ == ProjectorMode::Fullscreen) {
		// Borderless popup covering exactly the chosen monitor; HWND_TOP over the
		// taskbar. No exclusive fullscreen -- WS_POPUP + the monitor rect suffices.
		hwnd_ = CreateWindowExW(0, kProjectorClassName, title.c_str(), WS_POPUP, monitorRect.left,
					monitorRect.top, monitorRect.right - monitorRect.left,
					monitorRect.bottom - monitorRect.top, nullptr, nullptr, instance, nullptr);
		if (!hwnd_) {
			HostLog("[projector] fullscreen window create FAILED id=" + std::to_string(projectorId_));
			return false;
		}
		SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
		SetWindowPos(hwnd_, HWND_TOP, monitorRect.left, monitorRect.top, monitorRect.right - monitorRect.left,
			     monitorRect.bottom - monitorRect.top, SWP_SHOWWINDOW);
	} else {
		// Resizable overlapped window, default client size, centered on the chosen
		// monitor.
		RECT rc = {0, 0, kWindowedClientW, kWindowedClientH};
		AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
		const int winW = rc.right - rc.left;
		const int winH = rc.bottom - rc.top;
		const int monW = monitorRect.right - monitorRect.left;
		const int monH = monitorRect.bottom - monitorRect.top;
		const int x = monitorRect.left + (monW - winW) / 2;
		const int y = monitorRect.top + (monH - winH) / 2;
		hwnd_ = CreateWindowExW(0, kProjectorClassName, title.c_str(), WS_OVERLAPPEDWINDOW, x, y, winW, winH,
					nullptr, nullptr, instance, nullptr);
		if (!hwnd_) {
			HostLog("[projector] windowed window create FAILED id=" + std::to_string(projectorId_));
			return false;
		}
		SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
		ShowWindow(hwnd_, SW_SHOW);
		UpdateWindow(hwnd_);
	}

	// Honor the global "projectors always on top" preference at create time. For
	// fullscreen this is mostly redundant (already HWND_TOP over the taskbar) but
	// harmless; for windowed it pins the window above normal windows.
	if (ObsBootstrap::General().projectorAlwaysOnTop) {
		SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}

	// The obs_display covers the whole client area (device px; the process is
	// per-monitor-DPI-aware v2 so GetClientRect is in device px).
	RECT client;
	GetClientRect(hwnd_, &client);
	const int cx = client.right - client.left;
	const int cy = client.bottom - client.top;

	gs_init_data init = {};
	init.cx = uint32_t(cx > 0 ? cx : 16);
	init.cy = uint32_t(cy > 0 ? cy : 16);
	init.format = GS_BGRA;
	init.zsformat = GS_ZS_NONE;
	init.window.hwnd = hwnd_; // top-level HWND passthrough (gs_window.hwnd is void*)

	obs_display_t *display = obs_display_create(&init, 0x000000);
	display_ = display;
	HostLog(std::string("[projector] obs_display_create -> ") + (display ? "OK" : "NULL") +
		" id=" + std::to_string(projectorId_));
	if (!display) {
		return false;
	}
	// A Program projector samples obs_render_main_texture every frame; ref the Default
	// canvas's main composite BEFORE registering the draw callback, so the gate is
	// already held the first time the callback runs (no stale-texture at-open window).
	// Balanced in Destroy after the callback is removed. Other kinds render their own
	// target and need no ref.
	if (kind_ == ProjectorKind::Program) {
		ObsBootstrap::CanvasRuntime().AddPreview(std::string());
		mainRenderRefHeld_ = true;
	}

	obs_display_add_draw_callback(display, RenderProjector, state_);

	// Multiview: build the first scene snapshot now (so the first frames have
	// content even before any timer tick), then drive periodic refreshes from a
	// WM_TIMER on the UI thread (active-scene highlight + add/remove/rename pickup).
	if (kind_ == ProjectorKind::Multiview) {
		RefreshMultiviewSnapshot();
		if (SetTimer(hwnd_, kMultiviewRefreshTimer, kMultiviewRefreshMs, nullptr)) {
			mvTimerSet_ = true;
		}
	}
	return true;
}

void ProjectorWindow::ResizeDisplay(int cx, int cy)
{
	if (display_ && cx > 0 && cy > 0) {
		obs_display_resize(static_cast<obs_display_t *>(display_), uint32_t(cx), uint32_t(cy));
	}
}

void ProjectorWindow::Destroy()
{
	// Stop the Multiview refresh timer first so no snapshot rebuild can race the
	// teardown below (and so DestroyWindow has nothing left to reap).
	if (mvTimerSet_ && hwnd_) {
		KillTimer(hwnd_, kMultiviewRefreshTimer);
		mvTimerSet_ = false;
	}

	// Display first: remove the callback + destroy the swapchain while the target
	// mix is still alive (the UAF rule). Then the window, then drop the source
	// addref that pinned a scene/source target alive.
	if (display_) {
		obs_display_t *display = static_cast<obs_display_t *>(display_);
		obs_display_remove_draw_callback(display, RenderProjector, state_);
		obs_display_destroy(display);
		display_ = nullptr;
		HostLog("[projector] display destroyed id=" + std::to_string(projectorId_));
	}

	// Drop the Program main-composite ref only AFTER the draw callback is gone, so the
	// gate can never fire while this projector's callback still samples the texture.
	if (mainRenderRefHeld_) {
		ObsBootstrap::CanvasRuntime().RemovePreview(std::string());
		mainRenderRefHeld_ = false;
	}

	// Multiview snapshot: the render thread (the only other reader) is gone now that
	// the display is destroyed, so release the addref'd scenes (balancing their
	// inc_showing) + label sources. Lock for uniformity; it is uncontended here.
	{
		std::lock_guard<std::mutex> lock(state_->mvMutex);
		for (obs_source_t *scene : state_->mvScenes) {
			if (scene) {
				obs_source_dec_showing(scene);
				obs_source_release(scene);
			}
		}
		for (obs_source_t *label : state_->mvLabels) {
			if (label) {
				obs_source_release(label);
			}
		}
		state_->mvScenes.clear();
		state_->mvLabels.clear();
		state_->mvNames.clear();
		state_->mvActiveIndex = -1;
		state_->mvRows = 0;
		state_->mvCols = 0;
	}
	if (hwnd_) {
		// Clear the back-pointer so a WM_DESTROY this triggers does not re-enter the
		// manager's Close for an already-removed projector.
		SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
		DestroyWindow(hwnd_);
		hwnd_ = nullptr;
	}
	if (source_) {
		obs_source_release(source_);
		source_ = nullptr;
		state_->source = nullptr;
	}
	canvas_ = nullptr;
	state_->canvas = nullptr;
}

void ProjectorWindow::RefreshMultiviewSnapshot()
{
	if (kind_ != ProjectorKind::Multiview) {
		return;
	}

	// 1. Resolve the live scene list (each addref'd to outlive this enum) + the
	//    active/program scene name. Default canvas (empty uuid) -> global scenes +
	//    the channel-0 program scene; a non-Default canvas -> its own scenes +
	//    channel-0 binding. UI thread; the render thread never does this.
	std::vector<obs_source_t *> scenes;
	std::string activeName;
	if (canvasUuid_.empty()) {
		obs_enum_scenes(
			[](void *param, obs_source_t *source) -> bool {
				auto *out = static_cast<std::vector<obs_source_t *> *>(param);
				obs_source_t *ref = obs_source_get_ref(source); // keep past enum
				if (ref) {
					out->push_back(ref);
				}
				return true;
			},
			&scenes);
		OBSSourceAutoRelease program = Transitions::GetProgramScene();
		if (program) {
			const char *n = obs_source_get_name(program);
			if (n) {
				activeName = n;
			}
		}
	} else if (canvas_) {
		obs_canvas_enum_scenes(
			canvas_,
			[](void *param, obs_source_t *source) -> bool {
				auto *out = static_cast<std::vector<obs_source_t *> *>(param);
				obs_source_t *ref = obs_source_get_ref(source);
				if (ref) {
					out->push_back(ref);
				}
				return true;
			},
			&scenes);
		OBSSourceAutoRelease current = obs_canvas_get_channel(canvas_, 0);
		if (current) {
			const char *n = obs_source_get_name(current);
			if (n) {
				activeName = n;
			}
		}
	}

	// 2. Grid dims = the configured layout's cell count, clamped to the live scene
	//    count, packed into the tightest near-square grid.
	const GeneralSettings &general = ObsBootstrap::General();
	const int maxCells = MaxCellsForLayout(general.multiviewLayout);
	int n = int(scenes.size());
	if (n > maxCells) {
		n = maxCells;
	}
	int cols = 0;
	int rows = 0;
	if (n > 0) {
		cols = int(std::ceil(std::sqrt(double(n))));
		rows = (n + cols - 1) / cols;
	}

	// Drop scenes beyond the cell cap (they are not shown, so never inc_showing'd).
	for (size_t i = size_t(n); i < scenes.size(); ++i) {
		obs_source_release(scenes[i]);
	}
	scenes.resize(size_t(n));

	// 3. Names + active index from the kept scenes.
	std::vector<std::string> names;
	names.reserve(scenes.size());
	int activeIndex = -1;
	for (size_t i = 0; i < scenes.size(); ++i) {
		const char *nm = obs_source_get_name(scenes[i]);
		std::string s = nm ? nm : std::string();
		if (!activeName.empty() && s == activeName) {
			activeIndex = int(i);
		}
		names.push_back(std::move(s));
	}

	// 4. Cheap path: scene SET + grid + drawNames unchanged -> only the active index
	//    can have moved. Update it under the lock and drop the freshly-taken refs
	//    (the existing snapshot stays; we never inc_showing'd these, so just release).
	{
		std::lock_guard<std::mutex> lock(state_->mvMutex);
		const bool unchanged = names == state_->mvNames && rows == state_->mvRows && cols == state_->mvCols &&
				       general.multiviewDrawNames == state_->mvDrawNames;
		if (unchanged) {
			state_->mvActiveIndex = activeIndex;
			for (obs_source_t *scene : scenes) {
				obs_source_release(scene);
			}
			return;
		}
	}

	// 5. Full rebuild: activate the kept scenes (so non-program scenes still render)
	//    and build a parallel label per scene -- all on the UI thread, off the lock.
	for (obs_source_t *scene : scenes) {
		obs_source_inc_showing(scene);
	}
	std::vector<obs_source_t *> labels;
	labels.reserve(scenes.size());
	for (const std::string &nm : names) {
		labels.push_back(CreateMultiviewLabel(nm.c_str()));
	}

	// 6. Swap the new snapshot in under the lock (a fast pointer swap); stash the old
	//    vectors to release AFTER unlocking, once the render thread can no longer
	//    reference them.
	std::vector<obs_source_t *> oldScenes;
	std::vector<obs_source_t *> oldLabels;
	{
		std::lock_guard<std::mutex> lock(state_->mvMutex);
		oldScenes.swap(state_->mvScenes);
		oldLabels.swap(state_->mvLabels);
		state_->mvScenes = std::move(scenes);
		state_->mvLabels = std::move(labels);
		state_->mvNames = std::move(names);
		state_->mvActiveIndex = activeIndex;
		state_->mvRows = rows;
		state_->mvCols = cols;
		state_->mvDrawNames = general.multiviewDrawNames;
	}

	// 7. Release the previous snapshot (balance its inc_showing + addref).
	for (obs_source_t *scene : oldScenes) {
		if (scene) {
			obs_source_dec_showing(scene);
			obs_source_release(scene);
		}
	}
	for (obs_source_t *label : oldLabels) {
		if (label) {
			obs_source_release(label);
		}
	}
}

namespace {

// Map a window message to its ProjectorWindow (stashed in GWLP_USERDATA at
// create). null for a foreign HWND or after Destroy cleared it.
ProjectorWindow *ProjectorFromHwnd(HWND hwnd)
{
	return reinterpret_cast<ProjectorWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

LRESULT CALLBACK ProjectorWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg) {
	case WM_SIZE: {
		// Windowed-mode resize: keep the obs_display matched to the client area.
		ProjectorWindow *self = ProjectorFromHwnd(hwnd);
		if (self) {
			RECT rc;
			GetClientRect(hwnd, &rc);
			self->ResizeDisplay(rc.right - rc.left, rc.bottom - rc.top);
		}
		return 0;
	}
	case WM_TIMER: {
		// Multiview refresh tick (UI thread). Only Multiview windows arm a timer;
		// RefreshMultiviewSnapshot no-ops for any other kind.
		ProjectorWindow *self = ProjectorFromHwnd(hwnd);
		if (self) {
			self->RefreshMultiviewSnapshot();
		}
		return 0;
	}
	case WM_KEYDOWN:
		if (wparam == VK_ESCAPE) {
			// Self-close via the manager so teardown is ordered + the event fires.
			ProjectorWindow *self = ProjectorFromHwnd(hwnd);
			if (self && Projector::Instance()) {
				const int id = self->Id();
				Projector::Instance()->Close(id);
				Bridge::EmitEvent(EventNames::kProjectorChanged, Bridge::json{{"closed", id}});
			}
			return 0;
		}
		break;
	case WM_CLOSE: {
		// Close button / Alt+F4: route through the manager (ordered teardown +
		// event). Re-entrancy-safe: Close no-ops if the id is already gone.
		ProjectorWindow *self = ProjectorFromHwnd(hwnd);
		if (self && Projector::Instance()) {
			const int id = self->Id();
			Projector::Instance()->Close(id);
			Bridge::EmitEvent(EventNames::kProjectorChanged, Bridge::json{{"closed", id}});
		} else {
			DestroyWindow(hwnd);
		}
		return 0;
	}
	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

ProjectorManager::ProjectorManager(HINSTANCE instance) : instance_(instance) {}

ProjectorManager::~ProjectorManager()
{
	DestroyAll();
}

int ProjectorManager::Open(ProjectorKind kind, const std::string &name, const std::string &canvasUuid, bool fullscreen,
			   int monitorIndex, std::string &error)
{
	// Resolve the chosen monitor's rect. Fullscreen requires a valid index;
	// windowed falls back to the primary (or the first) monitor.
	const std::vector<MonitorInfo> monitors = EnumerateMonitors();
	RECT monitorRect = {0, 0, kWindowedClientW, kWindowedClientH};
	bool haveRect = false;
	if (monitorIndex >= 0 && monitorIndex < int(monitors.size())) {
		const MonitorInfo &m = monitors[size_t(monitorIndex)];
		monitorRect = {m.x, m.y, m.x + m.width, m.y + m.height};
		haveRect = true;
	} else if (!fullscreen) {
		for (const MonitorInfo &m : monitors) {
			if (m.primary) {
				monitorRect = {m.x, m.y, m.x + m.width, m.y + m.height};
				haveRect = true;
				break;
			}
		}
		if (!haveRect && !monitors.empty()) {
			const MonitorInfo &m = monitors.front();
			monitorRect = {m.x, m.y, m.x + m.width, m.y + m.height};
			haveRect = true;
		}
		monitorIndex = -1; // windowed without a usable index is "primary/none"
	}
	if (fullscreen && !haveRect) {
		error = "fullscreen requires a valid monitor index";
		return 0;
	}

	// Acquire the target. Scene/Source: addref the named source (pinned for the
	// projector's life). Canvas: borrow the live mix from the runtime.
	obs_source_t *source = nullptr;
	obs_canvas_t *canvas = nullptr;
	switch (kind) {
	case ProjectorKind::Scene:
	case ProjectorKind::Source:
		source = obs_get_source_by_name(name.c_str()); // addref'd
		if (!source) {
			error = "no source named '" + name + "'";
			return 0;
		}
		break;
	case ProjectorKind::Canvas:
		canvas = ObsBootstrap::CanvasRuntime().Find(canvasUuid); // borrowed
		if (!canvas) {
			error = "no live canvas mix for '" + canvasUuid + "'";
			return 0;
		}
		break;
	case ProjectorKind::Multiview:
		// Default canvas (empty uuid) renders the global scenes -> no mix to borrow.
		// A non-Default canvas borrows its live mix (must exist).
		if (!canvasUuid.empty()) {
			canvas = ObsBootstrap::CanvasRuntime().Find(canvasUuid); // borrowed
			if (!canvas) {
				error = "no live canvas mix for '" + canvasUuid + "'";
				return 0;
			}
		}
		break;
	case ProjectorKind::Program:
		break;
	}

	const int id = nextId_++;
	const ProjectorMode mode = fullscreen ? ProjectorMode::Fullscreen : ProjectorMode::Windowed;
	auto window = std::make_unique<ProjectorWindow>(id, kind, name, canvasUuid, mode, monitorIndex, source, canvas);
	if (!window->Create(instance_, monitorRect)) {
		error = "failed to create projector window/display";
		window->Destroy(); // releases the source addref if any
		return 0;
	}

	projectors_.push_back(std::move(window));
	HostLog("[projector] opened id=" + std::to_string(id) + " kind=" + KindToString(kind) +
		(mode == ProjectorMode::Fullscreen ? " fullscreen" : " windowed") +
		" monitor=" + std::to_string(monitorIndex));
	return id;
}

bool ProjectorManager::Close(int projectorId)
{
	for (auto it = projectors_.begin(); it != projectors_.end(); ++it) {
		if ((*it)->Id() == projectorId) {
			// Destroy (display before window) then erase. Erasing after Destroy means
			// a WM_DESTROY re-entering via the (already-cleared) GWLP_USERDATA finds no
			// ProjectorWindow and no-ops -- no double-free.
			(*it)->Destroy();
			projectors_.erase(it);
			HostLog("[projector] closed id=" + std::to_string(projectorId));
			return true;
		}
	}
	return false;
}

std::vector<ProjectorManager::ProjectorInfo> ProjectorManager::List() const
{
	std::vector<ProjectorInfo> out;
	for (const auto &p : projectors_) {
		out.push_back(
			ProjectorInfo{p->Id(), p->Kind(), p->Name(), p->CanvasUuid(), p->Mode(), p->MonitorIndex()});
	}
	return out;
}

void ProjectorManager::DestroyAll()
{
	// Tear every display down before the canvas mixes are freed (UAF rule). Destroy
	// is idempotent; the unique_ptr dtors would also call it, but doing it here
	// keeps the ordering explicit relative to ObsBootstrap::Stop().
	for (auto &p : projectors_) {
		p->Destroy();
	}
	projectors_.clear();
}

void ProjectorManager::ApplyAlwaysOnTop(bool onTop)
{
	for (auto &p : projectors_) {
		HWND hwnd = p->Hwnd();
		if (hwnd) {
			SetWindowPos(hwnd, onTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
				     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}
	}
}

bool ProjectorManager::HasDisplayForTest(int projectorId) const
{
	for (const auto &p : projectors_) {
		if (p->Id() == projectorId) {
			return p->HasDisplay();
		}
	}
	return false;
}

namespace Projector {

void SetInstance(ProjectorManager *pm)
{
	g_instance = pm;
}

ProjectorManager *Instance()
{
	return g_instance;
}

} // namespace Projector
