#ifndef OBS_MULTISTREAM_FRONTEND_PERF_REPRO_SELFTEST_HPP_
#define OBS_MULTISTREAM_FRONTEND_PERF_REPRO_SELFTEST_HPP_

// Automated live perf-repro self-test: the regression gate for the Main-composite
// compositor-starvation incident. Armed by BRAIDCAST_SELFTEST_STREAM=perf-repro
// (read in main.cpp, parallel to FE_SMOKE_QUIT_SECONDS) and driven by a dedicated
// WM_TIMER -- NOT the FE_SMOKE_QUIT_SECONDS smoke suite, since this goes live for
// real on the user's REAL current scene collection + REAL enabled YouTube output
// bindings (forced to broadcast-private only) and must never fire unattended in
// the smoke path. Declared in its own header rather than growing
// obs_bootstrap.hpp's smoke-suite list: this feature has its own trigger,
// multi-tick cadence, and process exit-code contract, independent of that suite.
namespace ObsBootstrap {

// Reset the state machine and read BRAIDCAST_SELFTEST_DURATION / _MAXLAG /
// _COLLECTION from the environment. Call exactly once, before the first
// RunPerfReproSelfTest() tick, only when BRAIDCAST_SELFTEST_STREAM=perf-repro
// (the caller gates the arm + the SetTimer on that env var itself).
void ArmPerfReproSelfTest();

// Advance the state machine by one step. Call on every fire of the caller's
// perf-repro WM_TIMER once armed. A no-op returning true if never armed.
// Returns true once the whole flow has terminated (summary written, every
// mutated side effect torn down) -- the caller should then KillTimer +
// PostMessageW(WM_CLOSE) to exit.
bool RunPerfReproSelfTest();

// The flow's terminal process exit code once RunPerfReproSelfTest() has
// returned true: 0 PASS, 1 FAIL, 2 skip (YouTube not connected / no matching
// enabled binding), 3 provisioning/infra error (collection switch failed,
// force-privacy failed, did not go live within the bound). 0 before the flow
// finishes.
int PerfReproSelfTestExitCode();

} // namespace ObsBootstrap

#endif // OBS_MULTISTREAM_FRONTEND_PERF_REPRO_SELFTEST_HPP_
