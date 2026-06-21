#ifndef OBS_MULTISTREAM_FRONTEND_PREVIEW_WINDOW_HPP_
#define OBS_MULTISTREAM_FRONTEND_PREVIEW_WINDOW_HPP_

#include <windows.h>

#include <cstdint>

// Owns the native preview: a borderless child HWND of the host (sibling above the
// CEF browser HWND, z-ordered on top) plus an obs_display attached to it. The
// obs_display is its own D3D11 swapchain libobs steps independently of the CEF
// browser's renderer, rendering the active program scene (output channel 0) with
// aspect-correct letterboxing. Lives between ObsBootstrap::Start() and Stop().
//
// The overlay HWND is created lazily on the first SetRect so the UI drives its
// geometry. SetRect/Hide run on the host UI thread (the CEF message-router
// callback thread is the same thread here).
class PreviewWindow {
public:
	// host: the top-level host window the overlay is parented to. siblingAbove:
	// the CEF browser HWND the overlay must stay z-ordered above (may be null at
	// construction; the overlay is created above whatever is current at SetRect).
	PreviewWindow(HWND host, HINSTANCE instance);

	// Position/size the overlay (device pixels, host-client coords) and resize the
	// obs_display. Creates the overlay HWND + display on first call. Zero/negative
	// size hides instead.
	void SetRect(int x, int y, int cx, int cy);

	// Hide the overlay HWND (keeps it + the display alive for the next SetRect).
	void Hide();

	// Destroy the obs_display (must run while libobs is up) and the overlay HWND.
	void Destroy();

private:
	void EnsureCreated();

	HWND host_;
	HINSTANCE instance_;
	HWND hwnd_ = nullptr; // overlay child HWND; null until first SetRect
};

// Process-wide accessor to the single live PreviewWindow so the bridge methods
// (preview.setRect / preview.hide) can reach it without threading a pointer
// through the dispatch registry. Set by main.cpp after the display is ready and
// cleared before teardown; both bridge and main run on the same UI thread.
namespace Preview {
void SetInstance(PreviewWindow *pw);
PreviewWindow *Instance();
} // namespace Preview

#endif // OBS_MULTISTREAM_FRONTEND_PREVIEW_WINDOW_HPP_
