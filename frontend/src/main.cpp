#include <obs.h>

#include <windows.h>

#include <cstdlib>
#include <memory>
#include <string>

#include "include/cef_app.h"
#include "include/cef_browser.h"

#include "app.hpp"
#include "client.hpp"
#include "log.hpp"
#include "obs_bootstrap.hpp"
#include "preview_window.hpp"
#include "scheme.hpp"

namespace {

constexpr int kHostWidth = 1280;
constexpr int kHostHeight = 720;
constexpr wchar_t kHostClassName[] = L"ObsMultiStreamShell";
constexpr UINT_PTR kSizeProbeTimerId = 1;
constexpr UINT_PTR kSmokeQuitTimerId = 2;

// Host-window-owned handles. Single-threaded (browser process UI thread).
HWND g_preview_hwnd = nullptr;
CefRefPtr<CefBrowser> g_browser;

// Owns the obs_display attached to the preview child HWND. Created after
// ObsBootstrap::Start() succeeds; destroyed before ObsBootstrap::Stop().
std::unique_ptr<PreviewWindow> g_preview;

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

// Split the client area: obs preview child HWND on the LEFT half, the embedded
// CEF UI browser on the RIGHT half.
void LayoutChildren(HWND host)
{
	RECT rc;
	GetClientRect(host, &rc);
	const int width = rc.right - rc.left;
	const int height = rc.bottom - rc.top;
	const int half = width / 2;

	if (g_preview_hwnd) {
		MoveWindow(g_preview_hwnd, 0, 0, half, height, TRUE);
		if (g_preview) {
			g_preview->Resize(uint32_t(half), uint32_t(height));
		}
	}

	if (g_browser) {
		HWND browser_hwnd = g_browser->GetHost()->GetWindowHandle();
		if (browser_hwnd) {
			SetWindowPos(browser_hwnd, nullptr, half, 0, width - half, height,
				     SWP_NOZORDER | SWP_NOACTIVATE);
		}
	}
}

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg) {
	case WM_CREATE: {
		// Bare child HWND for the preview region; an obs_display attaches to it.
		g_preview_hwnd = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, kHostWidth / 2,
						 kHostHeight, hwnd, nullptr,
						 reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)),
						 nullptr);
		return 0;
	}
	case WM_SIZE:
		LayoutChildren(hwnd);
		return 0;
	case WM_TIMER:
		if (wparam == kSizeProbeTimerId) {
			KillTimer(hwnd, kSizeProbeTimerId);
			obs_source_t *src = obs_get_source_by_name("fe-test-web");
			if (src) {
				HostLog("[obs] fe-test-web size: " + std::to_string(obs_source_get_width(src)) + "x" +
					std::to_string(obs_source_get_height(src)) +
					" (active fps=" + std::to_string(obs_get_active_fps()) + ")");
				obs_source_release(src);
			} else {
				HostLog("[obs] fe-test-web not found for size probe");
			}
			// Page is loaded by now: re-fire SCENE_CHANGED so the UI observes a
			// forwarded obs.event end-to-end (obs->shim->bridge->JS).
			ObsBootstrap::FireSceneChanged();
		} else if (wparam == kSmokeQuitTimerId) {
			KillTimer(hwnd, kSmokeQuitTimerId);
			HostLog("[host] smoke-quit timer fired -> WM_CLOSE");
			PostMessageW(hwnd, WM_CLOSE, 0, 0);
		}
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

// Drain CEF-posted teardown tasks (BrowserSource `delete this` + CloseBrowser)
// after CefRunMessageLoop has returned but before CefShutdown. Bounded so a
// stuck task can't hang teardown.
void DrainCefTasks()
{
	for (int i = 0; i < 50; ++i) {
		CefDoMessageLoopWork();
		Sleep(2);
	}
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
	settings.windowless_rendering_enabled = true;  // required for obs-browser OSR sources

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

	// Attach an obs_display to the preview child HWND so the active scene renders
	// into the left region each frame. The child HWND was created in WM_CREATE.
	if (g_preview_hwnd) {
		g_preview = std::make_unique<PreviewWindow>(g_preview_hwnd);
		g_preview->CreateDisplay();
	}

	// Probe the test source's size after its async CEF browser has spun up.
	SetTimer(host, kSizeProbeTimerId, 4000, nullptr);

	// Env-gated headless smoke path: self-terminate after N seconds so log
	// capture is automatable. Inert without FE_SMOKE_QUIT_SECONDS.
	if (const char *secs = getenv("FE_SMOKE_QUIT_SECONDS")) {
		const int n = atoi(secs);
		if (n > 0) {
			HostLog("[host] smoke-quit armed: " + std::to_string(n) + "s");
			SetTimer(host, kSmokeQuitTimerId, UINT(n) * 1000, nullptr);
		}
	}

	CefRefPtr<Client> client(new Client());
	CefBrowserSettings browser_settings;
	CefWindowInfo window_info;
	window_info.SetAsChild(host, CefRect(kHostWidth / 2, 0, kHostWidth / 2, kHostHeight));

	g_browser = CefBrowserHost::CreateBrowserSync(window_info, client, app->startup_url(), browser_settings, nullptr,
						      nullptr);
	if (!g_browser) {
		// No browser means OnBeforeClose never fires to quit the loop; bail
		// rather than hang in CefRunMessageLoop with no way out.
		HostLog("[cef] CreateBrowserSync failed -- aborting");
		ObsBootstrap::TeardownScene();
		DrainCefTasks();
		ObsBootstrap::Stop();
		CefShutdown();
		return 1;
	}

	LayoutChildren(host);

	CefRunMessageLoop();

	g_browser = nullptr;

	// Destroy the obs_display while libobs is still up, before obs_shutdown
	// tears down the graphics device.
	if (g_preview) {
		g_preview->Destroy();
		g_preview.reset();
	}

	// Release the test scene + browser source, then pump CEF so the source's
	// posted `delete this` / CloseBrowser tasks drain (the run-loop has already
	// returned; these would otherwise leak/dangle into CefShutdown).
	ObsBootstrap::TeardownScene();
	DrainCefTasks();

	// Tear libobs down before CEF: obs holds a D3D11 device + module handles.
	ObsBootstrap::Stop();

	CefShutdown();
	HostLog("[cef] shutdown complete");
	return 0;
}
