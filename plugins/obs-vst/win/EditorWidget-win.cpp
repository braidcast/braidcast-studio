/*****************************************************************************
Copyright (C) 2016-2017 by Colin Edwards.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*****************************************************************************/

#include "../headers/EditorWidget.h"
#include "../headers/VSTPlugin.h"

static const wchar_t *kEditorWindowClass = L"BraidcastVSTEditorWindow";
static constexpr UINT_PTR kIdleTimerId = 1;
static constexpr UINT kIdleTimerMs = 30;
// Marshals a plugin-requested resize onto the UI thread.
static constexpr UINT WM_VST_RESIZE = WM_APP + 1;

EditorWidget::EditorWidget(void *parent, VSTPlugin *plugin) : plugin(plugin)
{
	UNUSED_PARAMETER(parent);
}

EditorWidget::~EditorWidget()
{
	close();
}

void EditorWidget::buildEffectContainer(AEffect *e)
{
	effect = e;
}

void EditorWidget::setWindowTitle(const std::string &t)
{
	int len = MultiByteToWideChar(CP_UTF8, 0, t.c_str(), -1, nullptr, 0);
	std::wstring wide(len > 1 ? len - 1 : 0, L'\0');
	if (len > 1) {
		MultiByteToWideChar(CP_UTF8, 0, t.c_str(), -1, wide.data(), len);
	}
	title = wide;

	HWND hwnd = windowHandle.load();
	if (hwnd) {
		SetWindowTextW(hwnd, title.c_str());
	}
}

void EditorWidget::show()
{
	if (uiThread.joinable()) {
		return;
	}

	finished = false;
	windowReady = false;
	uiThread = std::thread(&EditorWidget::runWindow, this);

	// Block until the window exists (or its thread bailed) so close() and
	// handleResizeRequest() always see a valid handle and never race the
	// not-yet-created window.
	std::unique_lock<std::mutex> lock(readyMutex);
	readyCv.wait(lock, [this] { return windowReady; });
}

void EditorWidget::runWindow()
{
	static std::once_flag classOnce;
	std::call_once(classOnce, [] {
		WNDCLASSEXW wcex{sizeof(wcex)};
		wcex.lpfnWndProc = &EditorWidget::wndProc;
		wcex.hInstance = GetModuleHandleW(nullptr);
		wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
		wcex.lpszClassName = kEditorWindowClass;
		RegisterClassExW(&wcex);
	});

	// Fixed-size dialog-style frame: caption + system menu, no resize border
	// (VST editors are fixed-size and drive their own dimensions).
	windowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
	windowExStyle = 0;

	HWND hwnd = CreateWindowExW(windowExStyle, kEditorWindowClass, title.c_str(), windowStyle, CW_USEDEFAULT,
				   CW_USEDEFAULT, 300, 300, nullptr, nullptr, GetModuleHandleW(nullptr), this);
	windowHandle.store(hwnd);

	if (hwnd && effect) {
		effect->dispatcher(effect, effEditOpen, 0, 0, hwnd, 0);
		editOpened = true;
		applyEditRect();
		SetTimer(hwnd, kIdleTimerId, kIdleTimerMs, nullptr);
		ShowWindow(hwnd, SW_SHOW);
		UpdateWindow(hwnd);
	}

	{
		std::lock_guard<std::mutex> lock(readyMutex);
		windowReady = true;
	}
	readyCv.notify_one();

	if (!hwnd) {
		finished = true;
		return;
	}

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	windowHandle.store(nullptr);
	finished = true;
}

void EditorWidget::applyEditRect()
{
	HWND hwnd = windowHandle.load();
	if (!hwnd || !effect) {
		return;
	}

	VstRect *rect = nullptr;
	effect->dispatcher(effect, effEditGetRect, 0, 0, &rect, 0);

	int clientWidth = 300;
	int clientHeight = 300;
	if (rect) {
		// On Windows the size reported by the plugin is larger than its
		// actual size by the monitor's UI scale factor, so divide it out.
		UINT dpi = GetDpiForWindow(hwnd);
		double scale = dpi > 0 ? dpi / 96.0 : 1.0;
		clientWidth = static_cast<int>((rect->right - rect->left) / scale);
		clientHeight = static_cast<int>((rect->bottom - rect->top) / scale);
	}

	RECT wr{0, 0, clientWidth, clientHeight};
	AdjustWindowRectEx(&wr, windowStyle, FALSE, windowExStyle);
	SetWindowPos(hwnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
		     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void EditorWidget::handleResizeRequest(int, int)
{
	// Some plugins (e.g. SPAN by Voxengo) can't resize automatically, so we
	// resize the host window for them. This may arrive on the audio thread,
	// so marshal the actual resize onto the UI thread.
	HWND hwnd = windowHandle.load();
	if (hwnd) {
		PostMessageW(hwnd, WM_VST_RESIZE, 0, 0);
	}
}

void EditorWidget::close()
{
	HWND hwnd = windowHandle.load();
	if (hwnd) {
		PostMessageW(hwnd, WM_CLOSE, 0, 0);
	}
	if (uiThread.joinable()) {
		uiThread.join();
	}
}

LRESULT CALLBACK EditorWidget::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_CREATE) {
		auto cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
		return 0;
	}

	auto self = reinterpret_cast<EditorWidget *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

	switch (msg) {
	case WM_TIMER:
		if (self && self->effect && wParam == kIdleTimerId) {
			self->effect->dispatcher(self->effect, effEditIdle, 0, 0, nullptr, 0);
		}
		return 0;
	case WM_VST_RESIZE:
		if (self) {
			self->applyEditRect();
		}
		return 0;
	case WM_CLOSE:
		// effEditClose before destroying the host window so the plugin can
		// tear down its child GUI while its parent still exists.
		if (self && self->effect && self->editOpened.exchange(false)) {
			self->effect->dispatcher(self->effect, effEditClose, 0, 0, nullptr, 0);
		}
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		KillTimer(hwnd, kIdleTimerId);
		PostQuitMessage(0);
		return 0;
	default:
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
}
