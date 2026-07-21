#include "window_dpi.hpp"

namespace WindowDpi {

void HandleDpiChanged(HWND hwnd, WPARAM /*wparam*/, LPARAM lparam)
{
	const RECT *suggested = reinterpret_cast<const RECT *>(lparam);
	if (!hwnd || !suggested) {
		return;
	}
	// The OS-suggested rect already accounts for the new DPI; adopting it verbatim
	// (and letting the resulting WM_SIZE re-layout children) is the documented
	// Per-Monitor-V2 response to a DPI change.
	SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
		     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
}

} // namespace WindowDpi
