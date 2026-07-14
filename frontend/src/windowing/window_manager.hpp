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
	};
	Window *FindByHwnd(HWND hwnd);

	HINSTANCE instance_;
	std::string bundleBaseUrl_; // e.g. "app://app/index.html"
	int nextId_ = 1;            // windowId 0 is reserved for the main window
	std::vector<Window> windows_;
	bool closingAll_ = false; // app-shutdown: suppress per-window window.closed redock broadcasts
};

#endif // OBS_MULTISTREAM_FRONTEND_WINDOW_MANAGER_HPP_
