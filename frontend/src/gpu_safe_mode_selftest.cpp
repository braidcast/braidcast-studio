// RunGpuSafeModeSelfTest: GpuSafeMode's pure rules -- which WebGL renderer strings count as
// hardware, the boot plan for each combination of override, marker, sentinels and strikes
// (including the once-per-CEF-version re-probe and the override's precedence), what a
// renderer report and a no-verdict close do, and the marker/strike bodies. File-free: the
// decisions are tested as data, not through the config directory the running app uses.

#include <string>

#include "gpu_safe_mode.hpp"
#include "log.hpp"
#include "obs_bootstrap.hpp"

namespace {

using GpuSafeMode::BootInputs;
using GpuSafeMode::BootPlan;
using GpuSafeMode::BootReason;
using GpuSafeMode::ProbeEvent;
using GpuSafeMode::Renderer;

constexpr char kCef[] = "127.145.7+g2b7d20b+chromium-127.0.6533.120";
constexpr char kOtherCef[] = "150.0.0+gabcdef0+chromium-150.0.7871.0";

struct RendererCase {
	const char *renderer;
	Renderer expected;
};

constexpr RendererCase kRendererCases[] = {
	{"ANGLE (NVIDIA, NVIDIA GeForce RTX 5070 Ti (0x00002C05) Direct3D11 vs_5_0 ps_5_0, D3D11)", Renderer::Hardware},
	{"ANGLE (AMD, AMD Radeon 780M Graphics (0x000015BF) Direct3D11 vs_5_0 ps_5_0, D3D11)", Renderer::Hardware},
	{"ANGLE (Intel, Intel(R) UHD Graphics 770 (0x00004680) Direct3D11 vs_5_0 ps_5_0, D3D11)", Renderer::Hardware},
	{"ANGLE (Google, Vulkan 1.3.0 (SwiftShader Device (Subzero) (0x0000C0DE)), SwiftShader driver)",
	 Renderer::Software},
	{"Google SwiftShader", Renderer::Software},
	{"ANGLE (Microsoft, Microsoft Basic Render Driver (0x0000008C) Direct3D11 vs_5_0 ps_5_0, D3D11)",
	 Renderer::Software},
	{"llvmpipe (LLVM 15.0.7, 256 bits)", Renderer::Software},
	{"", Renderer::Software},
};

BootInputs Inputs(bool disableMarker, bool marker, const char *markerCef, bool paint, bool renderer, int strikes = 0,
		  bool haveBase = true)
{
	BootInputs in;
	in.disableMarker = disableMarker;
	in.haveConfigBase = haveBase;
	in.safeModeMarker = marker;
	in.markerCefVersion = markerCef;
	in.runningCefVersion = kCef;
	in.paintSentinel = paint;
	in.rendererSentinel = renderer;
	in.strikes = strikes;
	return in;
}

struct PlanCase {
	const char *name;
	BootInputs in;
	BootPlan expected;
};

BootPlan Expect(bool disableGpu, BootReason reason, bool writeMarker, bool removeMarker, bool clearProbes,
		bool armProbes, int strikes)
{
	BootPlan p;
	p.disableGpu = disableGpu;
	p.reason = reason;
	p.writeMarker = writeMarker;
	p.removeMarker = removeMarker;
	p.clearProbes = clearProbes;
	p.armProbes = armProbes;
	p.strikes = strikes;
	return p;
}

// Expect(disableGpu, reason, writeMarker, removeMarker, clearProbes, armProbes, strikes)
const PlanCase kPlanCases[] = {
	{"clean", Inputs(false, false, "", false, false),
	 Expect(false, BootReason::Hardware, false, false, false, true, 0)},
	{"clean-carries-strike", Inputs(false, false, "", false, false, 1),
	 Expect(false, BootReason::Hardware, false, false, false, true, 1)},
	{"no-base", Inputs(false, false, "", false, false, 0, false),
	 Expect(false, BootReason::NoConfigBase, false, false, false, false, 0)},
	{"override", Inputs(true, false, "", false, false),
	 Expect(true, BootReason::ManualOverride, false, false, false, false, 0)},
	// The override wins over everything and consumes nothing: a stale-version marker
	// and both sentinels are left for the boot after the override is removed.
	{"override-beats-reprobe", Inputs(true, true, kOtherCef, true, true),
	 Expect(true, BootReason::ManualOverride, false, false, false, false, 0)},
	{"persisted", Inputs(false, true, kCef, false, false),
	 Expect(true, BootReason::PersistedSafeMode, false, false, true, false, 0)},
	// Stale sentinels under a same-version marker are cleared, so deleting the marker
	// later yields a clean probe boot rather than a strike.
	{"persisted-stale-sentinels", Inputs(false, true, kCef, true, true, 1),
	 Expect(true, BootReason::PersistedSafeMode, false, false, true, false, 0)},
	{"reprobe-new-cef", Inputs(false, true, kOtherCef, false, false),
	 Expect(false, BootReason::ReprobeAfterCefChange, false, true, true, true, 0)},
	{"reprobe-legacy-marker", Inputs(false, true, "", false, false),
	 Expect(false, BootReason::ReprobeAfterCefChange, false, true, true, true, 0)},
	{"no-paint-strike-1", Inputs(false, false, "", true, true),
	 Expect(false, BootReason::NoPaintLastBoot, false, false, true, true, 1)},
	{"no-paint-strike-2", Inputs(false, false, "", true, false, 1),
	 Expect(true, BootReason::NoPaintLastBoot, true, false, true, false, 0)},
	{"no-report-strike-1", Inputs(false, false, "", false, true),
	 Expect(false, BootReason::NoRendererReportLastBoot, false, false, true, true, 1)},
	{"no-report-strike-2", Inputs(false, false, "", false, true, 1),
	 Expect(true, BootReason::NoRendererReportLastBoot, true, false, true, false, 0)},
};

bool SamePlan(const BootPlan &a, const BootPlan &b)
{
	return a.disableGpu == b.disableGpu && a.reason == b.reason && a.writeMarker == b.writeMarker &&
	       a.removeMarker == b.removeMarker && a.clearProbes == b.clearProbes && a.armProbes == b.armProbes &&
	       a.strikes == b.strikes;
}

// The boot after a run that armed both sentinels, closed them per `close`, and left
// `strikes` on disk.
BootPlan NextBoot(const GpuSafeMode::ProbeClose &close, bool painted, int strikes)
{
	const bool paintLeft = !painted && !close.removePaintSentinel;
	return GpuSafeMode::PlanBoot(Inputs(false, false, "", paintLeft, !close.removeRendererSentinel, strikes));
}

} // namespace

void ObsBootstrap::RunGpuSafeModeSelfTest()
{
	std::string failures;
	for (const RendererCase &c : kRendererCases) {
		if (GpuSafeMode::ClassifyRenderer(c.renderer) != c.expected) {
			failures += std::string(" renderer:\"") + GpuSafeMode::SanitizeRenderer(c.renderer) + "\"";
		}
	}
	for (const PlanCase &c : kPlanCases) {
		if (!SamePlan(GpuSafeMode::PlanBoot(c.in), c.expected)) {
			failures += std::string(" plan:") + c.name;
		}
	}

	// Renderer reports. An explicit software report persists at once; a hardware one
	// clears the strikes, so the boot after is a plain probe with none carried.
	const GpuSafeMode::ReportPlan sw = GpuSafeMode::PlanReport(false, BootReason::Hardware, Renderer::Software);
	const GpuSafeMode::ReportPlan hw =
		GpuSafeMode::PlanReport(false, BootReason::NoRendererReportLastBoot, Renderer::Hardware);
	const GpuSafeMode::ReportPlan already =
		GpuSafeMode::PlanReport(true, BootReason::PersistedSafeMode, Renderer::Software);
	const GpuSafeMode::ReportPlan noBase =
		GpuSafeMode::PlanReport(false, BootReason::NoConfigBase, Renderer::Software);
	if (!sw.persistSoftware || !sw.clearRendererSentinel || !sw.clearStrikes) {
		failures += " report:software";
	}
	if (hw.persistSoftware || !hw.clearRendererSentinel || !hw.clearStrikes ||
	    !SamePlan(GpuSafeMode::PlanBoot(Inputs(false, false, "", false, false, 0)),
		      Expect(false, BootReason::Hardware, false, false, false, true, 0))) {
		failures += " report:hardware-clears-strikes";
	}
	if (already.persistSoftware || already.clearStrikes || noBase.persistSoftware || noBase.clearRendererSentinel) {
		failures += " report:no-verdict";
	}

	// Runs that say nothing about the GPU. A load error retires both sentinels whether or
	// not anything painted; a deliberate exit after a paint with no report retires the
	// renderer one. Either way the next boot is a probe, and a carried strike is kept.
	const BootPlan afterLoadError =
		NextBoot(GpuSafeMode::PlanClose(ProbeEvent::MainFrameLoadError, false), false, 1);
	if (!SamePlan(afterLoadError, Expect(false, BootReason::Hardware, false, false, false, true, 1))) {
		failures += " close:load-error";
	}
	const BootPlan afterExit = NextBoot(GpuSafeMode::PlanClose(ProbeEvent::DeliberateExit, false), true, 0);
	if (!SamePlan(afterExit, Expect(false, BootReason::Hardware, false, false, false, true, 0))) {
		failures += " close:exit-no-report";
	}
	// ...but closing a UI that never painted is the blank-window case, and is a strike.
	const BootPlan afterBlankExit = NextBoot(GpuSafeMode::PlanClose(ProbeEvent::DeliberateExit, false), false, 0);
	if (afterBlankExit.reason != BootReason::NoPaintLastBoot || afterBlankExit.strikes != 1) {
		failures += " close:blank-exit";
	}

	// A re-probe does not repeat: the marker a failed re-probe writes carries this CEF, and
	// the boot after reads it as persisted rather than probing again.
	const std::string written = GpuSafeMode::FormatMarker(kCef, BootReason::NoPaintLastBoot);
	const BootPlan afterFailedReprobe = GpuSafeMode::PlanBoot(
		Inputs(false, true, GpuSafeMode::MarkerCefVersion(written).c_str(), false, false));
	if (afterFailedReprobe.reason != BootReason::PersistedSafeMode) {
		failures += " reprobe-once";
	}
	if (GpuSafeMode::MarkerCefVersion("reason=x\r\ncef=" + std::string(kCef) + "\r\n") != kCef ||
	    !GpuSafeMode::MarkerCefVersion("").empty()) {
		failures += " marker-parse";
	}
	if (GpuSafeMode::ParseStrikes(GpuSafeMode::FormatStrikes(1)) != 1 || GpuSafeMode::ParseStrikes("") != 0 ||
	    GpuSafeMode::ParseStrikes("strikes=-3") != 0 || GpuSafeMode::ParseStrikes("strikes=x") != 0) {
		failures += " strikes-parse";
	}
	if (GpuSafeMode::SanitizeRenderer("ANGLE (NVIDIA, RTX (0x00002C05) D3D11)\n\"x") !=
	    "ANGLE (NVIDIA, RTX D3D11)x") {
		failures += " sanitize";
	}

	HostLog(std::string("[selftest] gpu safe mode -> ") + (failures.empty() ? "OK" : "MISMATCH:" + failures));
}
