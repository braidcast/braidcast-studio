#ifndef OBS_MULTISTREAM_FRONTEND_PROJECTOR_WINDOW_HPP_
#define OBS_MULTISTREAM_FRONTEND_PROJECTOR_WINDOW_HPP_

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

struct obs_canvas;
typedef struct obs_canvas obs_canvas_t;
struct obs_source;
typedef struct obs_source obs_source_t;

// A native projector: a standalone top-level Win32 window whose entire client
// area is an obs_display rendering one chosen target (program / a scene / a
// source / an additional canvas) with aspect-correct letterboxing. It is the
// read-only cousin of PreviewSurface -- no selection, no drag, no CEF browser.
//
// Two modes: fullscreen (borderless WS_POPUP covering one monitor's rect) and
// windowed (a resizable WS_OVERLAPPEDWINDOW). Both close on Esc, the close
// button, and Alt+F4; closing routes through ProjectorManager so teardown is
// ordered (display destroyed before the window) and re-entrancy-safe.
//
// Threading: WndProc + Open/Close/DestroyAll + draw-callback registration run on
// the UI thread. The draw callback itself runs on the libobs render thread, but
// reads only pointers that are borrowed (the canvas mix, owned by CanvasRuntime)
// or addref-pinned (the scene/source), both of which outlive the display.
//
// UAF discipline: a projector's obs_display must never outlive its target mix. A
// scene/source target is addref'd for the projector's whole lifetime; a canvas
// target is borrowed and its display is destroyed (DestroyAll) before the canvas
// mixes are freed.

enum class ProjectorKind { Program, Scene, Source, Canvas, Multiview };

// Map ProjectorKind <-> its wire string over a single data table (adding a kind
// is one table row, not a new branch).
bool KindFromString(const std::string &name, ProjectorKind &out);
const char *KindToString(ProjectorKind kind);

enum class ProjectorMode { Fullscreen, Windowed };

// One monitor's geometry, as enumerated by EnumerateMonitors(). `index` is the
// 0-based enumeration order; x/y/width/height come from MONITORINFO.rcMonitor.
struct MonitorInfo {
	int index = 0;
	std::string name; // szDevice, UTF-8
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
	bool primary = false;
};

// Enumerate the system's monitors (EnumDisplayMonitors order). Shared by the
// bridge's display.listMonitors and the manager's fullscreen-rect lookup.
std::vector<MonitorInfo> EnumerateMonitors();

class ProjectorWindow {
public:
	// kind/name/canvasUuid describe the render target (name for scene/source,
	// canvasUuid for canvas; both empty for program). For a scene/source target,
	// `source` is the addref'd source the projector pins alive (released on
	// teardown). For a canvas target, `canvas` is the borrowed obs_canvas_t. The
	// caller (the manager) has already resolved + validated these.
	ProjectorWindow(int projectorId, ProjectorKind kind, const std::string &name, const std::string &canvasUuid,
			ProjectorMode mode, int monitorIndex, obs_source_t *source, obs_canvas_t *canvas);
	~ProjectorWindow();

	ProjectorWindow(const ProjectorWindow &) = delete;
	ProjectorWindow &operator=(const ProjectorWindow &) = delete;

	// Create the top-level window + obs_display covering its client area. instance
	// is the module HINSTANCE; monitorRect is the chosen monitor's rcMonitor (used
	// for fullscreen positioning and windowed centering). Returns false on failure
	// (window/display creation). UI thread.
	bool Create(HINSTANCE instance, const RECT &monitorRect);

	// Destroy the obs_display (remove draw callback, obs_display_destroy) then the
	// window, then release the pinned source. Ordered so the display dies before
	// its mix could be freed. Idempotent. UI thread.
	void Destroy();

	// Resize the obs_display to (cx, cy) device px (windowed-mode WM_SIZE). No-op
	// before the display exists or for a zero size. UI thread.
	void ResizeDisplay(int cx, int cy);

	// Re-resolve a Multiview projector's scene snapshot + labels (the per-cell
	// targets the render thread reads). UI thread only -- it mutates the State
	// snapshot under State::mvMutex, so a deleted scene can never reach the render
	// thread. Cheap when the scene set is unchanged (only the active-scene index
	// updates); rebuilds the addref'd scene + label vectors when scenes are
	// added/removed/renamed. A no-op for non-Multiview kinds. Driven by a WM_TIMER
	// while the window is open, plus an initial call at Create().
	void RefreshMultiviewSnapshot();

	int Id() const { return projectorId_; }
	ProjectorKind Kind() const { return kind_; }
	const std::string &Name() const { return name_; }
	const std::string &CanvasUuid() const { return canvasUuid_; }
	ProjectorMode Mode() const { return mode_; }
	int MonitorIndex() const { return monitorIndex_; }
	HWND Hwnd() const { return hwnd_; }

	// Whether the obs_display exists (non-null). Used by the smoke self-test to
	// prove the display was created.
	bool HasDisplay() const { return display_ != nullptr; }

	// Per-window state the draw callback reads (kind + the borrowed/pinned target
	// pointers). Defined in the .cpp; named here only so the .cpp helpers can name
	// it. Incomplete outside that TU.
	struct State;

private:
	int projectorId_;
	ProjectorKind kind_;
	std::string name_;       // scene/source name (empty otherwise)
	std::string canvasUuid_; // canvas uuid (empty otherwise)
	ProjectorMode mode_;
	int monitorIndex_; // -1 = none/primary

	State *state_;
	obs_source_t *source_; // addref'd for scene/source kinds; null otherwise
	obs_canvas_t *canvas_; // borrowed for canvas kind; null otherwise

	HWND hwnd_ = nullptr;
	void *display_ = nullptr; // obs_display_t* (opaque here)
	bool mvTimerSet_ = false;  // a Multiview refresh WM_TIMER is armed on hwnd_
};

// Owns the live projectors, each a top-level window. unique_ptr so every window
// keeps a stable address while the list grows (the WndProc maps an HWND ->
// ProjectorWindow via GWLP_USERDATA, which stores that address). The single live
// manager is reachable process-wide via Projector::Instance().
//
// Open/Close/List/DestroyAll run on the UI thread. DestroyAll runs at teardown,
// while libobs is up and BEFORE the canvas mixes are freed (a canvas projector's
// display renders a canvas mix). Close() is re-entrancy-safe so a window can
// self-close from its own WndProc (Esc / close button).
class ProjectorManager {
public:
	explicit ProjectorManager(HINSTANCE instance);
	~ProjectorManager();

	ProjectorManager(const ProjectorManager &) = delete;
	ProjectorManager &operator=(const ProjectorManager &) = delete;

	// Open a projector. Inputs are assumed validated by the bridge method; Open
	// still fails gracefully (returns 0 + fills error) if the window/display can't
	// be created or the monitor index is out of range. Returns the new projectorId
	// (> 0) on success. For scene/source, Open addref's the named source itself.
	int Open(ProjectorKind kind, const std::string &name, const std::string &canvasUuid, bool fullscreen,
		 int monitorIndex, std::string &error);

	// Close + destroy the projector with `projectorId`. Re-entrancy-safe: a
	// missing id is a no-op (false). Emits no event itself (the caller does).
	bool Close(int projectorId);

	// One projector's tracked metadata, for projector.list.
	struct ProjectorInfo {
		int projectorId;
		ProjectorKind kind;
		std::string name;
		std::string canvasUuid;
		ProjectorMode mode;
		int monitorIndex; // -1 when unset
	};
	std::vector<ProjectorInfo> List() const;

	// Tear down every projector (display before window). Called at teardown before
	// the canvas mixes are freed and before obs_shutdown.
	void DestroyAll();

	// Re-apply the always-on-top window style to every live projector (called when
	// the General setting changes so open projectors update without a reopen). UI
	// thread.
	void ApplyAlwaysOnTop(bool onTop);

	// Whether the projector with `projectorId` has a live obs_display. Used by the
	// smoke self-test. false for an unknown id.
	bool HasDisplayForTest(int projectorId) const;

private:
	HINSTANCE instance_;
	int nextId_ = 1; // projectorId is always > 0
	std::vector<std::unique_ptr<ProjectorWindow>> projectors_;
};

// Process-wide accessor to the single live ProjectorManager so the bridge methods
// (projector.open / projector.close / projector.list) and a self-closing window's
// WndProc can reach it without threading a pointer through. Set by main.cpp after
// libobs is up and cleared before teardown; bridge + main + WndProc share the UI
// thread.
namespace Projector {
void SetInstance(ProjectorManager *pm);
ProjectorManager *Instance();
} // namespace Projector

#endif // OBS_MULTISTREAM_FRONTEND_PROJECTOR_WINDOW_HPP_
