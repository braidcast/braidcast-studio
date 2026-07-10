// bridge.hpp pulls in the CEF headers, whose CefDOMNode declares methods like
// GetNextSibling that <windows.h> would otherwise macro-clobber. Include it (and
// thus CEF) before any Windows header so CEF parses clean.
#include "app_icon.hpp"
#include "event_names.hpp"
#include "bridge.hpp"

#include "interact_window.hpp"
#include "projector_window.hpp" // EnumerateMonitors() for initial centering

#include <obs.h>

#include <windowsx.h>

#include <algorithm>
#include <string>

#include "log.hpp"

namespace {

constexpr wchar_t kInteractClassName[] = L"BraidcastInteract";

// Default client size before the source's aspect is known.
constexpr int kDefaultClientW = 1280;
constexpr int kDefaultClientH = 720;

// Private message posted from the source "rename" signal (any thread) so the
// retitle runs on the UI thread, reading the already-updated source name.
constexpr UINT WM_INTERACT_RENAME = WM_APP + 1;

InteractManager *g_instance = nullptr;

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

LRESULT CALLBACK InteractWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

ATOM RegisterInteractClass(HINSTANCE instance)
{
	static ATOM atom = 0;
	if (atom) {
		return atom;
	}
	WNDCLASSEXW wc = {0};
	wc.cbSize = sizeof(wc);
	// CS_DBLCLKS so WM_*BUTTONDBLCLK is delivered (double-click forwarding).
	wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
	wc.lpfnWndProc = InteractWndProc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
	wc.lpszClassName = kInteractClassName;
	ApplyAppIcon(wc, instance);
	atom = RegisterClassExW(&wc);
	return atom;
}

} // namespace

// Per-window draw-callback state: only the addref-pinned source, set on the UI
// thread before the callback is registered and cleared in Destroy after the
// display is gone. The render thread reads it lock-free; the source outlives the
// display, so the read is safe.
struct InteractWindow::State {
	obs_source_t *source = nullptr; // addref'd; outlives the display
};

namespace {

// Resolve the source's base (canvas) size. Uses the source's intrinsic size when
// available, falling back to the global base when the source has no size yet (a
// freshly added capture). Shared by the draw callback AND the WndProc coordinate
// mapping so both agree on the letterbox transform. Returns false when no size is
// available (then the caller skips).
bool InteractBaseSize(obs_source_t *source, float &baseCX, float &baseCY)
{
	if (!source) {
		return false;
	}
	const uint32_t w = obs_source_get_width(source);
	const uint32_t h = obs_source_get_height(source);
	if (w > 0 && h > 0) {
		baseCX = float(w);
		baseCY = float(h);
		return true;
	}
	obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		return false;
	}
	baseCX = float(ovi.base_width);
	baseCY = float(ovi.base_height);
	return true;
}

// Draw callback: fired by libobs once per frame on the render thread. cx/cy are
// the display (HWND client) pixel size. Letterbox the source's base size into it
// keeping aspect. `data` is the InteractWindow::State (addref-pinned source).
void RenderInteract(void *data, uint32_t cx, uint32_t cy)
{
	auto *state = static_cast<InteractWindow::State *>(data);
	if (!state->source) {
		return;
	}

	float baseCX = 0.0f;
	float baseCY = 0.0f;
	if (!InteractBaseSize(state->source, baseCX, baseCY)) {
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
	const bool previous = gs_set_linear_srgb(true);

	gs_ortho(0.0f, baseCX, 0.0f, baseCY, -100.0f, 100.0f);
	gs_set_viewport(drawX, drawY, drawCX, drawCY);
	obs_source_video_render(state->source);

	gs_set_linear_srgb(previous);
	gs_projection_pop();
	gs_viewport_pop();
}

// --- input translation (UI thread, WndProc) --------------------------------

// obs modifier mask from the current keyboard state. Shared by key + mouse
// events; mouse events additionally OR in the pressed-button bits.
uint32_t KeyModifiers()
{
	uint32_t m = INTERACT_NONE;
	if (GetKeyState(VK_SHIFT) < 0) {
		m |= INTERACT_SHIFT_KEY;
	}
	if (GetKeyState(VK_CONTROL) < 0) {
		m |= INTERACT_CONTROL_KEY;
	}
	if (GetKeyState(VK_MENU) < 0) {
		m |= INTERACT_ALT_KEY;
	}
	return m;
}

// Mouse-event modifiers: keyboard modifiers + the pressed mouse buttons. `mkFlags`
// is the message's MK_* bitset (authoritative for mouse messages: wparam for
// button/move messages, GET_KEYSTATE_WPARAM(wparam) for wheel messages).
uint32_t MouseModifiers(WPARAM mkFlags)
{
	uint32_t m = KeyModifiers();
	if (mkFlags & MK_LBUTTON) {
		m |= INTERACT_MOUSE_LEFT;
	}
	if (mkFlags & MK_MBUTTON) {
		m |= INTERACT_MOUSE_MIDDLE;
	}
	if (mkFlags & MK_RBUTTON) {
		m |= INTERACT_MOUSE_RIGHT;
	}
	return m;
}

// Map a client-pixel point to source-pixel coordinates via the inverse of the
// letterbox transform the draw callback uses (recomputed from the live client
// rect + source size). Fills ev.x/ev.y regardless; returns whether the point is
// inside the source rect (OBS uses the coords-when-outside for the mouse_up
// clamp, but suppresses move/click outside).
bool MapToSource(obs_source_t *source, HWND hwnd, int mx, int my, obs_mouse_event &ev)
{
	float baseCX = 0.0f;
	float baseCY = 0.0f;
	if (!InteractBaseSize(source, baseCX, baseCY)) {
		return false;
	}
	RECT rc;
	GetClientRect(hwnd, &rc);
	const float cx = float(rc.right - rc.left);
	const float cy = float(rc.bottom - rc.top);
	if (cx <= 0.0f || cy <= 0.0f || baseCX <= 0.0f || baseCY <= 0.0f) {
		return false;
	}

	const float scale = (cx / baseCX < cy / baseCY) ? cx / baseCX : cy / baseCY;
	const float offX = (cx - baseCX * scale) / 2.0f;
	const float offY = (cy - baseCY * scale) / 2.0f;
	const float srcX = (float(mx) - offX) / scale;
	const float srcY = (float(my) - offY) / scale;

	ev.x = int(srcX);
	ev.y = int(srcY);

	if (srcX < 0.0f || srcX > baseCX) {
		return false;
	}
	if (srcY < 0.0f || srcY > baseCY) {
		return false;
	}
	return true;
}

// Forward a mouse click. Follows OBS: send the click when the button is released
// or the cursor is inside the source.
void HandleClick(obs_source_t *source, HWND hwnd, WPARAM wparam, LPARAM lparam, int32_t button, bool mouseUp,
		 uint32_t clickCount)
{
	obs_mouse_event ev = {};
	ev.modifiers = MouseModifiers(wparam);
	const bool inside = MapToSource(source, hwnd, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), ev);
	if (mouseUp || inside) {
		obs_source_send_mouse_click(source, &ev, button, mouseUp, clickCount);
	}
}

// Forward a key press/release. `text` (if non-null) is the UTF-8 of a typed char
// for the WM_CHAR path; key down/up use native_vkey/scancode with no text.
void HandleKey(obs_source_t *source, WPARAM wparam, LPARAM lparam, bool keyUp, const char *text)
{
	obs_key_event ev = {};
	ev.modifiers = KeyModifiers();
	ev.native_modifiers = 0;
	ev.native_scancode = uint32_t((lparam >> 16) & 0xff);
	ev.native_vkey = uint32_t(wparam);
	// obs_key_event.text is non-const; point it at a valid buffer for the send
	// call only. Empty (never null) for key down/up, mirroring OBS's text.data().
	char empty[1] = {0};
	ev.text = text ? const_cast<char *>(text) : empty;
	obs_source_send_key_click(source, &ev, keyUp);
}

InteractWindow *InteractFromHwnd(HWND hwnd)
{
	return reinterpret_cast<InteractWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// Source "remove" signal: the source was deleted (or its collection swapped).
// Our hard ref keeps it (and its CEF browser) alive as a zombie until the window
// closes, so close it. Fires from libobs (possibly off the UI thread) while the
// source is being torn down; PostMessage defers the close + our ref-release to
// the UI thread, after the signal returns -- never re-entering the teardown.
void OnSourceRemoved(void *data, calldata_t * /*cd*/)
{
	auto *self = static_cast<InteractWindow *>(data);
	if (self && self->Hwnd()) {
		PostMessageW(self->Hwnd(), WM_CLOSE, 0, 0);
	}
}

// Source "rename" signal: defer the retitle to the UI thread, which reads the
// fresh name off the pinned source (already renamed by then).
void OnSourceRenamed(void *data, calldata_t * /*cd*/)
{
	auto *self = static_cast<InteractWindow *>(data);
	if (self && self->Hwnd()) {
		PostMessageW(self->Hwnd(), WM_INTERACT_RENAME, 0, 0);
	}
}

// Self-close through the manager so teardown is ordered + re-entrancy-safe (the
// GWLP_USERDATA is cleared in Destroy, so a WM_DESTROY this triggers no-ops).
void SelfClose(InteractWindow *self)
{
	if (!self || !Interact::Instance()) {
		return;
	}
	const int id = self->Id();
	Interact::Instance()->Close(id);
	Bridge::EmitEvent(EventNames::kInteractChanged, Bridge::json{{"closed", id}});
}

LRESULT CALLBACK InteractWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	InteractWindow *self = InteractFromHwnd(hwnd);
	obs_source_t *source = self ? self->PinnedSource() : nullptr;

	switch (msg) {
	case WM_MOUSEMOVE: {
		if (source) {
			obs_mouse_event ev = {};
			ev.modifiers = MouseModifiers(wparam);
			const bool inside = MapToSource(source, hwnd, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), ev);
			obs_source_send_mouse_move(source, &ev, !inside);
			// Arm WM_MOUSELEAVE so a real leave sends a final mouse_leave move.
			TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
			TrackMouseEvent(&tme);
		}
		return 0;
	}
	case WM_MOUSELEAVE: {
		if (source) {
			obs_mouse_event ev = {};
			obs_source_send_mouse_move(source, &ev, true);
		}
		return 0;
	}
	case WM_LBUTTONDOWN:
		if (self) {
			self->PressButton(MK_LBUTTON);
			HandleClick(source, hwnd, wparam, lparam, MOUSE_LEFT, false, 1);
		}
		return 0;
	case WM_LBUTTONDBLCLK:
		if (self) {
			self->PressButton(MK_LBUTTON);
			HandleClick(source, hwnd, wparam, lparam, MOUSE_LEFT, false, 2);
		}
		return 0;
	case WM_LBUTTONUP:
		if (self) {
			HandleClick(source, hwnd, wparam, lparam, MOUSE_LEFT, true, 1);
			self->ReleaseButton(MK_LBUTTON);
		}
		return 0;
	case WM_MBUTTONDOWN:
		if (self) {
			self->PressButton(MK_MBUTTON);
			HandleClick(source, hwnd, wparam, lparam, MOUSE_MIDDLE, false, 1);
		}
		return 0;
	case WM_MBUTTONDBLCLK:
		if (self) {
			self->PressButton(MK_MBUTTON);
			HandleClick(source, hwnd, wparam, lparam, MOUSE_MIDDLE, false, 2);
		}
		return 0;
	case WM_MBUTTONUP:
		if (self) {
			HandleClick(source, hwnd, wparam, lparam, MOUSE_MIDDLE, true, 1);
			self->ReleaseButton(MK_MBUTTON);
		}
		return 0;
	case WM_RBUTTONDOWN:
		if (self) {
			self->PressButton(MK_RBUTTON);
			HandleClick(source, hwnd, wparam, lparam, MOUSE_RIGHT, false, 1);
		}
		return 0;
	case WM_RBUTTONDBLCLK:
		if (self) {
			self->PressButton(MK_RBUTTON);
			HandleClick(source, hwnd, wparam, lparam, MOUSE_RIGHT, false, 2);
		}
		return 0;
	case WM_RBUTTONUP:
		if (self) {
			HandleClick(source, hwnd, wparam, lparam, MOUSE_RIGHT, true, 1);
			self->ReleaseButton(MK_RBUTTON);
		}
		return 0;
	case WM_CAPTURECHANGED:
		// Another window stole the grab: forget our pressed buttons (no synthetic
		// up, no ReleaseCapture -- we no longer hold it).
		if (self) {
			self->ResetButtons();
		}
		return 0;
	case WM_MOUSEWHEEL: {
		// Wheel coords in lParam are SCREEN coords; map to client first.
		if (source) {
			obs_mouse_event ev = {};
			ev.modifiers = MouseModifiers(GET_KEYSTATE_WPARAM(wparam));
			POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
			ScreenToClient(hwnd, &pt);
			if (MapToSource(source, hwnd, pt.x, pt.y, ev)) {
				obs_source_send_mouse_wheel(source, &ev, 0, GET_WHEEL_DELTA_WPARAM(wparam));
			}
		}
		return 0;
	}
	case WM_MOUSEHWHEEL: {
		if (source) {
			obs_mouse_event ev = {};
			ev.modifiers = MouseModifiers(GET_KEYSTATE_WPARAM(wparam));
			POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
			ScreenToClient(hwnd, &pt);
			if (MapToSource(source, hwnd, pt.x, pt.y, ev)) {
				obs_source_send_mouse_wheel(source, &ev, GET_WHEEL_DELTA_WPARAM(wparam), 0);
			}
		}
		return 0;
	}
	case WM_SETFOCUS:
		if (source) {
			obs_source_send_focus(source, true);
		}
		return 0;
	case WM_KILLFOCUS:
		if (source) {
			obs_source_send_focus(source, false);
		}
		return 0;
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (msg == WM_KEYDOWN && wparam == VK_ESCAPE) {
			SelfClose(self);
			return 0;
		}
		if (source) {
			// Correlate the translated character (TranslateMessage queues a WM_CHAR
			// before this WM_KEYDOWN is dispatched) and send ONE combined key_click
			// carrying both native_vkey and text. This avoids obs-browser emitting a
			// second JS keydown with keyCode 0 from a standalone WM_CHAR. Only plain
			// keys are peeked; SYSKEY (Alt+...) keeps its WM_SYSCHAR for DefWindowProc
			// so system shortcuts (Alt+F4, menu mnemonics) still work.
			char text[8] = {0};
			if (msg == WM_KEYDOWN) {
				MSG cm;
				if (PeekMessageW(&cm, hwnd, WM_CHAR, WM_CHAR, PM_REMOVE)) {
					const wchar_t wc = wchar_t(cm.wParam);
					const int n = WideCharToMultiByte(CP_UTF8, 0, &wc, 1, text,
									  int(sizeof(text) - 1), nullptr, nullptr);
					text[(n > 0 && n < int(sizeof(text))) ? n : 0] = '\0';
				}
			}
			HandleKey(source, wparam, lparam, false, text[0] ? text : nullptr);
		}
		// Sys keys must still reach DefWindowProc so Alt+F4 / the system menu work.
		if (msg == WM_SYSKEYDOWN) {
			break;
		}
		return 0;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		if (source) {
			HandleKey(source, wparam, lparam, true, nullptr);
		}
		if (msg == WM_SYSKEYUP) {
			break;
		}
		return 0;
	case WM_INTERACT_RENAME: {
		// Retitle off the (already-renamed) pinned source, on the UI thread.
		if (source) {
			const char *name = obs_source_get_name(source);
			const std::wstring title = Utf8ToWide(std::string("Interact - ") + (name ? name : ""));
			SetWindowTextW(hwnd, title.c_str());
		}
		return 0;
	}
	case WM_SIZE: {
		if (self) {
			RECT rc;
			GetClientRect(hwnd, &rc);
			self->ResizeDisplay(rc.right - rc.left, rc.bottom - rc.top);
		}
		return 0;
	}
	case WM_CLOSE:
		if (self && Interact::Instance()) {
			SelfClose(self);
		} else {
			DestroyWindow(hwnd);
		}
		return 0;
	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

InteractWindow::InteractWindow(int interactId, obs_source_t *source)
	: interactId_(interactId), state_(new State()), source_(source)
{
	const char *uuid = source ? obs_source_get_uuid(source) : nullptr;
	if (uuid) {
		sourceUuid_ = uuid;
	}
	state_->source = source;
}

InteractWindow::~InteractWindow()
{
	Destroy();
	delete state_;
}

bool InteractWindow::Create(HINSTANCE instance)
{
	RegisterInteractClass(instance);

	const char *name = obs_source_get_name(source_);
	const std::wstring title = Utf8ToWide(std::string("Interact - ") + (name ? name : ""));

	// Initial client size fits the source's aspect into a sane box (never upscaled
	// beyond native); falls back to a default when the source has no size yet.
	int clientW = kDefaultClientW;
	int clientH = kDefaultClientH;
	const uint32_t sw = obs_source_get_width(source_);
	const uint32_t sh = obs_source_get_height(source_);
	if (sw > 0 && sh > 0) {
		double scale = std::min(double(kDefaultClientW) / double(sw), double(kDefaultClientH) / double(sh));
		if (scale > 1.0) {
			scale = 1.0;
		}
		clientW = std::max(int(double(sw) * scale + 0.5), 320);
		clientH = std::max(int(double(sh) * scale + 0.5), 180);
	}

	RECT rc = {0, 0, clientW, clientH};
	AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
	const int winW = rc.right - rc.left;
	const int winH = rc.bottom - rc.top;

	// Center on the primary monitor; CW_USEDEFAULT if none can be resolved.
	int x = CW_USEDEFAULT;
	int y = CW_USEDEFAULT;
	for (const MonitorInfo &m : EnumerateMonitors()) {
		if (m.primary) {
			x = m.x + (m.width - winW) / 2;
			y = m.y + (m.height - winH) / 2;
			break;
		}
	}

	hwnd_ = CreateWindowExW(0, kInteractClassName, title.c_str(), WS_OVERLAPPEDWINDOW, x, y, winW, winH, nullptr,
				nullptr, instance, nullptr);
	if (!hwnd_) {
		HostLog("[interact] window create FAILED id=" + std::to_string(interactId_));
		return false;
	}
	SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
	ShowWindow(hwnd_, SW_SHOW);
	UpdateWindow(hwnd_);

	// obs_display over the client area (device px; per-monitor-DPI-aware v2 so
	// GetClientRect is already device px).
	RECT client;
	GetClientRect(hwnd_, &client);
	const int cx = client.right - client.left;
	const int cy = client.bottom - client.top;

	gs_init_data init = {};
	init.cx = uint32_t(cx > 0 ? cx : 16);
	init.cy = uint32_t(cy > 0 ? cy : 16);
	init.format = GS_BGRA;
	init.zsformat = GS_ZS_NONE;
	init.window.hwnd = hwnd_;

	obs_display_t *display = obs_display_create(&init, 0x000000);
	display_ = display;
	HostLog(std::string("[interact] obs_display_create -> ") + (display ? "OK" : "NULL") +
		" id=" + std::to_string(interactId_));
	if (!display) {
		return false;
	}
	obs_display_add_draw_callback(display, RenderInteract, state_);

	// Track the source's lifetime: auto-close on delete (else our hard ref leaves
	// a zombie source/browser running) and retitle on rename. Disconnected in
	// Destroy before the ref is dropped.
	if (signal_handler_t *handler = obs_source_get_signal_handler(source_)) {
		signal_handler_connect(handler, "remove", OnSourceRemoved, this);
		signal_handler_connect(handler, "rename", OnSourceRenamed, this);
	}
	return true;
}

void InteractWindow::ResizeDisplay(int cx, int cy)
{
	if (display_ && cx > 0 && cy > 0) {
		obs_display_resize(static_cast<obs_display_t *>(display_), uint32_t(cx), uint32_t(cy));
	}
}

void InteractWindow::Destroy()
{
	// Display first: remove the callback + destroy the swapchain while the source
	// is still alive (the UAF rule). Then the window, then drop the source addref
	// LAST.
	if (display_) {
		obs_display_t *display = static_cast<obs_display_t *>(display_);
		obs_display_remove_draw_callback(display, RenderInteract, state_);
		obs_display_destroy(display);
		display_ = nullptr;
		HostLog("[interact] display destroyed id=" + std::to_string(interactId_));
	}
	if (hwnd_) {
		// Clear the back-pointer so a WM_DESTROY this triggers does not re-enter the
		// manager's Close for an already-removed window.
		SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
		DestroyWindow(hwnd_);
		hwnd_ = nullptr;
	}
	if (source_) {
		// Disconnect the lifetime signals BEFORE dropping our ref so a remove/rename
		// racing teardown can't fire into a half-destroyed window. Disconnect is a
		// safe no-op if Create failed before connecting.
		if (signal_handler_t *sh = obs_source_get_signal_handler(source_)) {
			signal_handler_disconnect(sh, "remove", OnSourceRemoved, this);
			signal_handler_disconnect(sh, "rename", OnSourceRenamed, this);
		}
		obs_source_release(source_);
		source_ = nullptr;
		state_->source = nullptr;
	}
}

obs_source_t *InteractWindow::PinnedSource() const
{
	return source_;
}

void InteractWindow::PressButton(unsigned buttonMask)
{
	// Grab the mouse on the first button down so a drag that leaves the client
	// area keeps delivering moves + the matching button-up.
	if (buttonsDown_ == 0 && hwnd_) {
		SetCapture(hwnd_);
	}
	buttonsDown_ |= buttonMask;
}

void InteractWindow::ReleaseButton(unsigned buttonMask)
{
	buttonsDown_ &= ~buttonMask;
	if (buttonsDown_ == 0) {
		ReleaseCapture();
	}
}

void InteractWindow::ResetButtons()
{
	// Capture was taken by another window: forget our pressed buttons without
	// calling ReleaseCapture (we no longer hold it) and without fabricating an up.
	buttonsDown_ = 0;
}

InteractManager::InteractManager(HINSTANCE instance) : instance_(instance) {}

InteractManager::~InteractManager()
{
	DestroyAll();
}

int InteractManager::Open(obs_source_t *source, std::string &error)
{
	if (!source) {
		error = "no source to interact with";
		return 0;
	}

	// Dedup per source uuid: one window per source. Bring the existing one forward.
	const char *uuid = obs_source_get_uuid(source);
	if (uuid && uuid[0]) {
		for (const auto &w : windows_) {
			if (w->SourceUuid() == uuid) {
				SetForegroundWindow(w->Hwnd());
				return w->Id();
			}
		}
	}

	// Take our OWN ref for the window's whole life (caller still owns + releases
	// its ref).
	obs_source_t *pinned = obs_source_get_ref(source);
	if (!pinned) {
		error = "failed to acquire source reference";
		return 0;
	}

	const int id = nextId_++;
	auto window = std::make_unique<InteractWindow>(id, pinned);
	if (!window->Create(instance_)) {
		error = "failed to create interaction window/display";
		window->Destroy(); // releases the pinned ref
		return 0;
	}

	windows_.push_back(std::move(window));
	HostLog("[interact] opened id=" + std::to_string(id));
	return id;
}

bool InteractManager::Close(int interactId)
{
	for (auto it = windows_.begin(); it != windows_.end(); ++it) {
		if ((*it)->Id() == interactId) {
			// Destroy (display before window) then erase. Erasing after Destroy means
			// a WM_DESTROY re-entering via the cleared GWLP_USERDATA finds no window and
			// no-ops -- no double-free.
			(*it)->Destroy();
			windows_.erase(it);
			HostLog("[interact] closed id=" + std::to_string(interactId));
			return true;
		}
	}
	return false;
}

void InteractManager::DestroyAll()
{
	for (auto &w : windows_) {
		w->Destroy();
	}
	windows_.clear();
}

bool InteractManager::HasDisplayForTest(int interactId) const
{
	for (const auto &w : windows_) {
		if (w->Id() == interactId) {
			return w->HasDisplay();
		}
	}
	return false;
}

namespace Interact {

void SetInstance(InteractManager *im)
{
	g_instance = im;
}

InteractManager *Instance()
{
	return g_instance;
}

} // namespace Interact
