#include <windows.h>

#include <string>

#include "include/cef_app.h"
#include "include/cef_browser.h"

#include "app.hpp"
#include "client.hpp"
#include "log.hpp"
#include "obs_bootstrap.hpp"
#include "scheme.hpp"

namespace {

constexpr int kHostWidth = 1280;
constexpr int kHostHeight = 720;
constexpr wchar_t kHostClassName[] = L"ObsMultiStreamShell";

CefRefPtr<CefBrowser> g_browser;

// The UI loads from the offline app:// bundle served by scheme.cpp.
const char *StartupUrl()
{
	return "app://app/index.html";
}

// Point the loader at the rundir bin dir so obs.dll + libobs-d3d11.dll resolve
// before any obs call. Derived from the exe path -- the exe sits in that dir.
void AddObsBinDirToSearchPath()
{
	wchar_t exe_path[MAX_PATH] = {0};
	GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
	std::wstring dir(exe_path);
	size_t slash = dir.find_last_of(L"\\/");
	if (slash != std::wstring::npos) {
		dir.resize(slash);
	}
	SetDllDirectoryW(dir.c_str());
}

void LayoutBrowser(HWND host)
{
	if (!g_browser) {
		return;
	}
	RECT rc;
	GetClientRect(host, &rc);
	HWND browser_hwnd = g_browser->GetHost()->GetWindowHandle();
	if (browser_hwnd) {
		SetWindowPos(browser_hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
			     SWP_NOZORDER | SWP_NOACTIVATE);
	}
}

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg) {
	case WM_SIZE:
		LayoutBrowser(hwnd);
		return 0;
	case WM_DESTROY:
		// Closing the host destroys its child browser window, which drives
		// Client::OnBeforeClose -> CefQuitMessageLoop.
		return 0;
	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

HWND CreateHostWindow(HINSTANCE instance)
{
	WNDCLASSEXW wc = {0};
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = HostWndProc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	wc.lpszClassName = kHostClassName;
	RegisterClassExW(&wc);

	RECT rc = {0, 0, kHostWidth, kHostHeight};
	const DWORD style = WS_OVERLAPPEDWINDOW;
	AdjustWindowRect(&rc, style, FALSE);

	return CreateWindowExW(0, kHostClassName, L"OBS MultiStreamer", style, CW_USEDEFAULT, CW_USEDEFAULT,
			       rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, instance, nullptr);
}

} // namespace

// Entry point for every CEF process (the browser process plus the render/GPU/
// utility subprocesses, which all re-launch this same executable).
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPTSTR, int)
{
	CefMainArgs main_args(hInstance);

	CefRefPtr<App> app(new App(StartupUrl()));

	// Subprocesses dispatch here and return >= 0.
	const int exit_code = CefExecuteProcess(main_args, app.get(), nullptr);
	if (exit_code >= 0) {
		return exit_code;
	}

	CefSettings settings;
	settings.no_sandbox = true;
	settings.multi_threaded_message_loop = false; // we drive CefRunMessageLoop()

	if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
		return CefGetExitCode();
	}
	HostLog("[cef] initialized");

	HWND host = CreateHostWindow(hInstance);
	if (!host) {
		HostLog("[host] CreateHostWindow failed -- aborting");
		CefShutdown();
		return 1;
	}
	ShowWindow(host, SW_SHOW);
	UpdateWindow(host);

	AddObsBinDirToSearchPath();

	if (!ObsBootstrap::Start()) {
		HostLog("[obs] ObsBootstrap::Start() failed -- aborting");
		CefShutdown();
		return 1;
	}

	CefRefPtr<Client> client(new Client());
	CefBrowserSettings browser_settings;
	CefWindowInfo window_info;
	window_info.SetAsChild(host, CefRect(0, 0, kHostWidth, kHostHeight));

	g_browser = CefBrowserHost::CreateBrowserSync(window_info, client, app->startup_url(), browser_settings, nullptr,
						      nullptr);
	if (!g_browser) {
		// No browser means OnBeforeClose never fires to quit the loop; bail
		// rather than hang in CefRunMessageLoop with no way out.
		HostLog("[cef] CreateBrowserSync failed -- aborting");
		ObsBootstrap::Stop();
		CefShutdown();
		return 1;
	}

	LayoutBrowser(host);

	CefRunMessageLoop();

	g_browser = nullptr;

	// Tear libobs down before CEF: obs holds a D3D11 device + module handles.
	ObsBootstrap::Stop();

	CefShutdown();
	HostLog("[cef] shutdown complete");
	return 0;
}
