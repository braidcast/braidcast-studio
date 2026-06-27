#ifndef OBS_MULTISTREAM_FRONTEND_INTERACT_WINDOW_HPP_
#define OBS_MULTISTREAM_FRONTEND_INTERACT_WINDOW_HPP_

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

struct obs_source;
typedef struct obs_source obs_source_t;

// A native source-interaction window: a standalone resizable top-level Win32
// window whose entire client area is an obs_display rendering ONE interactive
// source (full client, aspect-letterboxed) and forwarding mouse / keyboard /
// focus into it via obs_source_send_*. It is the input-forwarding cousin of
// ProjectorWindow -- same render/letterbox/lifecycle skeleton, plus a WndProc
// that translates Win32 input into obs_interaction events.
//
// Only sources whose output flags carry OBS_SOURCE_INTERACTION belong here; the
// bridge gates that before Open is called.
//
// Threading: WndProc + Open/Close/DestroyAll + draw-callback registration run on
// the UI thread; all obs_source_send_* therefore run on the UI thread. The draw
// callback runs on the libobs render thread but reads only the addref-pinned
// source, which outlives the display.
//
// UAF discipline: the source is addref'd for the window's whole lifetime and
// released only AFTER obs_display_destroy (in Destroy, last).

class InteractWindow {
public:
	// `source` is the addref'd source the window pins alive (released on
	// teardown). The caller (the manager) has already resolved + validated it.
	InteractWindow(int interactId, obs_source_t *source);
	~InteractWindow();

	InteractWindow(const InteractWindow &) = delete;
	InteractWindow &operator=(const InteractWindow &) = delete;

	// Create the top-level window + obs_display covering its client area. instance
	// is the module HINSTANCE. Returns false on failure (window/display creation).
	// UI thread.
	bool Create(HINSTANCE instance);

	// Destroy the obs_display (remove draw callback, obs_display_destroy) then the
	// window, then release the pinned source. Ordered so the display dies before
	// its source could be freed. Idempotent. UI thread.
	void Destroy();

	// Resize the obs_display to (cx, cy) device px (WM_SIZE). No-op before the
	// display exists or for a zero size. UI thread.
	void ResizeDisplay(int cx, int cy);

	int Id() const { return interactId_; }
	HWND Hwnd() const { return hwnd_; }
	const std::string &SourceUuid() const { return sourceUuid_; }

	// Borrowed view of the addref-pinned source, for the WndProc input forwarding.
	// Valid until Destroy releases the ref (and nulls it). UI thread only.
	obs_source_t *PinnedSource() const;

	// Win32 mouse-capture tracking so a press-drag that leaves the client area
	// still delivers WM_MOUSEMOVE/WM_*BUTTONUP (raw Win32 has no implicit grab).
	// `buttonMask` is the MK_* bit for the button. UI thread only.
	void PressButton(unsigned buttonMask);   // SetCapture on the first button down
	void ReleaseButton(unsigned buttonMask); // ReleaseCapture when the last clears
	void ResetButtons();                     // capture stolen (WM_CAPTURECHANGED)

	// Whether the obs_display exists (non-null). Used by the smoke self-test.
	bool HasDisplay() const { return display_ != nullptr; }

	// Per-window state the draw callback + WndProc read (the addref-pinned
	// source). Defined in the .cpp; named here only so the .cpp helpers can name
	// it. Incomplete outside that TU.
	struct State;

private:
	int interactId_;
	std::string sourceUuid_; // for per-source dedup in the manager

	State *state_;
	obs_source_t *source_; // addref'd for the window's whole lifetime

	HWND hwnd_ = nullptr;
	void *display_ = nullptr;  // obs_display_t* (opaque here)
	unsigned buttonsDown_ = 0; // MK_* bits of currently pressed mouse buttons
};

// Owns the live interaction windows, each a top-level window. unique_ptr so every
// window keeps a stable address while the list grows (the WndProc maps an HWND ->
// InteractWindow via GWLP_USERDATA, which stores that address). The single live
// manager is reachable process-wide via Interact::Instance().
//
// Open/Close/DestroyAll run on the UI thread. Close() is re-entrancy-safe so a
// window can self-close from its own WndProc (Esc / close button).
class InteractManager {
public:
	explicit InteractManager(HINSTANCE instance);
	~InteractManager();

	InteractManager(const InteractManager &) = delete;
	InteractManager &operator=(const InteractManager &) = delete;

	// Open an interaction window for `source` (assumed validated as interactive by
	// the bridge). Dedups per source uuid: an existing window for the same source
	// is brought to the foreground and its id returned (no second window). Open
	// takes its OWN addref on the source; the caller still owns + releases its ref.
	// Returns the interactId (> 0) on success, 0 + fills error on failure.
	int Open(obs_source_t *source, std::string &error);

	// Close + destroy the window with `interactId`. Re-entrancy-safe: a missing id
	// is a no-op (false).
	bool Close(int interactId);

	// Tear down every interaction window (display before window). Called at
	// teardown before the source mixes are freed and before obs_shutdown.
	void DestroyAll();

	// Whether the window with `interactId` has a live obs_display. Used by the
	// smoke self-test. false for an unknown id.
	bool HasDisplayForTest(int interactId) const;

private:
	HINSTANCE instance_;
	int nextId_ = 1; // interactId is always > 0
	std::vector<std::unique_ptr<InteractWindow>> windows_;
};

// Process-wide accessor to the single live InteractManager so the bridge methods
// (sources.interact / sources.closeInteract) and a self-closing window's WndProc
// can reach it without threading a pointer through. Set by main.cpp after libobs
// is up and cleared before teardown; bridge + main + WndProc share the UI thread.
namespace Interact {
void SetInstance(InteractManager *im);
InteractManager *Instance();
} // namespace Interact

#endif // OBS_MULTISTREAM_FRONTEND_INTERACT_WINDOW_HPP_
