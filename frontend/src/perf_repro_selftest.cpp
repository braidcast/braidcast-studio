#include "perf_repro_selftest.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>

#include <util/platform.h>

#include "bridge.hpp"
#include "log.hpp"
#include "util/session_log.hpp"

namespace {

// State-machine phases, advanced one step per WM_TIMER tick (see
// ObsBootstrap::RunPerfReproSelfTest). BootSettle and Measuring are the only
// phases that legitimately span multiple ticks; every other phase runs to
// completion within the tick it is entered on (see the `continue`-driven loop).
enum class Phase {
	Idle,
	BootSettle,
	CheckOptOut,
	Minimizing,
	Measuring,
	Restoring,
	WriteSummary,
	Finished,
};

struct State {
	Phase phase = Phase::Idle;
	int exitCode = -1; // -1 = not yet decided; see PerfReproSelfTestExitCode()
	std::string failReason;

	HWND host = nullptr;
	bool windowMinimized = false;

	std::chrono::steady_clock::time_point phaseStart;
	std::chrono::steady_clock::time_point measureStart;
	std::chrono::steady_clock::time_point lastSampleAt;

	int durationSec = 8;
	double maxLagPct = 2.0;

	double worstRenderLagPct = 0.0;
	double worstEncodeSkipPct = 0.0;
};

State g_state;

constexpr std::chrono::milliseconds kBootSettle{3000};
constexpr std::chrono::seconds kSampleInterval{1};

// Short, CI-friendly default background-measurement window. The old harness used
// 90s because it had to wait on a real YouTube go-live + poll-live handshake;
// there is no network wait here anymore, so a few seconds of sampling while
// minimized is enough to observe render pacing under the opt-out. Overridable via
// BRAIDCAST_SELFTEST_DURATION.
constexpr int kDefaultDurationSec = 8;
constexpr double kDefaultMaxLagPct = 2.0;

// The two control/state bits main.cpp's wWinMain asserts to opt out of background
// power throttling -- the same PROCESS_POWER_THROTTLING_* constants from
// <windows.h> that the SetProcessInformation call uses. ControlMask having both
// set = the process is asserting control; StateMask having both clear = it is not
// being throttled.
constexpr ULONG kOptOutBits = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
			      PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;

int EnvInt(const char *name, int fallback)
{
	const char *v = getenv(name);
	if (!v || !*v) {
		return fallback;
	}
	const int n = atoi(v);
	return n > 0 ? n : fallback;
}

double EnvDouble(const char *name, double fallback)
{
	const char *v = getenv(name);
	if (!v || !*v) {
		return fallback;
	}
	char *end = nullptr;
	const double d = strtod(v, &end);
	return (end && end != v) ? d : fallback;
}

// <config>/braidcast/selftest/<file>, mirroring the shape of MultistreamBasicPath
// (StorePaths.hpp) / SessionLog's "braidcast/logs" -- a fresh subdir, so the same
// one-line os_get_config_path idiom, not a shared helper (there is nothing to
// factor: each caller resolves a different, genuinely distinct subdir).
std::string SelfTestConfigPath(const std::string &file)
{
	char base[512];
	if (os_get_config_path(base, sizeof(base), "braidcast/selftest") <= 0) {
		return std::string();
	}
	os_mkdirs(base);
	std::string path = base;
	path += "/";
	path += file;
	return path;
}

// Reuse SessionLog's own per-session timestamp (its filename is already
// "YYYY-MM-DD HH-MM-SS.txt") instead of a fresh time() call, per the "reuse the
// session log's timestamp" instruction.
std::string TimestampFromSessionLog()
{
	const std::string logPath = SessionLog::CurrentPath();
	if (logPath.empty()) {
		return "unknown";
	}
	const size_t slash = logPath.find_last_of("/\\");
	const std::string base = slash == std::string::npos ? logPath : logPath.substr(slash + 1);
	const size_t dot = base.rfind(".txt");
	return dot == std::string::npos ? base : base.substr(0, dot);
}

// True iff main.cpp's wWinMain ProcessPowerThrottling opt-out is in force: both
// control bits asserted (main.cpp is asserting control) AND neither state bit set
// (the process is not being throttled). On a broken/absent opt-out, sets `reason`
// and returns false. If the GetProcessInformation query itself fails, sets
// `reason` and returns false with `apiFailed` true so the caller can map it to the
// infra exit code 3 rather than a FAIL.
bool ProcessPowerThrottleOptOutActive(std::string &reason, bool &apiFailed)
{
	apiFailed = false;

	PROCESS_POWER_THROTTLING_STATE state = {};
	state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
	if (!GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state))) {
		apiFailed = true;
		reason = "GetProcessInformation(ProcessPowerThrottling) failed: err=" + std::to_string(GetLastError());
		return false;
	}

	const bool controlAsserted = (state.ControlMask & kOptOutBits) == kOptOutBits;
	const bool notThrottled = (state.StateMask & kOptOutBits) == 0;
	if (controlAsserted && notThrottled) {
		return true;
	}

	std::string detail;
	if ((state.ControlMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) == 0) {
		detail += " EXECUTION_SPEED control bit not asserted;";
	}
	if ((state.ControlMask & PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION) == 0) {
		detail += " IGNORE_TIMER_RESOLUTION control bit not asserted;";
	}
	if ((state.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) != 0) {
		detail += " EXECUTION_SPEED still throttled (state bit set);";
	}
	if ((state.StateMask & PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION) != 0) {
		detail += " IGNORE_TIMER_RESOLUTION still throttled (state bit set);";
	}
	reason = "power-throttle opt-out missing/broken:" + detail;
	return false;
}

// One measurement sample via "stats.get" -- the SAME synchronous bridge method
// (bridge.cpp:MethodStatsGet) the Stats dock polls, so renderLagPct/encodeSkipPct
// are computed by the real stats source (obs_get_lagged_frames() /
// obs_get_total_frames() / video_output_get_skipped_frames), not re-derived here.
void SampleStats(State &st)
{
	Bridge::json result;
	std::string err;
	if (!Bridge::Dispatch("stats.get", Bridge::json(nullptr), result, err)) {
		return;
	}
	if (result.contains("general") && result["general"].is_object()) {
		const Bridge::json &g = result["general"];
		st.worstRenderLagPct = std::max(st.worstRenderLagPct, g.value("renderLagPct", 0.0));
		st.worstEncodeSkipPct = std::max(st.worstEncodeSkipPct, g.value("encodeSkipPct", 0.0));
	}
}

void WriteSummary(State &st)
{
	if (st.exitCode < 0) {
		st.exitCode = st.worstRenderLagPct > st.maxLagPct ? 1 : 0;
	}
	const char *resultName = st.exitCode == 0   ? "PASS"
				 : st.exitCode == 1 ? "FAIL"
				 : st.exitCode == 2 ? "SKIP"
						    : "ERROR";

	const Bridge::json summary{
		{"result", resultName},
		{"exitCode", st.exitCode},
		{"failReason", st.failReason},
		{"durationSec", st.durationSec},
		{"maxLagPctThreshold", st.maxLagPct},
		{"worstRenderLagPct", st.worstRenderLagPct},
		{"worstEncodeSkipPct", st.worstEncodeSkipPct},
	};

	const std::string path = SelfTestConfigPath("perf-repro-" + TimestampFromSessionLog() + ".txt");
	if (!path.empty()) {
		std::ofstream out(path, std::ios::out | std::ios::trunc);
		if (out) {
			out << summary.dump(2);
		}
	}

	HostLog(std::string("[selftest-stream] perf-repro ") + resultName + " worstRenderLagPct=" +
		std::to_string(st.worstRenderLagPct) + " worstEncodeSkipPct=" + std::to_string(st.worstEncodeSkipPct) +
		" threshold=" + std::to_string(st.maxLagPct) + " summary=" + (path.empty() ? "(unwritten)" : path));
}

} // namespace

void ObsBootstrap::ArmPerfReproSelfTest(HWND host)
{
	g_state = State{};
	g_state.phase = Phase::BootSettle;
	g_state.host = host;
	g_state.phaseStart = std::chrono::steady_clock::now();
	g_state.durationSec = EnvInt("BRAIDCAST_SELFTEST_DURATION", kDefaultDurationSec);
	g_state.maxLagPct = EnvDouble("BRAIDCAST_SELFTEST_MAXLAG", kDefaultMaxLagPct);
	HostLog("[selftest-stream] armed: duration=" + std::to_string(g_state.durationSec) +
		"s maxLagPct=" + std::to_string(g_state.maxLagPct));
}

bool ObsBootstrap::RunPerfReproSelfTest()
{
	State &st = g_state;

	for (;;) {
		switch (st.phase) {
		case Phase::Idle:
			return true; // never armed

		case Phase::BootSettle: {
			if (std::chrono::steady_clock::now() - st.phaseStart < kBootSettle) {
				return false;
			}
			st.phase = Phase::CheckOptOut;
			continue;
		}

		case Phase::CheckOptOut: {
			std::string reason;
			bool apiFailed = false;
			if (!ProcessPowerThrottleOptOutActive(reason, apiFailed)) {
				st.failReason = reason;
				st.exitCode = apiFailed ? 3 : 1;
				HostLog("[selftest-stream] " + reason + " (exitCode " + std::to_string(st.exitCode) +
					")");
				// Nothing was minimized yet, so skip Restoring and finalize.
				st.phase = Phase::WriteSummary;
				continue;
			}
			HostLog("[selftest-stream] power-throttle opt-out active: EXECUTION_SPEED + "
				"IGNORE_TIMER_RESOLUTION control asserted, process not throttled");
			st.phase = Phase::Minimizing;
			continue;
		}

		case Phase::Minimizing: {
			if (st.host) {
				ShowWindow(st.host, SW_MINIMIZE);
				st.windowMinimized = true;
				HostLog("[selftest-stream] host window minimized (simulating loss of foreground)");
			} else {
				HostLog("[selftest-stream] no host window supplied; measuring in-place");
			}
			st.measureStart = std::chrono::steady_clock::now();
			st.lastSampleAt = st.measureStart;
			st.phase = Phase::Measuring;
			// Return so the minimize takes effect before the first sample.
			return false;
		}

		case Phase::Measuring: {
			const auto now = std::chrono::steady_clock::now();
			if (now - st.lastSampleAt >= kSampleInterval) {
				SampleStats(st);
				st.lastSampleAt = now;
			}
			if (now - st.measureStart >= std::chrono::seconds(st.durationSec)) {
				HostLog("[selftest-stream] measurement window complete (" +
					std::to_string(st.durationSec) + "s)");
				st.phase = Phase::Restoring;
				continue;
			}
			return false;
		}

		case Phase::Restoring: {
			if (st.windowMinimized && st.host) {
				ShowWindow(st.host, SW_RESTORE);
				HostLog("[selftest-stream] host window restored");
			}
			st.phase = Phase::WriteSummary;
			continue;
		}

		case Phase::WriteSummary: {
			WriteSummary(st);
			st.phase = Phase::Finished;
			continue;
		}

		case Phase::Finished:
			return true;
		}
	}
}

int ObsBootstrap::PerfReproSelfTestExitCode()
{
	return g_state.exitCode < 0 ? 0 : g_state.exitCode;
}
