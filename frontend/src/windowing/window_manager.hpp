#ifndef OBS_MULTISTREAM_FRONTEND_WINDOW_MANAGER_HPP_
#define OBS_MULTISTREAM_FRONTEND_WINDOW_MANAGER_HPP_

#include <windows.h>

#include <string>
#include <vector>

#include "include/cef_browser.h"

// Creates + tracks detached top-level host windows, each owning its own child
// CEF browser pointed at the app bundle with a ?window=<id>&dock=<dockId>
// query. Single-threaded: all calls on the CEF UI thread. Surfaces for a window
// are torn down (PreviewManager::DestroyWindow) before the window's browser
// closes and before any canvas mix is freed.
//
// Every child browser is created with the SHARED process-wide Client
// (Client::Shared), NOT a fresh Client: all browsers must land in one
// browser_list_ + Bridge emit registry so the message loop quits only when the
// LAST browser across all windows closes.
//
// Lifetime of a detached window (all on the CEF UI thread):
// - Detach creates the host and shows it at once (its class brush is the page's base
//   color), creates its browser, closes the host's preview gate
//   (OverlaySurface::SetHostReady(host, false)), registers the host with PreviewManager,
//   and arms the paint watchdog. The host HWND owns that timer (kPaintWatchdogTimerId); only Detach
//   and OnRendererTerminated arm it.
// - The gate opens only on window.ready, which the page sends once animation frames
//   have actually run after its dock mounted -- evidence that Chromium is producing
//   frames for it. Opening lets the preview surfaces held behind the gate take their
//   rects. Nothing else opens it: a page that never paints keeps its surfaces held, so
//   the window shows as a blank page, never as a preview with no UI around it.
// - The watchdog only reports: when it fires before window.ready it logs, once, that the
//   page has not painted and its surfaces stay held. It is killed when the gate opens,
//   and on redock, WM_CLOSE and app shutdown (ReleaseWindow). A renderer crash closes the
//   gate again and re-arms it for the reloaded page.
// - window.ready for an id that is not in windows_ (0, never issued, or erased by a
//   redock or close) is a no-op, and so are window.ready and the watchdog once CloseAll
//   has begun.
// - Teardown is ReleaseWindow then DestroyWindow: surfaces (PreviewManager::DestroyWindow,
//   before any canvas mix can be freed) -> host unregistered and its gate forgotten
//   without replaying anything held -> browser (CloseBrowser) -> the windows_ entry erased
//   -> the host HWND destroyed.
class WindowManager {
public:
	WindowManager(HINSTANCE instance, const std::string &bundleBaseUrl);
	~WindowManager();

	WindowManager(const WindowManager &) = delete;
	WindowManager &operator=(const WindowManager &) = delete;

	// Create a detached window owning `dockId`; returns its windowId (>0), or 0 on
	// failure.
	int Detach(const std::string &dockId);
	// Destroy the detached window for `windowId` (tears down its surfaces first).
	bool Redock(int windowId);
	// {windowId, dockId} for every live detached window.
	struct WindowInfo {
		int windowId;
		std::string dockId;
	};
	std::vector<WindowInfo> List() const;

	// The top-level HWND backing `windowId`, or null if unknown. Used by the window
	// control bridge (minimize/maximize/close) to act on a detached window.
	HWND HwndFor(int windowId) const;

	// The detached page's animation frames have run since its dock mounted
	// (window.ready): open the window's gate. Returns false, doing nothing, for an id
	// that is not a live detached window.
	bool MarkReady(int windowId);

	// The renderer behind `browser` terminated. For a detached window's browser: drop its
	// preview surfaces, close its gate and re-arm the paint watchdog, so nothing draws
	// over the page until the reload reports ready. No-op for any other browser.
	void OnRendererTerminated(CefRefPtr<CefBrowser> browser);

	// Whether `windowId` is a live detached window whose gate is open. For self-tests.
	bool IsReadyForTest(int windowId) const;

	// Tear down every detached window's preview surfaces + browser. Called at
	// global teardown BEFORE ObsBootstrap::Stop() frees canvas mixes.
	void DestroyAll();

	// Close every detached window (drives each browser's OnBeforeClose). Called
	// from the MAIN window's WM_CLOSE so the shared browser_list_ can drain to
	// empty -- otherwise a live detached browser keeps CefRunMessageLoop alive
	// after the main window closes (the N-browsers quit guard, working as intended).
	void CloseAll();

	static WindowManager *Instance();
	static void SetInstance(WindowManager *wm);

	// The detached-window WndProc. Public so the window class registration (in the
	// .cpp) can reference it; not called directly otherwise.
	static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

private:
	struct Window {
		int windowId;
		std::string dockId;
		HWND hwnd;
		CefRefPtr<CefBrowser> browser;
		// The gate is open: the page reported window.ready.
		bool ready = false;
	};
	Window *FindByHwnd(HWND hwnd);
	Window *FindById(int windowId);
	const Window *FindById(int windowId) const { return const_cast<WindowManager *>(this)->FindById(windowId); }

	// Open w's gate: kill the paint watchdog, let its held preview surfaces take their
	// rects. Idempotent.
	void OpenGate(Window &w);
	// The teardown every removal path shares, up to and excluding the erase and the
	// DestroyWindow: paint watchdog, surfaces, host registration and gate, browser.
	static void ReleaseWindow(const Window &w);

	HINSTANCE instance_;
	std::string bundleBaseUrl_; // e.g. "app://app/index.html"
	int nextId_ = 1;            // windowId 0 is reserved for the main window
	std::vector<Window> windows_;
	bool closingAll_ = false; // app-shutdown: suppress per-window window.closed redock broadcasts
};

#endif // OBS_MULTISTREAM_FRONTEND_WINDOW_MANAGER_HPP_
