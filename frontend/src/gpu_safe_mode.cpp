#include "gpu_safe_mode.hpp"

#include <util/bmem.h>
#include <util/platform.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "include/cef_version.h"
#include "log.hpp"
#include "multistream/StorePaths.hpp"
#include "util/env_config.hpp"
#include "util/string_util.hpp"

namespace {

// Per-boot sentinel: armed once the main browser exists, deleted on the main frame's
// first load. Its survival to the next boot means the UI never came up: a strike.
constexpr char kSentinelFile[] = "gpu-probe.lock";

// Per-boot sentinel armed alongside kSentinelFile, deleted by the main page's
// renderer report. Its survival means the page never reported: a strike.
constexpr char kRendererSentinelFile[] = "gpu-renderer-probe.lock";

// Consecutive strikes, carried across GPU boots until a hardware report clears them
// or kStrikesToPersist of them persist safe mode. Its own file rather than a line in
// a sentinel, because a run that retires its sentinels without a verdict (a load
// error, a deliberate exit) must not reset the count. Body per FormatStrikes.
constexpr char kStrikesFile[] = "gpu-probe.strikes";

// Sticky safe-mode marker: once software is detected the app persists this so every
// later boot under the same CEF stays on software rendering without re-probing (and
// re-crashing). Body per FormatMarker.
constexpr char kSafeModeFile[] = "gpu-safe-mode.on";

constexpr char kMarkerCefKey[] = "cef=";
constexpr char kStrikesKey[] = "strikes=";

// WebGL renderer substrings that mean the page is not on a hardware GPU, matched
// case-insensitively. SwiftShader is Chromium's own fallback; the other two are the
// Windows and Mesa software rasterizers a remote or driverless session reports.
constexpr const char *kSoftwareRendererNeedles[] = {"swiftshader", "basic render driver", "llvmpipe"};

constexpr size_t kMaxLoggedRendererChars = 160;

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

// UI-thread-only latch: only the first renderer report of a launch is a verdict.
bool g_rendererReported = false;

// Set by GpuSafeMode::DecideAtBoot and read by everything after it on the main/UI
// thread (CEF runs its UI thread on the main thread here).
bool g_softwareRendering = false;
GpuSafeMode::BootDecision g_bootDecision;
bool g_armPending = false;

// Set by ArmProbes: this launch's sentinels are on disk and NoteMainFrameLoadError /
// NoteDeliberateExit may retire them.
bool g_probesArmed = false;
int g_mainBrowserId = 0;

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

std::string ReadTextFile(const std::string &absPath)
{
	char *raw = os_quick_read_utf8_file(absPath.c_str());
	if (!raw) {
		return std::string();
	}
	std::string body(raw);
	bfree(raw);
	return body;
}

void WriteTextFile(const std::string &absPath, const std::string &body, const char *consequence)
{
	if (!os_quick_write_utf8_file(absPath.c_str(), body.c_str(), body.size(), false)) {
		HostLog("[gpu] could not write " + absPath + "; " + consequence);
	}
}

void WriteSafeModeMarker(const std::string &absPath, GpuSafeMode::BootReason reason)
{
	WriteTextFile(absPath, GpuSafeMode::FormatMarker(CEF_VERSION, reason),
		      "software mode will not persist to the next boot");
}

void StoreStrikes(int strikes)
{
	const std::string path = BraidcastConfigPath(kStrikesFile);
	if (strikes <= 0) {
		RemoveFile(path);
		return;
	}
	WriteTextFile(path, GpuSafeMode::FormatStrikes(strikes),
		      "the GPU strike count will not carry to the next boot");
}

GpuSafeMode::BootDecision ResolveBootDecision()
{
	GpuSafeMode::BootInputs in;
	in.runningCefVersion = CEF_VERSION;
	in.disableMarker = MarkerFileBesideExe(kDisableGpuMarker);

	std::string sentinel;
	std::string rendererSentinel;
	// Resolved even under the override, so the boot log can name it.
	const std::string safeMode = BraidcastConfigPath(kSafeModeFile);
	if (!in.disableMarker) {
		in.haveConfigBase = !ProbeBaseDir().empty();
	}
	const bool probing = !in.disableMarker && in.haveConfigBase;
	if (probing) {
		sentinel = BraidcastConfigPath(kSentinelFile);
		rendererSentinel = BraidcastConfigPath(kRendererSentinelFile);
		in.safeModeMarker = FileExists(safeMode);
		if (in.safeModeMarker) {
			in.markerCefVersion = GpuSafeMode::MarkerCefVersion(ReadTextFile(safeMode));
		}
		in.paintSentinel = FileExists(sentinel);
		in.rendererSentinel = FileExists(rendererSentinel);
		in.strikes = GpuSafeMode::ParseStrikes(ReadTextFile(BraidcastConfigPath(kStrikesFile)));
	}

	const GpuSafeMode::BootPlan plan = GpuSafeMode::PlanBoot(in);
	if (plan.removeMarker) {
		RemoveFile(safeMode);
	}
	if (plan.clearProbes) {
		RemoveFile(sentinel);
		RemoveFile(rendererSentinel);
	}
	if (plan.writeMarker) {
		WriteSafeModeMarker(safeMode, plan.reason);
	}
	if (probing && plan.strikes != in.strikes) {
		StoreStrikes(plan.strikes);
	}
	g_armPending = plan.armProbes;

	GpuSafeMode::BootDecision decision;
	decision.disableGpu = plan.disableGpu;
	decision.reason = plan.reason;
	decision.strikes = plan.strikes;
	decision.safeModeFile = safeMode;
	return decision;
}

// Retire this launch's still-armed sentinels per PlanClose.
void CloseProbes(GpuSafeMode::ProbeEvent event)
{
	if (!g_probesArmed) {
		return;
	}
	const GpuSafeMode::ProbeClose close = GpuSafeMode::PlanClose(event, g_rendererReported);
	if (close.removePaintSentinel) {
		g_sentinelCleared = true;
		RemoveFile(BraidcastConfigPath(kSentinelFile));
	}
	if (close.removeRendererSentinel) {
		RemoveFile(BraidcastConfigPath(kRendererSentinelFile));
	}
}

// The value of the first `key` line in a `key=value` body, or empty.
std::string LineValue(const std::string &body, const char *key)
{
	size_t pos = 0;
	while (pos < body.size()) {
		size_t end = body.find('\n', pos);
		if (end == std::string::npos) {
			end = body.size();
		}
		std::string line = body.substr(pos, end - pos);
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.rfind(key, 0) == 0) {
			return line.substr(std::strlen(key));
		}
		pos = end + 1;
	}
	return std::string();
}

} // namespace

GpuSafeMode::BootPlan GpuSafeMode::PlanBoot(const BootInputs &in)
{
	BootPlan plan;

	// The override beside the exe wins unconditionally, needs no config dir, and
	// touches no probe file: removing it must find the verdict it was hiding.
	if (in.disableMarker) {
		plan.disableGpu = true;
		plan.reason = BootReason::ManualOverride;
		plan.strikes = in.strikes;
		return plan;
	}

	// No writable config base: nothing can carry a verdict, so leave the GPU on.
	if (!in.haveConfigBase) {
		plan.reason = BootReason::NoConfigBase;
		return plan;
	}

	if (in.safeModeMarker) {
		if (in.markerCefVersion == in.runningCefVersion) {
			// Software boots arm nothing, so anything left over is stale; clearing it
			// here is what makes deleting the marker always yield a clean probe boot.
			plan.disableGpu = true;
			plan.reason = BootReason::PersistedSafeMode;
			plan.clearProbes = true;
			return plan;
		}
		// Written under another CEF (or before markers carried a version): this CEF
		// may handle the GPU, so probe it afresh from zero strikes. Whatever marker the
		// probe ends up writing carries THIS version, so it is not probed again.
		plan.reason = BootReason::ReprobeAfterCefChange;
		plan.removeMarker = true;
		plan.clearProbes = true;
		plan.armProbes = true;
		return plan;
	}

	if (in.paintSentinel || in.rendererSentinel) {
		plan.reason = in.paintSentinel ? BootReason::NoPaintLastBoot : BootReason::NoRendererReportLastBoot;
		plan.clearProbes = true;
		plan.strikes = in.strikes + 1;
		if (plan.strikes >= kStrikesToPersist) {
			plan.disableGpu = true;
			plan.writeMarker = true;
			plan.strikes = 0;
		} else {
			plan.armProbes = true;
		}
		return plan;
	}

	plan.reason = BootReason::Hardware;
	plan.armProbes = true;
	plan.strikes = in.strikes;
	return plan;
}

GpuSafeMode::ReportPlan GpuSafeMode::PlanReport(bool softwareRequested, BootReason bootReason, Renderer renderer)
{
	ReportPlan plan;
	if (softwareRequested) {
		plan.decision = "none, already software";
		return plan;
	}
	if (bootReason == BootReason::NoConfigBase) {
		plan.decision = "none, no config dir to persist a verdict in";
		return plan;
	}
	plan.clearRendererSentinel = true;
	plan.clearStrikes = true;
	if (renderer == Renderer::Hardware) {
		plan.decision = "hardware confirmed, GPU stays on, strikes cleared";
		return plan;
	}
	plan.persistSoftware = true;
	plan.decision = "software mode persisted for the next boot";
	return plan;
}

GpuSafeMode::ProbeClose GpuSafeMode::PlanClose(ProbeEvent event, bool reported)
{
	ProbeClose close;
	close.removeRendererSentinel = !reported;
	// A load error means the app never ran, so whether it painted says nothing about the
	// GPU. A deliberate exit keeps the paint sentinel: closing a window that never came
	// up is the blank-UI case.
	close.removePaintSentinel = event == ProbeEvent::MainFrameLoadError;
	return close;
}

GpuSafeMode::Renderer GpuSafeMode::ClassifyRenderer(const std::string &renderer)
{
	if (renderer.empty()) {
		return Renderer::Software;
	}
	const std::string lower = StringUtil::ToLower(renderer);
	for (const char *needle : kSoftwareRendererNeedles) {
		if (lower.find(needle) != std::string::npos) {
			return Renderer::Software;
		}
	}
	return Renderer::Hardware;
}

std::string GpuSafeMode::SanitizeRenderer(const std::string &renderer)
{
	std::string out;
	out.reserve(std::min(renderer.size(), kMaxLoggedRendererChars));
	for (size_t i = 0; i < renderer.size() && out.size() < kMaxLoggedRendererChars; ++i) {
		const unsigned char c = static_cast<unsigned char>(renderer[i]);
		// ANGLE writes the PCI device id as " (0x0000xxxx)"; the GPU name is enough.
		if (renderer.compare(i, 4, " (0x") == 0) {
			const size_t close = renderer.find(')', i);
			if (close != std::string::npos) {
				i = close;
				continue;
			}
		}
		if (c >= 0x20 && c != 0x7f && c != '"') {
			out.push_back(static_cast<char>(c));
		}
	}
	return out;
}

std::string GpuSafeMode::FormatMarker(const std::string &cefVersion, BootReason reason)
{
	return std::string(kMarkerCefKey) + cefVersion + "\nreason=" + ReasonName(reason) + "\n";
}

std::string GpuSafeMode::MarkerCefVersion(const std::string &markerBody)
{
	return LineValue(markerBody, kMarkerCefKey);
}

std::string GpuSafeMode::FormatStrikes(int strikes)
{
	return std::string(kStrikesKey) + std::to_string(strikes) + "\n";
}

int GpuSafeMode::ParseStrikes(const std::string &body)
{
	const std::string value = LineValue(body, kStrikesKey);
	if (value.empty() || value.size() > 2 ||
	    !std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) {
		return 0;
	}
	return std::stoi(value);
}

const char *GpuSafeMode::ReasonName(BootReason reason)
{
	switch (reason) {
	case BootReason::Hardware:
		return "hardware-probe";
	case BootReason::NoConfigBase:
		return "no-config-base";
	case BootReason::ManualOverride:
		return "manual-override";
	case BootReason::PersistedSafeMode:
		return "persisted-safe-mode";
	case BootReason::ReprobeAfterCefChange:
		return "reprobe-after-cef-change";
	case BootReason::NoPaintLastBoot:
		return "no-paint-last-boot";
	case BootReason::NoRendererReportLastBoot:
		return "no-renderer-report-last-boot";
	case BootReason::SoftwareRendererReported:
		return "software-renderer-reported";
	}
	return "unknown";
}

GpuSafeMode::BootDecision GpuSafeMode::DecideAtBoot()
{
	g_bootDecision = ResolveBootDecision();
	g_softwareRendering = g_bootDecision.disableGpu;
	return g_bootDecision;
}

void GpuSafeMode::ArmProbes(int mainBrowserId)
{
	if (!g_armPending) {
		return;
	}
	g_armPending = false;
	WriteEmptyFile(BraidcastConfigPath(kSentinelFile));
	WriteEmptyFile(BraidcastConfigPath(kRendererSentinelFile));
	g_mainBrowserId = mainBrowserId;
	g_probesArmed = true;
}

void GpuSafeMode::LogBootDecision()
{
	const BootDecision &d = g_bootDecision;
	std::string line = std::string("[gpu] boot: requested=") + (d.disableGpu ? "software" : "gpu") +
			   " reason=" + ReasonName(d.reason) + " strikes=" + std::to_string(d.strikes) + "/" +
			   std::to_string(kStrikesToPersist) + " cef=" + CEF_VERSION;
	if (!d.safeModeFile.empty()) {
		line += " marker=" + d.safeModeFile;
	}
	if (d.reason == BootReason::ManualOverride) {
		line += " (remove braidcast_disable_gpu.txt beside the exe to re-probe the GPU)";
	} else if (d.disableGpu) {
		line += " (delete the marker to re-probe the GPU)";
	}
	HostLog(line);
}

void GpuSafeMode::NotePaintSuccess()
{
	if (g_sentinelCleared) {
		return;
	}
	g_sentinelCleared = true;
	RemoveFile(BraidcastConfigPath(kSentinelFile));
}

void GpuSafeMode::NoteRendererReport(const std::string &renderer)
{
	if (g_rendererReported) {
		return;
	}
	g_rendererReported = true;

	const BootDecision &d = g_bootDecision;
	const Renderer kind = ClassifyRenderer(renderer);
	const ReportPlan plan = PlanReport(d.disableGpu, d.reason, kind);
	if (plan.clearRendererSentinel) {
		RemoveFile(BraidcastConfigPath(kRendererSentinelFile));
	}
	if (plan.clearStrikes) {
		StoreStrikes(0);
	}
	if (plan.persistSoftware) {
		WriteSafeModeMarker(d.safeModeFile, BootReason::SoftwareRendererReported);
	}
	HostLog(std::string("[gpu] renderer report: requested=") + (d.disableGpu ? "software" : "gpu") +
		" renderer=\"" + (renderer.empty() ? "<no renderer string>" : SanitizeRenderer(renderer)) + "\" -> " +
		(kind == Renderer::Hardware ? "hardware" : "software") + "; decision: " + plan.decision +
		(plan.persistSoftware ? " (marker=" + d.safeModeFile + ")" : std::string()));
}

void GpuSafeMode::NoteMainFrameLoadError(int browserId)
{
	if (!g_probesArmed || browserId != g_mainBrowserId) {
		return;
	}
	CloseProbes(ProbeEvent::MainFrameLoadError);
	HostLog("[gpu] main frame failed to load -- this launch carries no GPU verdict");
}

void GpuSafeMode::NoteDeliberateExit()
{
	CloseProbes(ProbeEvent::DeliberateExit);
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
