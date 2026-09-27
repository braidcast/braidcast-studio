#ifndef OBS_MULTISTREAM_FRONTEND_CAPTURE_RATE_SELFTEST_HPP_
#define OBS_MULTISTREAM_FRONTEND_CAPTURE_RATE_SELFTEST_HPP_

#include <windows.h>

// Automated capture-rate self-test, armed by BRAIDCAST_SELFTEST_STREAM=capture-rate.
//
// Drives real monitor captures (one WGC, one DXGI, and a second DXGI sharing the
// first one's duplicator) against a full-screen window that repaints on DwmFlush at
// a chosen cadence, plus a private async source fed by its own producer thread, and
// reads the rows back through stats.get -- the payload the Stats panel and the MCP
// server read. Checks, in order:
//   A  full cadence: display captures deliver >= 0.9 x min(main fps, change rate),
//      and a second DXGI source joining the shared duplicator shows no spike;
//   B  half cadence: about 1/2, locked at 1/2 after grace, never a warning;
//      bursty async input reads "below" while steady 30/s input never does;
//   C  static screen, still and then with the cursor moving over it (the WGC
//      source hidden): DXGI stays <= 5/s (pointer-only updates are not frames),
//      and a deinterlaced async source reads unmeasurable;
//   then a WGC re-show and a WGC->DXGI method switch (grace again, no spike), a
//   save/remove/load of the DXGI source (same uuid, no spike), and the session log
//   line written by SessionBegin/SessionEnd.
//
// The Default canvas is treated as live through the sampler's test override, since
// the reference rate comes only from live canvases and this run must not broadcast.
// It moves the real cursor and covers the primary monitor for about a minute.
namespace ObsBootstrap {

void ArmCaptureRateSelfTest(HWND host);

// One step per timer tick; true once finished (summary written, everything torn down).
bool RunCaptureRateSelfTest();

// 0 PASS, 1 FAIL (including stats.get carrying no captures), 2 SKIP (no DXGI
// duplicator or no WGC on this machine), 3 infra error. 0 before the flow finishes.
int CaptureRateSelfTestExitCode();

} // namespace ObsBootstrap

#endif // OBS_MULTISTREAM_FRONTEND_CAPTURE_RATE_SELFTEST_HPP_
