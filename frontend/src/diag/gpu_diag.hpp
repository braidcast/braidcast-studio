#pragma once

// Mid-session GPU-load diagnostics for pinning down the CEF GPU-process crash
// that gpu_safe_mode cannot catch: gpu_safe_mode only detects a GPU that never
// paints at boot, whereas this observes a GPU that dies mid-session after a clean
// first paint (crash-loops under go-live load). Two independent, env-gated
// facilities, both default OFF so they cost nothing unless explicitly enabled:
//
//   BRAIDCAST_GPUDIAG=1              periodic "[gpudiag]" sampler thread: on each
//                                    tick logs the count + WxH of every obs-browser
//                                    OSR source, the number of live outputs, and
//                                    process resident memory, plus edge-detected
//                                    go-live / stream-stop markers. Correlate the
//                                    timeline against the CEF debug.log crash
//                                    timestamps (see main.cpp CefSettings.log_file).
//   BRAIDCAST_GPUDIAG_INTERVAL_MS=n  override the 5000ms sample interval (min 250).
//   BRAIDCAST_DISABLE_BROWSER_SOURCES=1
//                                    A/B kill switch: neutralize every obs-browser
//                                    OSR source (blank about:blank 16x16 page) so a
//                                    live run carries no browser-source GPU raster
//                                    load -- isolates "is it the OSR browser sources
//                                    loading the GPU, or the encode load alone?".
//                                    Non-persistent: the host skips the teardown
//                                    scene-collection save so the blanked settings
//                                    never reach disk (this run does not save scene
//                                    edits).
//
// Everything routes through the existing HostLog/blog seam tagged "[gpudiag]" --
// no new logger and no new LogCat category (log.hpp is not touched).

namespace GpuDiag {

// True when BRAIDCAST_DISABLE_BROWSER_SOURCES is set (resolved once). The host
// reads this to skip the teardown scene-collection save so neutralized
// browser-source settings never persist.
bool BrowserSourcesDisabled();

// Arm the browser-source kill switch when BRAIDCAST_DISABLE_BROWSER_SOURCES is
// set; a no-op otherwise. Sweeps sources already loaded (the initial scene
// collection is loaded inside ObsBootstrap::Start, before this runs) and connects
// the global "source_create" signal so every later creation is neutralized too.
// Call once after ObsBootstrap::Start() (obs_get_signal_handler valid). Idempotent.
void InstallBrowserSourceKillSwitch();

// Start the periodic sampler thread when BRAIDCAST_GPUDIAG is set; a no-op
// otherwise (zero cost when off -- no thread is created). Call once after
// ObsBootstrap::Start().
void Start();

// Stop and join the sampler thread and disconnect the source_create hook. MUST
// run before ObsBootstrap::Stop()/obs_shutdown so the sampler never enumerates a
// torn-down engine. Safe to call on early-abort paths where Start never ran.
// Idempotent.
void Stop();

} // namespace GpuDiag
