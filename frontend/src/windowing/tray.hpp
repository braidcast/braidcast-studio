#pragma once

#include <windows.h>

#include <shellapi.h>

// Windows notification-area (system tray) icon for the host shell. Owns one
// Shell_NotifyIcon entry whose context menu drives Show/Hide, Start All / Stop
// All, the Virtual Camera toggle, and Exit. Single-threaded: every method runs
// on the host window's UI thread (the WndProc), so no synchronization is needed.
//
// Lifecycle policy (see main.cpp): the icon is created at startup only when
// General().alwaysShowTray is set; otherwise it is added lazily by HideHost()
// when the window minimizes to the tray and removed again by ShowHost() once the
// window is restored (unless alwaysShowTray keeps it pinned). NIM_DELETE must run
// before the host window is destroyed or a ghost icon lingers in the tray.
class TrayIcon {
public:
	// Private window message the shell posts to the host WndProc for tray
	// interaction (mouse events on the icon). Routed by HostWndProc to
	// OnCallback. WM_APP-based so it never collides with system messages.
	static constexpr UINT kTrayCallbackMsg = WM_APP + 17;

	TrayIcon() = default;
	~TrayIcon() = default;

	TrayIcon(const TrayIcon &) = delete;
	TrayIcon &operator=(const TrayIcon &) = delete;

	// Build the NOTIFYICONDATAW and add the icon (NIM_ADD). Stores host +
	// instance for later show/hide. Idempotent: a second call while already
	// added is a no-op.
	bool Create(HWND host, HINSTANCE instance);

	// Remove the icon (NIM_DELETE). Idempotent; safe to call in teardown.
	void Remove();

	// Restore the host window from the tray: SW_SHOW + foreground. If
	// alwaysShowTray is off, also removes the tray icon (it only exists while
	// minimized in that mode).
	void ShowHost();

	// Hide the host window to the tray: ensure the icon is present (lazily adds
	// it when alwaysShowTray is off), then SW_HIDE.
	void HideHost();

	// Build + show the context menu at the cursor and dispatch the chosen
	// command. Call from the right-click / context-menu tray callback.
	void ShowMenu();

	// Tray message handler: left double-click toggles the window, right-button
	// up (or WM_CONTEXTMENU) opens the menu.
	void OnCallback(WPARAM wparam, LPARAM lparam);

private:
	HWND host_ = nullptr;
	HINSTANCE instance_ = nullptr;
	NOTIFYICONDATAW nid_ = {};
	bool added_ = false;
};
