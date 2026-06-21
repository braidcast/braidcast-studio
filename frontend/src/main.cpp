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
CefRefPtr<CefBrowser> g_browser;

// Owns the per-canvas preview surfaces (each an obs_display on its own child
// HWND). Created after ObsBootstrap::Start() succeeds; all surfaces destroyed
// before ObsBootstrap::Stop() (which frees the canvas mixes those displays render).
std::unique_ptr<PreviewManager> g_preview;

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

// The CEF UI browser fills the whole host client area. The obs preview is a
// separate overlay HWND positioned by the UI via preview.setRect (it floats
// above the browser within the sibling z-order), so layout only resizes the
// browser here -- the UI re-reports its preview rect on resize/scroll.
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
	case WM_TIMER:
		if (wparam == kSizeProbeTimerId) {
			KillTimer(hwnd, kSizeProbeTimerId);
			HostLog("[obs] active fps=" + std::to_string(obs_get_active_fps()));
			// Page is loaded by now: re-fire SCENE_CHANGED so the UI observes a
			// forwarded obs.event end-to-end (obs->shim->bridge->JS).
			ObsBootstrap::FireSceneChanged();
			// In the headless smoke path, prove the 4.3.2 properties bridge
			// round-trips before self-quit.
			if (getenv("FE_SMOKE_QUIT_SECONDS")) {
				ObsBootstrap::RunPropertiesSelfTest();
				ObsBootstrap::RunPreviewEditSelfTest();
				ObsBootstrap::RunSettingsSelfTest();
				ObsBootstrap::RunMultistreamModelSelfTest();
				ObsBootstrap::RunCanvasBridgeSelfTest();
				ObsBootstrap::RunStreamProfileBridgeSelfTest();
				ObsBootstrap::RunOutputBindingBridgeSelfTest();
				ObsBootstrap::RunMultistreamEngineSelfTest();
				ObsBootstrap::RunCanvasRuntimeSelfTest();
				ObsBootstrap::RunCanvasSceneSelfTest();
			}
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
	// Per-monitor-DPI-v2 before any window is created so client rects and the
	// preview overlay map 1:1 to device pixels (the UI reports CSS px x dpr; we
	// place the HWND in device px). Must run in every CEF process, harmless in the
	// subprocesses. Single-monitor correctness is the gate; mixed-DPI multimonitor
	// is a refinement (the overlay re-reports its rect on move, but cross-monitor
	// dpr changes mid-drag are not yet handled).
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

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

	// Create the preview manager now that libobs is up. Each canvas's overlay HWND +
	// obs_display are created lazily on its first preview.setRect from the UI, so a
	// canvas renders exactly into the region the Svelte app designates.
	g_preview = std::make_unique<PreviewManager>(host, hInstance);
	Preview::SetInstance(g_preview.get());

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
	RECT host_rc;
	GetClientRect(host, &host_rc);
	window_info.SetAsChild(host, CefRect(0, 0, host_rc.right - host_rc.left, host_rc.bottom - host_rc.top));

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

	// Both siblings clip each other so neither overdraws the boundary: the obs
	// overlay floats above the browser without flicker (browser HWND gets the bit
	// here; the overlay sets it at creation).
	if (HWND browser_hwnd = g_browser->GetHost()->GetWindowHandle()) {
		SetWindowLongPtrW(browser_hwnd, GWL_STYLE,
				  GetWindowLongPtrW(browser_hwnd, GWL_STYLE) | WS_CLIPSIBLINGS);
	}

	LayoutBrowser(host);

	CefRunMessageLoop();

	g_browser = nullptr;

	// Destroy every preview surface's obs_display + overlay HWND while libobs is
	// still up. This must precede ObsBootstrap::Stop(): an additional canvas's
	// surface renders that canvas's mix, and Stop() (via CanvasRuntime::ClearAll)
	// frees those mixes, so the displays must die first. The Default surface renders
	// the global mix, freed even later by obs_shutdown.
	if (g_preview) {
		Preview::SetInstance(nullptr);
		g_preview->DestroyAll();
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
