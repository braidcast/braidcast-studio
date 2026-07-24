#include <obs.h>
#include <util/platform.h>
#include "event_names.hpp"

#include <windows.h>
#include <objbase.h>
#include <rtworkq.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "include/cef_app.h"
#include "include/cef_browser.h"

#include "app.hpp"
#include "windowing/app_icon.hpp"
#include "bridge.hpp"
#include "client.hpp"
#include "diag/gpu_diag.hpp"
#include "gpu_safe_mode.hpp"
#include "settings/GeneralSettings.hpp"
#include "windowing/interact_window.hpp"
#include "log.hpp"
#include "multistream/StorePaths.hpp"
#include "obs_bootstrap.hpp"
#include "perf_repro_selftest.hpp"
#include "windowing/native_theme.hpp"
#include "windowing/preview_window.hpp"
#include "windowing/projector_window.hpp"
#include "scene/scene_persistence.hpp"
#include "scheme.hpp"
#include "util/env_config.hpp"
#include "util/paths.hpp"
#include "util/session_log.hpp"
#include "windowing/tray.hpp"
#include "windowing/window_chrome.hpp"
#include "windowing/window_manager.hpp"

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

// Process-lifetime single-instance guard. Held from startup to a clean exit; the
// OS releases it if the process dies. Keyed on the resolved config dir so a
// portable dev build and an installed release (different config bases) never block
// each other -- only "same config launched twice", the real state-corruption
// hazard, is refused.
HANDLE g_instance_mutex = nullptr;

// Init-stage guards for the two Teardown() steps that are only valid once their
// subsystem is actually up. An early bail-out reaches the one shared Teardown()
// before CEF or libobs exist, so these gate the steps that would otherwise touch
// an uninitialized subsystem. Set true at the single real init site; checked in
// Teardown(). (RTWQ needs no such flag: RtwqStartup() runs unconditionally before
// every bail-out, so RtwqShutdown() is always paired.) Because ObsBootstrap::Start()
// runs after CefInitialize(), g_obsStarted implies g_cefInitialized.
bool g_cefInitialized = false;
bool g_obsStarted = false;

// The UI loads from the offline app:// bundle served by scheme.cpp. For live UI
// development, set FE_DEV_URL (e.g. http://localhost:5173) to point the window at a
// Vite dev server instead -- enables HMR. The native bridge is injected per-frame by
// CEF regardless of origin, so window.obs still works off the dev server.
const char *StartupUrl()
{
	static const std::string dev = Env::Value("FE_DEV_URL");
	if (!dev.empty()) {
		return dev.c_str();
	}
	return "app://app/index.html";
}

// Enable the process privileges the hardened startup path wants: SE_DEBUG (parity
// with upstream OBS's process hardening) and SE_INC_BASE_PRIORITY, which the
// GPU_PRIORITY_VAL-gated D3D11 GPU-scheduling-priority path needs to succeed. Ported
// from win-capture/inject-helper's load_debug_privilege, extended to both privileges.
void load_debug_privilege()
{
	const DWORD flags = TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY;
	const wchar_t *const privileges[] = {SE_DEBUG_NAME, SE_INC_BASE_PRIORITY_NAME};
	TOKEN_PRIVILEGES tp;
	HANDLE token;
	LUID val;

	if (!OpenProcessToken(GetCurrentProcess(), flags, &token)) {
		return;
	}

	for (const wchar_t *privilege : privileges) {
		if (!!LookupPrivilegeValue(nullptr, privilege, &val)) {
			tp.PrivilegeCount = 1;
			tp.Privileges[0].Luid = val;
			tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

			AdjustTokenPrivileges(token, false, &tp, sizeof(tp), nullptr, nullptr);
		}
	}

	CloseHandle(token);
}

// Point the loader at the rundir bin dir so obs.dll + libobs-d3d11.dll resolve
// before any obs call. Derived from the exe path -- the exe sits in that dir.
void AddObsBinDirToSearchPath()
{
	const std::wstring dir = ExecutableDir();
	SetDllDirectoryW(dir.c_str());
	// Under SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) the
	// SetDllDirectoryW directory is not searched, so also register this dir via
	// AddDllDirectory (LOAD_LIBRARY_SEARCH_USER_DIRS, which DEFAULT_DIRS includes) --
	// obs.dll + its co-located deps still resolve. The SetDllDirectoryW call above is
	// harmless and kept.
	AddDllDirectory(dir.c_str());
}

// FNV-1a of the config dir, ASCII-case-folded so path-casing variance can't split
// one install into two instances. Yields a mutex-name-safe hex token: a raw path
// can't name a mutex (backslash is the object-namespace separator).
std::wstring ConfigInstanceToken(const std::string &configDir)
{
	uint64_t hash = 1469598103934665603ULL;
	for (unsigned char c : configDir) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<unsigned char>(c - 'A' + 'a');
		}
		hash ^= c;
		hash *= 1099511628211ULL;
	}
	wchar_t token[17];
	swprintf(token, 17, L"%016llx", static_cast<unsigned long long>(hash));
	return std::wstring(token);
}

// Become the single instance for `configDir`. Returns false when another instance
// already owns it, after a best-effort raise of that instance's host window.
bool AcquireSingleInstance(const std::string &configDir)
{
	const std::wstring name = L"Local\\braidcast-singleton-" + ConfigInstanceToken(configDir);
	g_instance_mutex = CreateMutexW(nullptr, TRUE, name.c_str());
	if (g_instance_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
		if (HWND existing = FindWindowW(kHostClassName, nullptr)) {
			if (IsIconic(existing)) {
				ShowWindow(existing, SW_RESTORE);
			}
			SetForegroundWindow(existing);
		}
		return false;
	}
	return true;
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

// Persist the main host window's placement (last monitor + maximized state) to
// window.json so it reopens where it last closed. A minimized window is stored as
// normal so the app never reopens hidden to the taskbar.
void SaveHostPlacement(HWND host)
{
	WINDOWPLACEMENT wp;
	wp.length = sizeof(wp);
	if (!GetWindowPlacement(host, &wp)) {
		return;
	}
	const UINT showCmd = wp.showCmd == SW_SHOWMINIMIZED ? UINT(SW_SHOWNORMAL) : wp.showCmd;

	obs_data_t *root = obs_data_create();
	obs_data_set_int(root, "showCmd", showCmd);
	obs_data_set_int(root, "flags", wp.flags);
	obs_data_set_int(root, "left", wp.rcNormalPosition.left);
	obs_data_set_int(root, "top", wp.rcNormalPosition.top);
	obs_data_set_int(root, "right", wp.rcNormalPosition.right);
	obs_data_set_int(root, "bottom", wp.rcNormalPosition.bottom);
	const std::string path = MultistreamBasicPath("window.json");
	ReportSaveResult(SaveJsonAtomic(root, path), path);
	obs_data_release(root);
}

// Restore the placement saved by SaveHostPlacement before the window is first
// shown. Skips restore when the saved rect no longer intersects any monitor (a
// display was disconnected) so the window can't reopen off-screen, leaving the
// default CW_USEDEFAULT placement in that case.
void RestoreHostPlacement(HWND host)
{
	const std::string path = MultistreamBasicPath("window.json");
	obs_data_t *root = obs_data_create_from_json_file(path.c_str());
	if (!root) {
		return;
	}

	WINDOWPLACEMENT wp = {};
	wp.length = sizeof(wp);
	wp.showCmd = UINT(obs_data_get_int(root, "showCmd"));
	wp.flags = UINT(obs_data_get_int(root, "flags"));
	wp.rcNormalPosition.left = LONG(obs_data_get_int(root, "left"));
	wp.rcNormalPosition.top = LONG(obs_data_get_int(root, "top"));
	wp.rcNormalPosition.right = LONG(obs_data_get_int(root, "right"));
	wp.rcNormalPosition.bottom = LONG(obs_data_get_int(root, "bottom"));
	obs_data_release(root);

	if (!MonitorFromRect(&wp.rcNormalPosition, MONITOR_DEFAULTTONULL)) {
		return;
	}
	SetWindowPlacement(host, &wp);
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
			if (Env::IsSet("FE_SMOKE_QUIT_SECONDS")) {
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
				ObsBootstrap::RunNativeThemeSelfTest();
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
		// Snapshot the placement while the HWND is still valid so the next launch
		// reopens on the last monitor / maximized state. Skipped under the headless
		// smoke path, which never restored placement (it opens at the default rect),
		// so saving here would overwrite the user's real window.json with that default.
		if (!Env::IsSet("FE_SMOKE_QUIT_SECONDS")) {
			SaveHostPlacement(hwnd);
		}
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
	// The whole client area is covered by child/DComp render surfaces (CEF UI + the native
	// preview child) and is never GDI-painted, so a background brush + CS_HREDRAW/VREDRAW
	// only ever flash: whenever DWM briefly recomposes (e.g. the preview swapchain being
	// rebuilt on a modal open) the unpainted host surface is exposed. A null brush leaves it
	// untouched instead of erasing it white, and WS_CLIPCHILDREN keeps the parent from
	// painting over the children at all.
	wc.style = 0;
	wc.lpfnWndProc = HostWndProc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = nullptr;
	wc.lpszClassName = kHostClassName;
	ApplyAppIcon(wc, instance);
	RegisterClassExW(&wc);

	RECT rc = {0, 0, kHostWidth, kHostHeight};
	const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
	AdjustWindowRect(&rc, style, FALSE);

	return CreateWindowExW(0, kHostClassName, L"Braidcast", style, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
			       rc.bottom - rc.top, nullptr, nullptr, instance, nullptr);
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

// The one ordered teardown every exit runs -- the clean-exit path (after
// CefRunMessageLoop returns), the CreateBrowserSync-failure abort, and the earlier
// !CefInitialize / !host / !Start() bail-outs. The later callers have libobs up,
// CEF up, RTWQ started and the mutex held; the early callers reach here before
// libobs (and, for !CefInitialize, before CEF) exist. Per-owner guards cover the
// window/surface differences, g_cefInitialized / g_obsStarted gate the CEF/libobs
// steps for the early callers, and RTWQ + the mutex are always paired by this
// point. Order is load-bearing: every obs_display-backed surface is destroyed
// before ObsBootstrap::Stop() frees the canvas mixes those surfaces render.
void Teardown()
{
	// Stop the GPU-diagnostics sampler and disconnect its source_create hook before
	// any obs teardown: the sampler thread enumerates live obs sources/outputs, so it
	// must be joined before ObsBootstrap::Stop()/obs_shutdown free them. Idempotent
	// and safe on early-abort paths where the diagnostics never armed.
	GpuDiag::Stop();

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

	// libobs teardown, only once ObsBootstrap::Start() fully succeeded. An early
	// bail-out before/at Start() reaches here with libobs not up, where these steps
	// (channel-0 scene save, scene unbind, engine Stop) would touch an uninitialized
	// engine. g_obsStarted implies g_cefInitialized, so the CEF pumps below are live.
	if (g_obsStarted) {
		// Capture the latest global scene collection while channel 0 and every scene
		// source are still bound -- TeardownScene below unbinds and removes the
		// placeholder default scene, so this must run first. Skipped under the
		// browser-source kill switch: that run blanked every browser source's URL in
		// memory, so saving would persist about:blank over the user's real URLs.
		if (!GpuDiag::BrowserSourcesDisabled()) {
			SceneCollection::Save();
		}

		// Release the test scene + browser source, then pump CEF so the source's
		// posted `delete this` / CloseBrowser tasks drain (the run-loop has already
		// returned; these would otherwise leak/dangle into CefShutdown).
		ObsBootstrap::TeardownScene();
		DrainCefTasks();

		// Tear libobs down before CEF: obs holds a D3D11 device + module handles.
		// Stop pumps the injected drain after its source-removal sweep -- the browser
		// sources a loaded scene collection restored die there, not in TeardownScene
		// above, and their posted TID_UI teardown must run before CefShutdown.
		ObsBootstrap::Stop(DrainCefTasks);
	}

	// Only valid once CefInitialize() succeeded; the !CefInitialize bail-out reaches
	// here with CEF not up, where CefShutdown() must not be called.
	if (g_cefInitialized) {
		CefShutdown();
		HostLog("[cef] shutdown complete");
	}

	// Pair the startup RtwqStartup(): tear the MF Real-Time Work Queue down now that
	// obs (and its MF encoders) have stopped. Unconditional -- RtwqStartup() runs
	// before every bail-out that reaches Teardown(), so the pair is never unbalanced.
	RtwqShutdown();

	// Release the single-instance guard acquired at startup. Guarded so an early
	// bail-out that never acquired it is still safe.
	if (g_instance_mutex) {
		ReleaseMutex(g_instance_mutex);
		CloseHandle(g_instance_mutex);
		g_instance_mutex = nullptr;
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

	// Windows process hardening, mirroring upstream OBS's frontend/obs-main.cpp
	// startup preamble (never replicated in this from-scratch CEF entry). Main process
	// only -- the CEF subprocesses returned above -- and run before the crash handler
	// and any DLL load so it governs the whole process lifetime.

	// Suppress the OS hard-error popups (missing-DLL / disk-not-ready modal dialogs);
	// a failure then surfaces in our log path instead of a blocking message box.
	SetErrorMode(SEM_FAILCRITICALERRORS);

	// Give this process a high orderly-shutdown priority (0x300, above the 0x280
	// default) and do not retry shutdown if a blocking callback fails.
	SetProcessShutdownParameters(0x300, SHUTDOWN_NORETRY);

	// Safe DLL search: enable safe-search mode and restrict the default search set to
	// System32, the application dir, and AddDllDirectory-registered dirs. This drops
	// the SetDllDirectoryW path from the search order, so AddObsBinDirToSearchPath()
	// also registers the exe dir via AddDllDirectory (LOAD_LIBRARY_SEARCH_USER_DIRS)
	// -- obs.dll + its co-located deps still resolve.
	SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE | BASE_SEARCH_PATH_PERMANENT);
	SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

	// Force system-critical DLLs to load only from System32, blocking DLL-planting via
	// a co-located same-named DLL. Only affects system DLL resolution, so it is safe
	// for the co-located obs.dll. Copied from win-capture/inject-helper.
	PROCESS_MITIGATION_IMAGE_LOAD_POLICY image_load_policy = {0};
	image_load_policy.PreferSystem32Images = 1;
	SetProcessMitigationPolicy(ProcessImageLoadPolicy, &image_load_policy, sizeof(image_load_policy));

	// Enable SE_DEBUG + SE_INC_BASE_PRIORITY for this process (see load_debug_privilege).
	load_debug_privilege();

	// Bring up the Media Foundation Real-Time Work Queue so MF hardware-encoder worker
	// threads get real-time scheduling; paired with RtwqShutdown() on the normal
	// teardown path below.
	RtwqStartup();

	// Route bcrash() to a real sink BEFORE the exception filter below is
	// installed: libobs' default crash handler prints to stderr (invisible in a
	// GUI process) and exits 0, so a crash would both vanish and report
	// success. The sink writes <config>/crashes/ and exits non-zero.
	SessionLog::InstallCrashHandler();

	// Install the unhandled-exception filter as the first thing the main process
	// does after hardening, so a crash anywhere below -- CEF init, obs startup, the
	// message loop -- writes a crash report for field diagnosis. Main process only;
	// the CEF subprocesses returned above.
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
	throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
				 PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
	throttling.StateMask = 0;
	SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));

	// Point the loader at the rundir bin dir before the first obs call: portable-mode
	// detection below resolves the config base through os_get_config_path in the
	// non-portable case, and every later obs/D3D11 load needs this in place too.
	AddObsBinDirToSearchPath();

	// Single-instance guard, before any CEF/libobs/window bring-up so a rejected
	// second launch exits cleanly with no partial state. Keyed on the resolved
	// config dir (BraidcastConfigDir, shared with every store) so only a launch
	// against the SAME config is refused -- a portable dev build and an installed
	// release keep their own single instances without blocking each other.
	if (!AcquireSingleInstance(BraidcastConfigDir())) {
		HostLog("[host] another instance already owns this config -- exiting");
		// RtwqStartup() already ran; route through Teardown() to shut it down and
		// drop our (unowned) mutex handle. CEF/libobs steps are skipped (flags still
		// false), and ReleaseMutex on the mutex we do not own is a harmless no-op.
		Teardown();
		return 0;
	}

	// Decide GPU vs. software rendering BEFORE CEF spins up its GPU subprocess. On
	// some machines (e.g. a GPU newer than this libcef) the CEF GPU process
	// CHECK()-crashes in a loop and the renderer never composites, so the UI paints as
	// a blank window. A prior boot that created the browser but never confirmed a
	// paint trips an automatic, persistent fallback to software (SwiftShader)
	// rendering here; App reads this in OnBeforeCommandLineProcessing.
	const GpuSafeMode::BootDecision gpuDecision = GpuSafeMode::DecideAtBoot();
	app->set_software_mode(gpuDecision.disableGpu);
	if (gpuDecision.autoFellBack) {
		blog(LOG_WARNING,
		     "[gpu] a previous launch never confirmed a GPU paint -- rendering in software mode "
		     "(SwiftShader). Delete \"%s\" to re-probe the GPU.",
		     gpuDecision.safeModeFile.c_str());
	} else if (gpuDecision.disableGpu) {
		HostLog("[gpu] software rendering forced (disable-gpu marker or persisted safe mode)");
	}

	CefSettings settings;
	settings.no_sandbox = true;
	settings.multi_threaded_message_loop = false; // we drive CefRunMessageLoop()
	settings.windowless_rendering_enabled = true; // required for obs-browser OSR sources

	// Give CEF an explicit cache root under our config base. Without it CEF warns and
	// derives a process-singleton lock from a default AppData\Local\CEF path that can
	// collide across installs; scoping it here silences the warning and pins the
	// singleton to this install. cache_path is left empty (unchanged in-memory profile
	// behavior); only installation data lands on disk under this root.
	const std::string cefCache = BraidcastConfigPath("cef_cache");
	if (!cefCache.empty()) {
		os_mkdirs(cefCache.c_str());
		CefString(&settings.root_cache_path).FromString(cefCache);
	}

	// Pin CEF's own debug.log to a known, findable path under the config base
	// instead of the default (cwd/"debug.log" in the rundir). This is where the CEF
	// GPU-process crash signature lands -- "GPU process exited unexpectedly",
	// GpuControl.CreateCommandBuffer failures, "tile memory limits exceeded" -- with
	// millisecond timestamps to correlate against the "[gpudiag]" sampler timeline.
	// Severity is left at the default (INFO): the crash lines already emit at
	// WARNING/ERROR, and raising to VERBOSE would flood the log and perturb timing.
	const std::string cefLog = BraidcastConfigPath("cef_debug.log");
	if (!cefLog.empty()) {
		CefString(&settings.log_file).FromString(cefLog);
	}

	if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
		// CEF is not up, so Teardown() skips its CEF steps (g_cefInitialized still
		// false); it still shuts RTWQ down and releases the mutex acquired above.
		const int code = CefGetExitCode();
		Teardown();
		return code;
	}
	g_cefInitialized = true;
	HostLog("[cef] initialized");

	HWND host = CreateHostWindow(hInstance);
	if (!host) {
		HostLog("[host] CreateHostWindow failed -- aborting");
		Teardown();
		return 1;
	}
	// Remove the stock caption + enable the frameless chrome before the window is
	// shown, so no framed frame flashes on first paint.
	WindowChrome::Init(host, WindowChrome::kMainWindow);
	// Force the residual Win11 border + system menu dark so no light frame flashes.
	NativeTheme::ApplyDark(host);
	// Restore the last-session placement before the first show (the window has no
	// WS_VISIBLE yet, so SetWindowPlacement's maximized state survives to SW_SHOW).
	// Skipped under the headless smoke path so those runs stay deterministic.
	if (!Env::IsSet("FE_SMOKE_QUIT_SECONDS")) {
		RestoreHostPlacement(host);
	}
	// #3489 guard: when the restored placement lands on a non-primary monitor whose
	// DPI differs from the primary, Chromium can lay the browser out once at the
	// primary DPI and again after the OS settles the real DPI (double-scale). Force a
	// frame-changed pass now that the host sits on its target monitor so the first
	// visible layout is already at the correct scale; any late WM_DPICHANGED is then
	// corrected by WindowDpi::HandleDpiChanged.
	SetWindowPos(host, nullptr, 0, 0, 0, 0,
		     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	ShowWindow(host, SW_SHOW);
	UpdateWindow(host);

	if (!ObsBootstrap::Start()) {
		HostLog("[obs] ObsBootstrap::Start() failed -- aborting");
		Teardown();
		return 1;
	}
	g_obsStarted = true;

	// GPU-crash diagnostics (both env-gated, default off). Arm the browser-source
	// A/B kill switch first (it sweeps the sources the initial scene collection just
	// loaded inside Start()), then start the periodic "[gpudiag]" sampler. Both are
	// torn down in Teardown() before ObsBootstrap::Stop().
	GpuDiag::InstallBrowserSourceKillSwitch();
	GpuDiag::Start();

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
	const long smokeQuitSecs = Env::Number("FE_SMOKE_QUIT_SECONDS", 0);
	if (smokeQuitSecs > 0) {
		HostLog("[host] smoke-quit armed: " + std::to_string(smokeQuitSecs) + "s");
		SetTimer(host, kSmokeQuitTimerId, UINT(smokeQuitSecs) * 1000, nullptr);
	}

	// Env-gated automated perf-repro self-test: the regression gate for the
	// background power-throttling opt-out above (the SetProcessInformation/
	// ProcessPowerThrottling call). Network-free and deterministic -- it verifies
	// the opt-out is in force, minimizes the host window to simulate losing the
	// foreground, samples render pacing via stats.get for a fixed window, then
	// quits with a PASS/FAIL exit code. Inert without
	// BRAIDCAST_SELFTEST_STREAM=perf-repro.
	bool perfReproArmed = false;
	if (Env::Value("BRAIDCAST_SELFTEST_STREAM") == "perf-repro") {
		HostLog("[host] perf-repro selftest armed");
		ObsBootstrap::ArmPerfReproSelfTest(host);
		SetTimer(host, kPerfReproTimerId, 500, nullptr);
		perfReproArmed = true;
	}

	// ONE Client for the whole process, published via Client::SetShared so future
	// additional browsers (e.g. detached windows) reuse it: all browsers land in the
	// same browser_list_ + Bridge emit registry and the loop quits only when the
	// LAST browser closes.
	CefRefPtr<Client> client(new Client());
	Client::SetShared(client);
	CefBrowserSettings browser_settings;
	browser_settings.background_color = kAppBackgroundColor;
	CefWindowInfo window_info;
	RECT host_rc;
	GetClientRect(host, &host_rc);
	window_info.SetAsChild(host, CefRect(0, 0, host_rc.right - host_rc.left, host_rc.bottom - host_rc.top));

	g_browser = CefBrowserHost::CreateBrowserSync(window_info, client, app->startup_url(), browser_settings,
						      nullptr, nullptr);
	if (!g_browser) {
		// No browser means OnBeforeClose never fires to quit the loop; run the
		// shared teardown rather than hang in CefRunMessageLoop with no way out.
		HostLog("[cef] CreateBrowserSync failed -- aborting");
		Teardown();
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

	Teardown();

	// Propagate the perf-repro self-test's PASS/FAIL/skip/error exit code (see
	// perf_repro_selftest.hpp) so it can gate CI; every other path keeps the
	// existing always-0 exit.
	return perfReproArmed ? ObsBootstrap::PerfReproSelfTestExitCode() : 0;
}
