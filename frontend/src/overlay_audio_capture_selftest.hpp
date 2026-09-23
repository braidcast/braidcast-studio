#ifndef OBS_MULTISTREAM_FRONTEND_OVERLAY_AUDIO_CAPTURE_SELFTEST_HPP_
#define OBS_MULTISTREAM_FRONTEND_OVERLAY_AUDIO_CAPTURE_SELFTEST_HPP_

#include <windows.h>

// Automated overlay-audio self-test: the regression gate for obs-browser's rerouted-audio
// keepalive (AudioKeepaliveScript and the matching subtraction in OnAudioStreamPacket, both in
// plugins/obs-browser/browser-client.cpp). The smoke battery's
// RunOverlayAudioSelfTest proves the settings that route an overlay's audio; this proves the
// audio itself arrives, whole, in the mix.
//
// The defect it gates: CEF only runs a browser's audio capture while the page is audible, and
// tears it down about two seconds after the page goes quiet. Every alert that arrives after a
// pause therefore opens a fresh capture, which attaches only after the sound has started -- and
// the page is muted, so what plays before it attaches is lost outright. A viewer hears the tail
// of the alert. libobs logs one "Timestamp for source ... jumped" per restart.
//
// What it proves, driven rather than asserted: an alertbox overlay source with rerouted audio is
// created against a throwaway widget whose sound is a known clip, and five or more test alerts
// are fired through overlays.test -- the bridge method the editor's Test button calls -- at
// varied gaps of six seconds and up, so an unfixed capture has torn down before every one. The
// case owns one mix track (every other source is taken off it for the run and put back after),
// and that track's final mix is tapped and cross-correlated against the clip. Per alert, PASS
// needs 95% of the clip's energy in the mix, 95% of its loud 0.1-0.4 s onset, the onset reaching
// the mix within a second of the fire, and the quiet stretch before the next alert at or below
// -90 dBFS on each channel of the front pair -- the keepalive's DC is subtracted from both at the
// capture, so that stretch holds only what the subtraction leaves, and a channel it missed fails
// on its own. Overall it needs no timestamp jump for the source after warm-up (the
// proof the capture stayed up between alerts), and the source's own sample count tracking wall
// clock at the mix rate.
//
// Two variants, one state machine:
//   overlay-audio          the overlay's output alone on the track (monitoring off).
//   overlay-audio-monitor  the overlay set to monitor-and-output with its monitor on a named
//                          endpoint, and a Desktop Audio capture of that same endpoint on the
//                          track. libobs's deduplication then silences the overlay's own output,
//                          so the track carries what the monitor actually played -- the chain a
//                          viewer hears when an overlay is monitored to the device Desktop Audio
//                          captures. BRAIDCAST_SELFTEST_ENDPOINT is required and must name a
//                          quiet endpoint, for the reasons loopback-silence gives; the clips
//                          are audible on it while the case runs.
//
// The clip defaults to a synthesized one shaped like a notification sound (fast attack, decay
// over about a second, 1.85 s long). BRAIDCAST_SELFTEST_CLIP names a WAV file to use instead,
// whose first channel is the reference. It accepts only 16-bit PCM or 32-bit float WAV at the
// mix sample rate; anything else -- another bit depth or rate, or a compressed file -- is NOT
// RUN, because the clip is compared sample for sample against what reaches the mix.
// A clip whose onset is lost is aligned by cross-correlation on what survives, so a tonal or
// periodic clip can lock onto the wrong cycle and misreport where the loss fell; the verdict
// still fails, but the per-alert ranges are only trustworthy for the synthesized chirp.
// BRAIDCAST_SELFTEST_ALERTS sets the alert count (default and minimum 5, at most kMaxAlerts, what the
// capture buffers hold -- a larger value is clamped, and the clamp is logged).
// BRAIDCAST_SELFTEST_SPACING_MS replaces the built-in varied gaps with one fixed gap (minimum
// 6000), for probing how the capture behaves after long silences.
// BRAIDCAST_SELFTEST_UI_STALL_MS (default 1000, 0 disables) blocks the UI thread for that long
// right after each fire. CEF decides audibility and attaches its capture on that thread, and on
// air it is busy; at idle an unfixed capture attaches fast enough to keep most of the onset, so
// without the stall the case would not reproduce what viewers hear.
//
// Armed by BRAIDCAST_SELFTEST_STREAM and driven by the shared self-test stream WM_TIMER in
// main.cpp. Every line it logs starts "[selftest-stream] overlay-audio", the verdict included.
namespace ObsBootstrap {

// Reset the state machine for one variant and read the environment. Call exactly once, before
// the first tick. The HWND is unused; every BRAIDCAST_SELFTEST_STREAM mode arms through one
// signature.
void ArmOverlayAudioCaptureSelfTest(HWND host);
void ArmOverlayAudioMonitorSelfTest(HWND host);

// Advance the state machine by one step. Call on every fire of the self-test stream WM_TIMER
// once armed. A no-op returning true if never armed. Returns true once the flow has terminated
// (verdict logged, summary written, and the channel, track masks, monitoring device and widget
// restored).
bool RunOverlayAudioCaptureSelfTest();

// The terminal process exit code once RunOverlayAudioCaptureSelfTest() has returned true: 0 PASS,
// 1 FAIL, 2 SKIP (the monitor variant on a machine with no named render endpoint), 3 NOT RUN (the
// harness could not set the case up, or could not read an artifact the verdict depends on) --
// named by SelfTest::ResultName. Also 3 before the flow finishes, so a run cut short never reads
// as a pass.
int OverlayAudioCaptureSelfTestExitCode();

} // namespace ObsBootstrap

#endif // OBS_MULTISTREAM_FRONTEND_OVERLAY_AUDIO_CAPTURE_SELFTEST_HPP_
