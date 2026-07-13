#ifndef OBS_MULTISTREAM_FRONTEND_PERF_REPRO_SELFTEST_HPP_
#define OBS_MULTISTREAM_FRONTEND_PERF_REPRO_SELFTEST_HPP_

#include <windows.h>

// Automated perf-repro self-test: the regression gate for the background
// power-throttling opt-out in main.cpp's wWinMain -- the
// SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, ...) call
// that clears PROCESS_POWER_THROTTLING_EXECUTION_SPEED /
// PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION so Windows cannot drop the
// process into Efficiency Mode (EcoQoS) or revoke its 1ms timer resolution when
// it loses the foreground. Losing that opt-out was the real cause of stream
// render-lag (not the compositor-starvation theory the old harness chased).
//
// Armed by BRAIDCAST_SELFTEST_STREAM=perf-repro (read in main.cpp, parallel to
// FE_SMOKE_QUIT_SECONDS) and driven by a dedicated WM_TIMER -- NOT the
// FE_SMOKE_QUIT_SECONDS smoke suite. It is network-free and deterministic: it
// minimizes the host window to simulate losing the foreground, then samples
// render pacing off the stats bridge for a fixed window. No YouTube/OAuth/go-live
// path is involved. Declared in its own header rather than growing
// obs_bootstrap.hpp's smoke-suite list: this feature has its own trigger,
// multi-tick cadence, and process exit-code contract, independent of that suite.
namespace ObsBootstrap {

// Reset the state machine and read BRAIDCAST_SELFTEST_DURATION (background
// measurement window, seconds) / BRAIDCAST_SELFTEST_MAXLAG (max allowed
// renderLagPct during that window) from the environment. `host` is the shell HWND
// created in wWinMain, threaded in so later phases can ShowWindow(SW_MINIMIZE) it
// (to simulate losing the foreground) and SW_RESTORE it afterward. Call exactly
// once, before the first RunPerfReproSelfTest() tick, only when
// BRAIDCAST_SELFTEST_STREAM=perf-repro (the caller gates the arm + the SetTimer on
// that env var itself).
void ArmPerfReproSelfTest(HWND host);

// Advance the state machine by one step. Call on every fire of the caller's
// perf-repro WM_TIMER once armed. A no-op returning true if never armed.
// Returns true once the whole flow has terminated (summary written, the host
// window restored if it was minimized) -- the caller should then KillTimer +
// PostMessageW(WM_CLOSE) to exit.
bool RunPerfReproSelfTest();

// The flow's terminal process exit code once RunPerfReproSelfTest() has
// returned true: 0 PASS, 1 FAIL, 2 skip (reserved -- currently unreachable by
// this state machine, kept for contract stability), 3 infra error. 1 now covers
// either the opt-out being missing/broken OR render pacing degrading past the
// maxLagPct threshold while backgrounded. 3 covers the GetProcessInformation
// query itself failing. 0 before the flow finishes.
int PerfReproSelfTestExitCode();

} // namespace ObsBootstrap

#endif // OBS_MULTISTREAM_FRONTEND_PERF_REPRO_SELFTEST_HPP_
