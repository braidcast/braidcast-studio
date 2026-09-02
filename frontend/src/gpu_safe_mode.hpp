#pragma once

#include <cstdint>
#include <string>

// Automatic, persistent fallback to software (SwiftShader) rendering when CEF's
// GPU subprocess crash-loops. On some machines (notably a GPU newer than this
// libcef) the GPU process CHECK()-fails on launch with EXCEPTION_BREAKPOINT,
// Chromium restarts it, it crashes again, and the renderer never composites -- so
// the UI paints as a blank window. Launching with --disable-gpu avoids the crash
// entirely (the app ships vk_swiftshader.dll for software compositing).
//
// The decision is driven by a per-boot sentinel file: it is written before the
// GPU-backed browser is created and deleted once the renderer confirms a paint. If
// it survives to the next boot, the previous run started the browser but never
// composited (the crash signature), so that boot falls back to software rendering
// and persists the choice.
namespace GpuSafeMode {

struct BootDecision {
	// Run this boot with the GPU disabled (software rendering).
	bool disableGpu = false;
	// This boot fell back automatically after detecting a prior GPU crash; false for
	// manual/persisted software mode. Its only consumer is the LOG_WARNING in
	// wWinMain -- there is no boot-notice channel to the UI, so the fallback is
	// invisible to anyone not reading the log.
	bool autoFellBack = false;
	// Absolute path of the persisted safe-mode flag, for the fallback notice.
	std::string safeModeFile;
};

// Resolve the rendering mode for this launch. MUST be called on the main thread
// before CefInitialize (it reads/writes the sentinel and may arm the probe). See
// the state machine in gpu_safe_mode.cpp.
BootDecision DecideAtBoot();

// Clear the boot sentinel: proof the renderer composited this run, so the GPU path
// is healthy. Call from the main browser's load-completion hook. UI-thread only
// and idempotent (no-op after the first call and when no sentinel exists).
void NotePaintSuccess();

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
// windows do not overlap. GpuSafeMode's sentinel is cleared by the main frame's
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

struct BootDecision {
	// Effective value to publish to obs-browser as "BrowserHWAccel".
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
