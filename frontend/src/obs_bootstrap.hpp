#ifndef OBS_MULTISTREAM_FRONTEND_OBS_BOOTSTRAP_HPP_
#define OBS_MULTISTREAM_FRONTEND_OBS_BOOTSTRAP_HPP_

// Brings libobs up inside the CEF-hosted browser process and tears it down.
// 4.1.1 is the shell skeleton: Start() initializes the core, the video pipeline
// (D3D11), and audio. No plugins are loaded yet (that arrives in 4.1.3). Stop()
// reverses it, draining the deferred-destruction queue before obs_shutdown.
namespace ObsBootstrap {
bool Start();
void Stop();
} // namespace ObsBootstrap

#endif // OBS_MULTISTREAM_FRONTEND_OBS_BOOTSTRAP_HPP_
