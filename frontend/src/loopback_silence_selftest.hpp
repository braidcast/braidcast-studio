#ifndef OBS_MULTISTREAM_FRONTEND_LOOPBACK_SILENCE_SELFTEST_HPP_
#define OBS_MULTISTREAM_FRONTEND_LOOPBACK_SILENCE_SELFTEST_HPP_

#include <windows.h>

// Automated loopback-silence self-test: the regression gate for win-wasapi's silent
// render keepalive (SilentRenderKeepalive in plugins/win-wasapi/win-wasapi.cpp).
//
// The defect it gates: a Desktop Audio loopback capture receives no packets at all while
// its render endpoint is silent, because Windows stops the endpoint's audio engine when no
// render session is open on it. libobs keeps its own timeline, buffers for its 45-tick cap
// (960 ms at a 48 kHz mix) and then restarts the source's audio, which a viewer hears as
// desktop audio drifting late and
// then being cut.
//
// What it proves, driven rather than asserted: a loopback capture is opened on a named
// render endpoint and left alone for a long window, then judged on four things -- the
// longest stretch of wall clock with no audio callback at all, the received sample count
// held against wall clock at the rate libobs mixes at (the only gate that catches partial
// starvation, where every individual gap stays small but the stream still falls behind), and
// zero occurrences of libobs's two downstream complaints about that source in this session's
// log ("exceeded TS_SMOOTHING_THRESHOLD" and "Restarting source audio"). Those last two are
// why the capture is bound to a global output channel: they come from the mix path.
//
// THE ENDPOINT MUST BE NAMED AND MUST BE QUIET. BRAIDCAST_SELFTEST_ENDPOINT is required, and
// an unset one ends the run at exit 3 rather than picking a default. Any render session on
// the endpoint -- another app's, or this very fix's -- keeps the audio engine running and
// makes a fixed and an unfixed build look identical, so a guessed endpoint would report PASS
// while proving nothing. Measured on the development machine: the default speakers pass on
// both builds, while a second endpoint with nothing playing to it separates them completely
// (unfixed: zero packets; fixed: sample count tracking wall clock to within 0.01%). Every run
// writes its endpoint id into its summary so a green result stays auditable afterwards.
//
// Armed by BRAIDCAST_SELFTEST_STREAM=loopback-silence and driven by the shared self-test
// stream WM_TIMER in main.cpp -- NOT the FE_SMOKE_QUIT_SECONDS smoke suite, whose whole
// battery is far shorter than one useful measurement window here.
namespace ObsBootstrap {

// Reset the state machine and read BRAIDCAST_SELFTEST_DURATION (measurement window,
// seconds) and BRAIDCAST_SELFTEST_MAXGAP (longest tolerated stretch with no packet,
// milliseconds) from the environment. BRAIDCAST_SELFTEST_ENDPOINT, required and read at the
// open, pins the run to the endpoint whose id contains it; the candidate ids are logged.
// Call exactly once, before the first tick. The HWND is unused; it is taken so every
// BRAIDCAST_SELFTEST_STREAM mode arms through one signature.
void ArmLoopbackSilenceSelfTest(HWND host);

// Advance the state machine by one step. Call on every fire of the caller's self-test
// stream WM_TIMER once armed. A no-op returning true if never armed. Returns true once the
// flow has terminated (summary written, capture released, output channel restored) -- the
// caller should then KillTimer + PostMessageW(WM_CLOSE).
bool RunLoopbackSilenceSelfTest();

// The flow's terminal process exit code once RunLoopbackSilenceSelfTest() has returned
// true: 0 PASS, 1 FAIL, 2 SKIP (this machine cannot host the case -- no named render
// endpoint exists, or the capture never opened on the one chosen), 3 NOT RUN (the harness
// was not configured, or an artifact the verdict depends on could not be read: no
// BRAIDCAST_SELFTEST_ENDPOINT, no endpoint matching it, an unreadable session log, or an
// unreadable audio mix sample rate). Also 3 before the flow finishes, so a run that is armed
// and then cut short -- the window closing early, an operator closing it -- reports that it
// never judged rather than reporting a pass.
//
// 3 is deliberately not 0: a run that could not judge itself must never read as a pass.
int LoopbackSilenceSelfTestExitCode();

} // namespace ObsBootstrap

#endif // OBS_MULTISTREAM_FRONTEND_LOOPBACK_SILENCE_SELFTEST_HPP_
