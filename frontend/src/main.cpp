#include <obs.h>
#include "event_names.hpp"

#include <windows.h>

#include <cstdlib>
#include <memory>
#include <string>

#include "include/cef_app.h"
#include "include/cef_browser.h"

#include "app.hpp"
#include "app_icon.hpp"
#include "bridge.hpp"
#include "client.hpp"
#include "GeneralSettings.hpp"
#include "interact_window.hpp"
#include "log.hpp"
#include "obs_bootstrap.hpp"
#include "perf_repro_selftest.hpp"
#include "preview_window.hpp"
#include "projector_window.hpp"
#include "scene_persistence.hpp"
#include "scheme.hpp"
#include "tray.hpp"
#include "window_chrome.hpp"
#include "window_manager.hpp"

namespace {

constexpr int kHostWidth = 1280;
constexpr int kHostHeight = 720;
constexpr wchar_t kHostClassName[] = L"BraidcastShell";
constexpr UINT_PTR kSizeProbeTimerId = 1;
constexpr UINT_PTR kSmokeQuitTimerId = 2;
// Drives the perf-repro self-test state machine (BRAIDCAST_SELFTEST_STREAM=
// perf-repro); distinct from both timers above so the two selftest modes never
// collide on one id.
constexpr UINT_PTR kPerfReproTimerId = 3;

// Host-window-owned handles. Single-threaded (browser process UI thread).
CefRefPtr<CefBrowser> g_browser;

// Owns the per-canvas preview surfaces (each an obs_display on its own child
// HWND). Created after ObsBootstrap::Start() succeeds; all surfaces destroyed
// before ObsBootstrap::Stop() (which frees the canvas mixes those displays render).
std::unique_ptr<PreviewManager> g_preview;

// Creates + tracks detached top-level windows torn off the main shell. Each owns
// its own child browser sharing the process-wide Client, so all browsers drain
// from one browser_list_ at quit. Stood up after the main browser exists.
std::unique_ptr<WindowManager> g_windows;

// Owns the native projectors (each a top-level obs_display window). Created after
// the preview manager; every projector's display is destroyed (DestroyAll) before
// ObsBootstrap::Stop() frees the canvas mixes a canvas projector renders.
std::unique_ptr<ProjectorManager> g_projector;

// Owns the native source-interaction windows (each a top-level obs_display window
// forwarding input into one source). Created next to the projector manager; every
// window's display is destroyed (DestroyAll) before ObsBootstrap::Stop() frees the
// source mixes.
std::unique_ptr<InteractManager> g_interact;

// Owns the system-tray icon + its context menu (Show/Hide, Start/Stop All,
// Virtual Camera toggle, Exit). Created after ObsBootstrap::Start() (the menu
// actions reach into the engine + settings). NIM_DELETE'd in WM_CLOSE before the
// host window is destroyed.
std::unique_ptr<TrayIcon> g_tray;

// The UI loads from the offline app:// bundle served by scheme.cpp. For live UI
// development, set FE_DEV_URL (e.g. http://localhost:5173) to point the window at a
// Vite dev server instead -- enables HMR. The native bridge is injected per-frame by
// CEF regardless of origin, so window.obs still works off the dev server.
const char *StartupUrl()
{
	const char *dev = getenv("FE_DEV_URL");
	if (dev && *dev) {
		return dev;
	}
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
	// Frameless command-deck chrome (custom caption, resize borders, drag regions,
	// min size) is applied first; anything it fully handles short-circuits here.
	LRESULT chromeResult = 0;
	if (WindowChrome::HandleMessage(hwnd, msg, wparam, lparam, chromeResult)) {
		return chromeResult;
	}

	switch (msg) {
	case WM_SIZE:
		LayoutBrowser(hwnd);
		// Reflect maximize/restore so the web title bar's glyph can toggle. Main
		// window is windowId 0.
		if (wparam == SIZE_MAXIMIZED || wparam == SIZE_RESTORED) {
			Bridge::EmitEvent(EventNames::kWindowStateChanged,
					  Bridge::json{{"windowId", 0}, {"maximized", wparam == SIZE_MAXIMIZED}});
		}
		return 0;
	case TrayIcon::kTrayCallbackMsg:
		if (g_tray) {
			g_tray->OnCallback(wparam, lparam);
		}
		return 0;
	case WM_SYSCOMMAND:
		// Intercept the minimize gesture when "minimize to tray" is on: hide the
		// window to the tray instead of minimizing to the taskbar. Mask off the
		// low 4 bits (system reserves them) before comparing the command.
		if ((wparam & 0xFFF0) == SC_MINIMIZE && g_tray && ObsBootstrap::General().minimizeToTray) {
			g_tray->HideHost();
			return 0;
		}
		break;
	case WM_TIMER:
		if (wparam == kSizeProbeTimerId) {
			KillTimer(hwnd, kSizeProbeTimerId);
			HostLog("[obs] active fps=" + std::to_string(obs_get_active_fps()));
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
				ObsBootstrap::RunSceneDuplicateSelfTest();
				ObsBootstrap::RunTransformPivotSelfTest();
				ObsBootstrap::RunRotationBoundsSelfTest();
				ObsBootstrap::RunPreviewSurfaceIsolationSelfTest();
				ObsBootstrap::RunProjectorSelfTest();
				ObsBootstrap::RunAudioMixerSelfTest();
				ObsBootstrap::RunHotkeysSelfTest();
				ObsBootstrap::RunStatsSelfTest();
				ObsBootstrap::RunMcpSelfTest();
				ObsBootstrap::RunEventSelfTest();
				ObsBootstrap::RunOverlaySelfTest();
			}
		} else if (wparam == kSmokeQuitTimerId) {
			KillTimer(hwnd, kSmokeQuitTimerId);
			HostLog("[host] smoke-quit timer fired -> WM_CLOSE");
			PostMessageW(hwnd, WM_CLOSE, 0, 0);
		} else if (wparam == kPerfReproTimerId) {
			if (ObsBootstrap::RunPerfReproSelfTest()) {
				KillTimer(hwnd, kPerfReproTimerId);
				HostLog("[host] perf-repro selftest finished -> WM_CLOSE");
				PostMessageW(hwnd, WM_CLOSE, 0, 0);
			}
		}
		return 0;
	case WM_CLOSE:
		// Remove the tray icon while the host HWND is still valid (NIM_DELETE keys
		// off it); otherwise a ghost icon lingers in the notification area.
		if (g_tray) {
			g_tray->Remove();
		}
		// Close every detached window first so the shared browser_list_ can drain
		// to empty -- a live detached browser would otherwise keep
		// CefRunMessageLoop alive after the main window closes (the N-browsers quit
		// guard). Their WM_CLOSE posts are processed before the loop drains.
		if (g_windows) {
			g_windows->CloseAll();
		}
		break;
	case WM_DESTROY:
		// Closing the host destroys its child browser window, which drives
		// Client::OnBeforeClose -> CefQuitMessageLoop.
		WindowChrome::Discard(hwnd);
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
	ApplyAppIcon(wc, instance);
	RegisterClassExW(&wc);

	RECT rc = {0, 0, kHostWidth, kHostHeight};
	const DWORD style = WS_OVERLAPPEDWINDOW;
	AdjustWindowRect(&rc, style, FALSE);

	return CreateWindowExW(0, kHostClassName, L"Braidcast", style, CW_USEDEFAULT, CW_USEDEFAULT,
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

	// Install the unhandled-exception filter as the first thing the main process
	// does, so a crash anywhere below -- CEF init, obs startup, the message loop --
	// writes a minidump for field diagnosis. Main process only; the CEF subprocesses
	// returned above.
	obs_init_win32_crash_handler();

	// Opt the main process out of Windows background power throttling. When
	// Braidcast loses the foreground (e.g. a fullscreen game grabs focus on
	// another monitor) Windows otherwise drops it into Efficiency Mode (EcoQoS)
	// and revokes the 1ms timer libobs asked for, which coarsens compositor and
	// encoder frame pacing and surfaces as render lag until the window is
	// refocused. Clearing both control bits (StateMask 0) keeps the process at
	// full speed and honors its timer request while backgrounded, matching OBS's
	// fix for obsproject/obs-studio#12982. The OSR browser subprocess sets its
	// own opt-out; this covers the process that runs the video pipeline. No-op on
	// pre-1709 Windows.
	PROCESS_POWER_THROTTLING_STATE throttling = {};
	throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
	throttling.ControlMask =
		PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
	throttling.StateMask = 0;
	SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));

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
	// Remove the stock caption + enable the frameless chrome before the window is
	// shown, so no framed frame flashes on first paint.
	WindowChrome::Init(host, WindowChrome::kMainWindow);
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
	// Main window is windowId 0; its surfaces parent to this host HWND. (id 0 also
	// falls back to the constructor's host_; future detached windows register their
	// own host HWND under their windowId via the same RegisterWindow seam.)
	g_preview->RegisterWindow(0, host);

	// Stand up the projector manager (top-level projector windows; no parent host).
	g_projector = std::make_unique<ProjectorManager>(hInstance);
	Projector::SetInstance(g_projector.get());

	// Stand up the source-interaction manager (top-level interaction windows).
	g_interact = std::make_unique<InteractManager>(hInstance);
	Interact::SetInstance(g_interact.get());

	// System tray. Created here so its menu actions (Start/Stop All, Virtual
	// Camera, settings reads) have the engine + settings available. Policy: pin
	// the icon now only when alwaysShowTray is set; otherwise it is added lazily
	// the first time the window minimizes to the tray (HideHost) and removed when
	// restored (ShowHost). Start-minimized hides straight to the tray.
	g_tray = std::make_unique<TrayIcon>();
	if (ObsBootstrap::General().alwaysShowTray) {
		g_tray->Create(host, hInstance);
	}
	if (ObsBootstrap::General().startMinimized) {
		g_tray->HideHost();
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

	// Env-gated automated perf-repro self-test: the regression gate for the
	// background power-throttling opt-out above (the SetProcessInformation/
	// ProcessPowerThrottling call). Network-free and deterministic -- it verifies
	// the opt-out is in force, minimizes the host window to simulate losing the
	// foreground, samples render pacing via stats.get for a fixed window, then
	// quits with a PASS/FAIL exit code. Inert without
	// BRAIDCAST_SELFTEST_STREAM=perf-repro.
	bool perfReproArmed = false;
	if (const char *mode = getenv("BRAIDCAST_SELFTEST_STREAM")) {
		if (std::string(mode) == "perf-repro") {
			HostLog("[host] perf-repro selftest armed");
			ObsBootstrap::ArmPerfReproSelfTest(host);
			SetTimer(host, kPerfReproTimerId, 500, nullptr);
			perfReproArmed = true;
		}
	}

	// ONE Client for the whole process, published via Client::SetShared so future
	// additional browsers (e.g. detached windows) reuse it: all browsers land in the
	// same browser_list_ + Bridge emit registry and the loop quits only when the
	// LAST browser closes.
	CefRefPtr<Client> client(new Client());
	Client::SetShared(client);
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
	DBG(LogCat::Lifecycle, "cef browser created");

	// Both siblings clip each other so neither overdraws the boundary: the obs
	// overlay floats above the browser without flicker (browser HWND gets the bit
	// here; the overlay sets it at creation).
	if (HWND browser_hwnd = g_browser->GetHost()->GetWindowHandle()) {
		SetWindowLongPtrW(browser_hwnd, GWL_STYLE,
				  GetWindowLongPtrW(browser_hwnd, GWL_STYLE) | WS_CLIPSIBLINGS);
	}

	LayoutBrowser(host);

	// Stand up the detached-window manager. Detached child browsers reuse the shared
	// Client (Client::Shared) so they join the same browser_list_ + emit registry.
	g_windows = std::make_unique<WindowManager>(hInstance, "app://app/index.html");
	WindowManager::SetInstance(g_windows.get());

	CefRunMessageLoop();

	g_browser = nullptr;

	// Drop the process-wide shared Client ref so it doesn't outlive teardown.
	Client::SetShared(nullptr);

	// Tray icon was already NIM_DELETE'd in WM_CLOSE (while the HWND was valid);
	// drop the owner here. Remove() is idempotent if WM_CLOSE didn't run.
	if (g_tray) {
		g_tray->Remove();
		g_tray.reset();
	}

	// Tear down every detached window's surfaces + browser BEFORE g_preview->DestroyAll()
	// and ObsBootstrap::Stop() free the canvas mixes those surfaces render (UAF discipline).
	if (g_windows) {
		WindowManager::SetInstance(nullptr);
		g_windows->DestroyAll();
		g_windows.reset();
	}

	// Destroy every preview surface's obs_display + overlay HWND while libobs is
	// still up. This must precede ObsBootstrap::Stop(): an additional canvas's
	// surface renders that canvas's mix, and Stop() (via CanvasRuntime::ClearAll)
	// frees those mixes, so the displays must die first. The Default surface renders
	// the global mix, freed even later by obs_shutdown.
	// Tear down every projector's obs_display + window while libobs is up and BEFORE
	// the canvas mixes are freed (a canvas projector's display renders a canvas mix).
	if (g_projector) {
		Projector::SetInstance(nullptr);
		g_projector->DestroyAll();
		g_projector.reset();
	}

	if (g_interact) {
		Interact::SetInstance(nullptr);
		g_interact->DestroyAll();
		g_interact.reset();
	}

	if (g_preview) {
		Preview::SetInstance(nullptr);
		g_preview->DestroyAll();
		g_preview.reset();
	}

	// Capture the latest global scene collection on a clean exit, while channel 0
	// and every scene source are still bound -- TeardownScene below unbinds and
	// removes the placeholder default scene, so this must run first.
	SceneCollection::Save();

	// Release the test scene + browser source, then pump CEF so the source's
	// posted `delete this` / CloseBrowser tasks drain (the run-loop has already
	// returned; these would otherwise leak/dangle into CefShutdown).
	ObsBootstrap::TeardownScene();
	DrainCefTasks();

	// Tear libobs down before CEF: obs holds a D3D11 device + module handles.
	ObsBootstrap::Stop();

	CefShutdown();
	HostLog("[cef] shutdown complete");
	// Propagate the perf-repro self-test's PASS/FAIL/skip/error exit code (see
	// perf_repro_selftest.hpp) so it can gate CI; every other path keeps the
	// existing always-0 exit.
	return perfReproArmed ? ObsBootstrap::PerfReproSelfTestExitCode() : 0;
}
