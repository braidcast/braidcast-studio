#pragma once

#include <cstdint>
#include <string>

// Automatic, persistent fallback to software (SwiftShader) rendering when CEF's
// GPU subprocess cannot give the UI hardware. On some machines the GPU process
// CHECK()-fails with EXCEPTION_BREAKPOINT; Chromium restarts it, and after three
// crashes in one mode it falls back on its own (HARDWARE_GL -> SWIFTSHADER ->
// DISPLAY_COMPOSITOR, then a fatal "GPU process isn't usable" that takes the whole
// app down). The UI then paints, but in software, after a second of crashes, and
// with that fatal tail. Launching with --disable-gpu reaches the same software mode
// without any of it.
//
// So a paint is not proof of hardware; the main page's WebGL renderer report is.
// An explicit software report (or no WebGL at all) persists safe mode at once.
// Two per-boot sentinels, armed once the main browser exists, cover the runs that
// never report:
//   - the paint sentinel, cleared by the main frame's first load: the UI came up;
//   - the renderer sentinel, cleared by any renderer report.
// A sentinel surviving to the next boot is one strike, and kStrikesToPersist
// consecutive strikes persist safe mode; a hardware report clears the count. Runs
// that say nothing about the GPU leave no sentinel behind: a main-frame load error
// clears both, and a deliberate exit clears the renderer sentinel. A deliberate exit
// keeps the paint sentinel, because a UI that never came up and was closed is the
// blank-window failure this exists for.
//
// The persisted marker records the build and the CEF version it was written under;
// any other build or CEF drops it and probes the GPU afresh, so every update retries
// hardware by itself.
// That probe follows the usual rules: a software report re-persists at once, a
// sentinel-only failure after the usual strikes. Deleting the marker always yields a
// probe boot.
namespace GpuSafeMode {

inline constexpr int kStrikesToPersist = 2;

enum class Renderer { Hardware, Software };

// Why this boot runs the mode it does. Logged; also the self-test's oracle.
enum class BootReason {
	Hardware,                 // probes armed, GPU requested
	NoConfigBase,             // GPU requested, but no writable dir to probe in
	ManualOverride,           // braidcast_disable_gpu.txt beside the exe
	PersistedSafeMode,        // marker written under this same build and CEF version
	ReprobeAfterBuildChange,  // marker from another build or CEF version: GPU requested once more
	NoPaintLastBoot,          // the paint sentinel survived (a strike)
	NoRendererReportLastBoot, // the renderer sentinel survived (a strike)
	SoftwareRendererReported  // marker only: the page reported a software renderer
};

// Everything the boot decision reads, so the decision itself stays a pure function.
struct BootInputs {
	bool disableMarker = false;
	bool haveConfigBase = true;
	bool safeModeMarker = false;
	// The `cef=` line of the marker; empty for a marker written before it carried one.
	std::string markerCefVersion;
	std::string runningCefVersion;
	// The `build=` line of the marker; empty for a marker written before it carried one.
	std::string markerBuild;
	std::string runningBuild;
	bool paintSentinel = false;
	bool rendererSentinel = false;
	// Consecutive strikes recorded by earlier boots.
	int strikes = 0;
};

// What the boot does with the probe files, as decided by PlanBoot.
struct BootPlan {
	bool disableGpu = false;
	BootReason reason = BootReason::Hardware;
	bool writeMarker = false;  // persist safe mode under the running build and CEF version
	bool removeMarker = false; // a stale marker being re-probed
	bool clearProbes = false;  // delete leftover sentinels
	bool armProbes = false;    // arm both sentinels once the main browser exists
	int strikes = 0;           // strike count to keep on disk (0 removes it)
};

BootPlan PlanBoot(const BootInputs &in);

// What the main page's renderer report does, as decided by PlanReport.
struct ReportPlan {
	bool persistSoftware = false;       // write the marker now
	bool clearRendererSentinel = false; // the report arrived, so the sentinel is answered
	bool clearStrikes = false;
	const char *decision = "";
};

ReportPlan PlanReport(bool softwareRequested, BootReason bootReason, Renderer renderer);

// The two ways a run ends without saying anything about the GPU.
enum class ProbeEvent { MainFrameLoadError, DeliberateExit };

struct ProbeClose {
	bool removePaintSentinel = false;
	bool removeRendererSentinel = false;
};

// Which still-armed sentinels `event` retires. `reported` is whether a renderer
// report already answered this launch.
ProbeClose PlanClose(ProbeEvent event, bool reported);

// Classify a WebGL UNMASKED_RENDERER_WEBGL string. Empty means the page got no
// renderer string -- no WebGL context, or one that would not name its renderer --
// which counts as software: hardware has to be shown.
Renderer ClassifyRenderer(const std::string &renderer);

// The renderer string as it may appear in a log: control characters dropped, the
// "(0x0000xxxx)" device ids ANGLE embeds removed, length capped.
std::string SanitizeRenderer(const std::string &renderer);

// Marker body: one `key=value` per line. Only `cef=` and `build=` are read back.
std::string FormatMarker(const std::string &cefVersion, const std::string &build, BootReason reason);
std::string MarkerCefVersion(const std::string &markerBody);
std::string MarkerBuild(const std::string &markerBody);

// Strike file body (`strikes=N`); a missing or malformed body reads as 0.
std::string FormatStrikes(int strikes);
int ParseStrikes(const std::string &body);

const char *ReasonName(BootReason reason);

struct BootDecision {
	// Run this boot with the GPU disabled (software rendering).
	bool disableGpu = false;
	BootReason reason = BootReason::Hardware;
	int strikes = 0;
	// Absolute path of the persisted safe-mode marker; empty without a config base.
	std::string safeModeFile;
};

// Resolve the rendering mode for this launch. MUST be called on the main thread
// before CefInitialize: it reads the marker, sentinels and strikes and applies the
// plan, except arming, which waits for ArmProbes().
BootDecision DecideAtBoot();

// Arm the sentinels for a GPU boot, right after the main browser is created and
// before the message loop runs -- so an OBS-startup failure earlier in the boot is
// never read as a GPU verdict. No-op on a software boot. `mainBrowserId` is what
// NoteMainFrameLoadError matches against.
void ArmProbes(int mainBrowserId);

// Log the boot decision. Separate from DecideAtBoot because that runs before the
// session-log handler exists, where a blog() never reaches the file; call once
// that handler is installed.
void LogBootDecision();

// Clear the paint sentinel: the main frame finished loading, so the UI came up.
// It is NOT proof of hardware -- NoteRendererReport is. UI-thread only and
// idempotent.
void NotePaintSuccess();

// The main page's WebGL renderer string, reported once after its first paint (empty
// when it got none). Only the first report of a launch counts. UI-thread only.
void NoteRendererReport(const std::string &renderer);

// A main-frame load failed (not ERR_ABORTED) in `browserId`: the app never ran, so
// this launch carries no verdict. Ignored for any browser but the main one.
// UI-thread only.
void NoteMainFrameLoadError(int browserId);

// A deliberate exit (Teardown). Retires the renderer sentinel if no report came.
void NoteDeliberateExit();

// The disableGpu answer DecideAtBoot() returned this launch, for the subsystems
// that must not offer the GPU once the browser process runs with --disable-gpu.
// False until DecideAtBoot() has run.
bool SoftwareRendering();

} // namespace GpuSafeMode

// Crash-latched guard for obs-browser's hardware acceleration -- the GPU
// shared-texture path browser SOURCES render through. A machine whose CEF
// survives the main UI can still CHECK()-fail on CrBrowserMain once several
// sources bring that path up, which freezes the whole UI while libobs keeps
// running headless.
//
// This is a second probe rather than a reuse of GpuSafeMode above because the two
// windows do not overlap. GpuSafeMode's paint sentinel is cleared by the main frame's
// first paint, and no browser source has created its CEF browser by then --
// BrowserSource::CreateBrowser runs off the video tick, after the message loop is
// up -- so that window is already closed when this risk starts. Only the
// sentinel-file primitives are shared.
//
// The probe is armed at bootstrap when acceleration is actually applied, and
// closed by the first of: a task the CEF UI thread runs kProbeWindowMs after
// startup, the teardown every deliberate exit routes through, or bcrash()'s sink.
// The implication runs one way only. Reaching any of those proves CrBrowserMain
// was alive, so a run that reaches one cannot have hit the freeze; a sentinel
// surviving to the next boot does NOT prove the converse, because a power cut, an
// OS shutdown or a kill of a healthy-but-slow launch leaves the same trace. That
// residual surface is the accepted cost: it is one boot without acceleration and a
// checkbox to re-tick, against a crash loop the user can only escape by editing
// JSON. The two largest false-positive sources are excluded outright -- unattended
// runs never arm (they are designed to be killed), and a libobs fatal error closes
// the probe so an encoder or plugin fault cannot misattribute itself here.
namespace BrowserHwAccel {

// The libobs private-data key obs-browser reads its hardware-acceleration switch from.
inline constexpr char kPrivateDataKey[] = "BrowserHWAccel";

struct BootDecision {
	// Effective value to publish to obs-browser under kPrivateDataKey.
	bool enable = false;
	// This boot consumed a surviving sentinel. The caller turns the stored setting
	// off: that setting IS the latch, so the Settings checkbox stays an honest
	// picture of what is running and re-ticking it re-arms the probe.
	bool crashDetected = false;
};

// Resolve hardware acceleration for this launch. The two inputs stay separate on
// purpose: `enabledInSettings` is the user's own stored choice and is the only
// thing that may discard a pending verdict, while `gpuUnavailable` is a
// this-launch override (software rendering) that must leave a pending verdict
// standing -- dropping the GPU-disable marker beside the exe is the documented
// escape hatch for the very freeze this probe exists to catch, so a launch under
// it must not disarm the latch on the way past. Main thread, before obs-browser's
// module load reads the private-data bag. Arms the probe when the answer is on.
BootDecision DecideAtBoot(bool enabledInSettings, bool gpuUnavailable);

// Close the probe window: the CEF UI thread was alive to run this. UI-thread only,
// idempotent, and a no-op unless THIS process armed the probe -- an early-abort
// launch (single-instance rejection, a failed CefInitialize) must not delete the
// verdict a previous run left behind.
void NoteSurvived();

// Close the probe from bcrash()'s sink -- which libobs' own unhandled-exception
// filter routes every access violation and stack overflow through, not just
// explicit fatal errors. None of those is a browser-GPU freeze, and that process
// never reaches teardown (the sink _exit()s), so without this any hard fault would
// latch acceleration off. Callable from the faulting thread: it neither logs (the
// session-log handler's mutex may be held by that very thread) nor allocates (see
// the sentinel-path note in the .cpp).
void NoteFatalError();

// Retire a consumed crash verdict, once the caller has DURABLY stored the
// suppression. DecideAtBoot deliberately leaves the sentinel on disk when it
// reports crashDetected: the stored setting is the latch, so a failed save would
// otherwise spend the verdict and leave nothing behind, and the next launch would
// re-enable acceleration straight back into the freeze. Call only after the save
// succeeded; skip it and the next boot simply retries the suppression. No-op
// unless this boot actually consumed a verdict.
void ConfirmSuppressionPersisted();

// How long the probe stays open after startup. Deliberately generous rather than
// tight, because the two errors are not symmetric: closing too early means a slow
// bring-up freezes with no sentinel and the user is left with an app that hangs
// every launch and no way out but hand-editing JSON, while closing too late costs
// one boot without acceleration and a checkbox to re-tick. Since teardown also
// closes the probe, only an UNGRACEFUL kill inside the window misfires -- a user
// who simply quits early does not. So this is sized to cover the initial scene
// collection bringing its browser sources up on the shared-texture path with a
// cold cache, many sources and a slow disk, not to be the shortest window that
// usually works.
//
// It bounds boot-time bring-up only. A browser source added, or first shown,
// minutes into a session is outside this window and outside any window a fixed
// delay could offer; catching that needs a per-source readiness signal obs-browser
// does not expose.
inline constexpr int64_t kProbeWindowMs = 120000;

} // namespace BrowserHwAccel
