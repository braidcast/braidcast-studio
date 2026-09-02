#include "gpu_safe_mode.hpp"

#include <util/platform.h>

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "log.hpp"
#include "multistream/StorePaths.hpp"
#include "util/env_config.hpp"

namespace {

// Per-boot sentinel: written before the GPU-backed browser is created, deleted on
// the first confirmed paint. Its survival to the next boot is the GPU-crash signal.
constexpr char kSentinelFile[] = "gpu-probe.lock";

// Sticky safe-mode flag: once a GPU crash is detected the app persists this so every
// later boot stays on software rendering without re-probing (and re-crashing).
constexpr char kSafeModeFile[] = "gpu-safe-mode.on";

// User-createable override next to the exe -- mirrors the portable marker. Its mere
// presence forces software rendering, the escape hatch for a blank-screen install
// whose UI never comes up.
constexpr wchar_t kDisableGpuMarker[] = L"braidcast_disable_gpu.txt";

// Per-boot sentinel for the browser-source GPU path: written when hardware
// acceleration is applied, deleted by whichever of the three closers the CEF UI
// thread reaches first. Distinct from kSentinelFile because the two probe windows
// do not overlap (see the header).
constexpr char kHwAccelSentinelFile[] = "browser-hwaccel-probe.lock";

// UI-thread-only latch so repeated main-frame loads (e.g. detached windows) clear
// the sentinel just once.
bool g_sentinelCleared = false;

// Set by GpuSafeMode::DecideAtBoot, read by SoftwareRendering().
bool g_softwareRendering = false;

// True only while THIS process holds an armed browser-source sentinel. Everything
// that closes the probe checks it first, so an early-abort launch -- a
// single-instance rejection, a failed CefInitialize -- cannot delete the verdict a
// previous run left on disk. Atomic because every closer but one runs on TID_UI
// and the crash sink enters from whichever thread faulted.
std::atomic<bool> g_hwAccelProbeArmed{false};

// The armed sentinel's absolute path, resolved to a wide string at arm time so the
// crash sink can delete it with a bare DeleteFileW. That path is the only one that
// may not allocate or resolve anything (see session_log.cpp's handler contract), so
// it cannot go through BraidcastConfigPath/RemoveFile like the others. Published
// before g_hwAccelProbeArmed, whose seq_cst store is what makes it visible to a
// thread that observes the arm.
std::wstring g_hwAccelSentinelPathW;

// True between DecideAtBoot reporting a crash and the caller confirming the
// suppression reached disk. The sentinel deliberately stays on disk across that
// gap: the stored setting IS the latch, so consuming the verdict before the latch
// is durable would leave a failed save with neither, and the next launch would
// re-enable acceleration into the same freeze.
bool g_hwAccelSuppressionPending = false;

bool FileExists(const std::string &absPath)
{
	if (absPath.empty()) {
		return false;
	}
	std::error_code ec;
	return std::filesystem::exists(std::filesystem::u8path(absPath), ec);
}

void WriteEmptyFile(const std::string &absPath)
{
	if (absPath.empty()) {
		return;
	}
	std::ofstream file(std::filesystem::u8path(absPath), std::ios::binary | std::ios::trunc);
	if (!file) {
		HostLog("[gpu] could not write " + absPath +
			"; the GPU crash probe cannot carry its verdict to the next boot");
	}
}

void RemoveFile(const std::string &absPath)
{
	if (absPath.empty()) {
		return;
	}
	std::error_code ec;
	std::filesystem::remove(std::filesystem::u8path(absPath), ec);
}

// The config base both probes write their sentinel into, created if missing.
// Empty when the platform resolved no writable config path, which is the one
// state in which neither probe can carry a verdict to the next boot.
std::string ProbeBaseDir()
{
	const std::string &base = BraidcastConfigDir();
	if (base.empty()) {
		return std::string();
	}
	os_mkdirs(base.c_str());
	return base;
}

// Claim the right to close the probe. True for exactly one caller, so the disarm
// doubles as the once-only guard across the UI thread and the crashing thread.
bool DisarmHwAccelProbe()
{
	return g_hwAccelProbeArmed.exchange(false);
}

// Delete the browser-source sentinel the ordinary way. NOT for the crash path,
// which has its own allocation-free deletion.
void RemoveHwAccelSentinel()
{
	RemoveFile(BraidcastConfigPath(kHwAccelSentinelFile));
}

GpuSafeMode::BootDecision ResolveBootDecision()
{
	GpuSafeMode::BootDecision decision;

	// Manual override beside the exe wins unconditionally and needs no config dir.
	if (MarkerFileBesideExe(kDisableGpuMarker)) {
		decision.disableGpu = true;
		return decision;
	}

	if (ProbeBaseDir().empty()) {
		// No writable config base -> can't run the sentinel probe; leave GPU enabled.
		return decision;
	}

	const std::string sentinel = BraidcastConfigPath(kSentinelFile);
	const std::string safeMode = BraidcastConfigPath(kSafeModeFile);
	decision.safeModeFile = safeMode;

	// A previously-persisted crash forces software rendering with no probe.
	if (FileExists(safeMode)) {
		decision.disableGpu = true;
		return decision;
	}

	// A sentinel left from the previous run means that run started the GPU-backed
	// browser but never confirmed a paint: almost certainly the GPU crash loop. Fall
	// back now, persist it so future boots skip the probe, and clear the stale file.
	if (FileExists(sentinel)) {
		decision.disableGpu = true;
		decision.autoFellBack = true;
		WriteEmptyFile(safeMode);
		RemoveFile(sentinel);
		return decision;
	}

	// Healthy path: arm the probe for this GPU-enabled boot. NotePaintSuccess() clears
	// it once the renderer composites, proving the GPU path works this run.
	WriteEmptyFile(sentinel);
	return decision;
}

} // namespace

GpuSafeMode::BootDecision GpuSafeMode::DecideAtBoot()
{
	const BootDecision decision = ResolveBootDecision();
	g_softwareRendering = decision.disableGpu;
	return decision;
}

void GpuSafeMode::NotePaintSuccess()
{
	if (g_sentinelCleared) {
		return;
	}
	g_sentinelCleared = true;
	RemoveFile(BraidcastConfigPath(kSentinelFile));
}

bool GpuSafeMode::SoftwareRendering()
{
	return g_softwareRendering;
}

BrowserHwAccel::BootDecision BrowserHwAccel::DecideAtBoot(bool enabledInSettings, bool gpuUnavailable)
{
	BootDecision decision;
	decision.enable = enabledInSettings && !gpuUnavailable;

	// An unattended run drives itself and is killed on a liveness timeout, so it
	// produces exactly the trace a freeze does. It gets no probe at all: nothing is
	// armed, and no verdict a real session left is consumed. Its config base is a
	// throwaway directory beside the exe rather than the developer's, so this is
	// belt-and-braces -- but the invariant belongs next to the writes it protects,
	// not several files away in the path resolver.
	if (Env::IsSelfTestRun()) {
		return decision;
	}

	if (ProbeBaseDir().empty()) {
		// No writable config base -> the probe cannot carry a verdict to the next
		// boot. Honor the setting rather than silently overriding it; the user still
		// has the checkbox.
		return decision;
	}

	const std::string sentinel = BraidcastConfigPath(kHwAccelSentinelFile);

	if (!enabledInSettings) {
		// The user's own choice is off, so a pending verdict is moot: it can only
		// have come from a run they have since turned acceleration off after. Only
		// this branch may delete it -- gpuUnavailable must not, see below.
		RemoveFile(sentinel);
		return decision;
	}

	if (gpuUnavailable) {
		// Acceleration is still wanted; this launch just cannot offer the GPU. It
		// therefore proves nothing either way, so it neither arms nor consumes.
		// Leaving a pending verdict standing is the whole point: the GPU-disable
		// marker beside the exe is what a user drops there to get a frozen app back,
		// and consuming the verdict on that launch would disarm the latch and walk
		// them straight back into the freeze once they removed the marker.
		return decision;
	}

	// A sentinel left from the previous run means that run published acceleration to
	// obs-browser and then reached neither the probe timer, teardown, nor the crash
	// sink -- the freeze this guard exists for, or one of the residual causes the
	// header names. Report it but LEAVE IT ON DISK: the caller turns the stored
	// setting off, and only once that has actually persisted is the verdict spent.
	// ConfirmSuppressionPersisted() is what retires it.
	if (FileExists(sentinel)) {
		decision.enable = false;
		decision.crashDetected = true;
		g_hwAccelSuppressionPending = true;
		return decision;
	}

	WriteEmptyFile(sentinel);
	g_hwAccelSentinelPathW = std::filesystem::u8path(sentinel).wstring();
	g_hwAccelProbeArmed.store(true);
	return decision;
}

void BrowserHwAccel::NoteSurvived()
{
	if (!DisarmHwAccelProbe()) {
		return;
	}
	RemoveHwAccelSentinel();
	HostLog("[gpu] browser hardware-acceleration probe closed -- CEF's UI thread was alive to close it");
}

void BrowserHwAccel::NoteFatalError()
{
	if (!DisarmHwAccelProbe()) {
		return;
	}
	// Deliberately not RemoveFile(BraidcastConfigPath(...)): both of those allocate,
	// and this runs on a thread that has just faulted -- possibly on a stack overflow,
	// possibly while holding the CRT heap lock. DeleteFileW on the path resolved at
	// arm time touches neither the heap nor the stack beyond the call itself.
	if (!g_hwAccelSentinelPathW.empty()) {
		DeleteFileW(g_hwAccelSentinelPathW.c_str());
	}
}

void BrowserHwAccel::ConfirmSuppressionPersisted()
{
	if (!g_hwAccelSuppressionPending) {
		return;
	}
	g_hwAccelSuppressionPending = false;
	RemoveHwAccelSentinel();
}
