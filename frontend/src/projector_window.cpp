// bridge.hpp pulls in the CEF headers, whose CefDOMNode declares methods like
// GetNextSibling that <windows.h> would otherwise macro-clobber. Include it (and
// thus CEF) before any Windows header so CEF parses clean.
#include "bridge.hpp"

#include "projector_window.hpp"

#include "multistream/CanvasRuntime.hpp"
#include "multistream/CanvasStore.hpp"
#include "obs_bootstrap.hpp"

#include <obs.h>

#include <windowsx.h>

#include <string>

#include "log.hpp"

namespace {

constexpr wchar_t kProjectorClassName[] = L"ObsMultiStreamProjector";

// Default windowed client size before the user resizes it.
constexpr int kWindowedClientW = 1280;
constexpr int kWindowedClientH = 720;

// kind <-> wire string, the single source of truth for both directions.
struct KindName {
	ProjectorKind kind;
	const char *name;
};
const KindName kKindNames[] = {
	{ProjectorKind::Program, "program"},
	{ProjectorKind::Scene, "scene"},
	{ProjectorKind::Source, "source"},
	{ProjectorKind::Canvas, "canvas"},
};

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
	obs_canvas_t *canvas = nullptr; // borrowed; null unless kind == Canvas
	obs_source_t *source = nullptr; // addref'd; null unless kind == Scene/Source
};

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
	}
}

// Draw callback: fired by libobs once per frame on the render thread. cx/cy are
// the display (HWND client) pixel size. Fit the target's base size into it with
// letterboxing so it keeps its aspect ratio. `data` is the ProjectorWindow::State,
// which holds only borrowed/pinned pointers that outlive the display.
void RenderProjector(void *data, uint32_t cx, uint32_t cy)
{
	auto *state = static_cast<ProjectorWindow::State *>(data);

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
	obs_display_add_draw_callback(display, RenderProjector, state_);
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
	case WM_KEYDOWN:
		if (wparam == VK_ESCAPE) {
			// Self-close via the manager so teardown is ordered + the event fires.
			ProjectorWindow *self = ProjectorFromHwnd(hwnd);
			if (self && Projector::Instance()) {
				const int id = self->Id();
				Projector::Instance()->Close(id);
				Bridge::EmitEvent("projector.changed", Bridge::json{{"closed", id}});
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
			Bridge::EmitEvent("projector.changed", Bridge::json{{"closed", id}});
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

int ProjectorManager::Open(ProjectorKind kind, const std::string &name, const std::string &canvasUuid,
			   bool fullscreen, int monitorIndex, std::string &error)
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
		out.push_back(ProjectorInfo{p->Id(), p->Kind(), p->Name(), p->CanvasUuid(), p->Mode(),
					    p->MonitorIndex()});
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
