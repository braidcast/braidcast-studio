#include "tray.hpp"

#include <shellapi.h>

#include <string>

#include "settings/GeneralSettings.hpp"
#include "app_icon.hpp"
#include "log.hpp"
#include "multistream/MultistreamEngine.hpp"
#include "multistream/VirtualCamManager.hpp"
#include "obs_bootstrap.hpp"

namespace {

// Tray context-menu command ids. Non-zero (TrackPopupMenu returns 0 when the
// menu is dismissed without a selection). One enum value per menu action; the
// dispatch switch in ShowMenu mirrors this list.
enum TrayCommand : UINT {
	CmdShowHide = 1,
	CmdStartAll,
	CmdStopAll,
	CmdVirtualCam,
	CmdExit,
};

constexpr wchar_t kTooltip[] = L"Braidcast";

} // namespace

bool TrayIcon::Create(HWND host, HINSTANCE instance)
{
	host_ = host;
	instance_ = instance;
	if (added_) {
		return true;
	}

	nid_ = {};
	nid_.cbSize = sizeof(nid_);
	nid_.hWnd = host_;
	nid_.uID = 1;
	nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid_.uCallbackMessage = kTrayCallbackMsg;
	// Reuse the embedded app icon (small/caption size). LoadImageW returns a
	// shared resource handle -- not owned per-instance, so it is never
	// DestroyIcon'd (matches app_icon.hpp).
	nid_.hIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(kAppIconResId), IMAGE_ICON,
						   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
						   LR_DEFAULTCOLOR));
	wcsncpy_s(nid_.szTip, kTooltip, _TRUNCATE);

	if (!Shell_NotifyIconW(NIM_ADD, &nid_)) {
		HostLog("[tray] Shell_NotifyIcon NIM_ADD failed");
		return false;
	}
	added_ = true;
	return true;
}

void TrayIcon::Remove()
{
	if (!added_) {
		return;
	}
	Shell_NotifyIconW(NIM_DELETE, &nid_);
	added_ = false;
}

void TrayIcon::ShowHost()
{
	if (!host_) {
		return;
	}
	ShowWindow(host_, SW_SHOW);
	SetForegroundWindow(host_);
	// In lazy mode the icon exists only while minimized; drop it on restore.
	if (!ObsBootstrap::General().alwaysShowTray) {
		Remove();
	}
}

void TrayIcon::HideHost()
{
	if (!host_) {
		return;
	}
	// Ensure the icon is present so the window can be restored from the tray.
	Create(host_, instance_);
	ShowWindow(host_, SW_HIDE);
}

void TrayIcon::ShowMenu()
{
	if (!host_) {
		return;
	}

	const bool visible = IsWindowVisible(host_) != FALSE;
	const bool vcamActive = ObsBootstrap::VirtualCam().IsActive();

	HMENU menu = CreatePopupMenu();
	if (!menu) {
		return;
	}
	AppendMenuW(menu, MF_STRING, CmdShowHide, visible ? L"Hide" : L"Show");
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, CmdStartAll, L"Start All");
	AppendMenuW(menu, MF_STRING, CmdStopAll, L"Stop All");
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, CmdVirtualCam, vcamActive ? L"Stop Virtual Camera" : L"Start Virtual Camera");
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, CmdExit, L"Exit");

	POINT pt;
	GetCursorPos(&pt);
	// Required so the menu dismisses when the user clicks elsewhere (Win32
	// tray-menu quirk: the owning window must be foreground first).
	SetForegroundWindow(host_);
	const UINT cmd = static_cast<UINT>(
		TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, host_, nullptr));
	DestroyMenu(menu);

	switch (cmd) {
	case CmdShowHide:
		if (visible) {
			HideHost();
		} else {
			ShowHost();
		}
		break;
	case CmdStartAll:
		ObsBootstrap::Multistream().StartAllEnabled();
		break;
	case CmdStopAll:
		ObsBootstrap::Multistream().StopAll();
		break;
	case CmdVirtualCam:
		if (vcamActive) {
			ObsBootstrap::VirtualCam().Stop();
		} else {
			std::string err;
			if (!ObsBootstrap::VirtualCam().Start(err)) {
				HostLog("[tray] virtual camera start failed: " + err);
			}
		}
		break;
	case CmdExit:
		PostMessageW(host_, WM_CLOSE, 0, 0);
		break;
	default:
		break;
	}
}

void TrayIcon::OnCallback(WPARAM /*wparam*/, LPARAM lparam)
{
	switch (LOWORD(lparam)) {
	case WM_LBUTTONDBLCLK:
		// Double-click toggles the window between shown and hidden-to-tray.
		if (IsWindowVisible(host_)) {
			HideHost();
		} else {
			ShowHost();
		}
		break;
	case WM_RBUTTONUP:
	case WM_CONTEXTMENU:
		ShowMenu();
		break;
	default:
		break;
	}
}
