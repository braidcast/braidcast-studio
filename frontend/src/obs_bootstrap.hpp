#ifndef OBS_MULTISTREAM_FRONTEND_OBS_BOOTSTRAP_HPP_
#define OBS_MULTISTREAM_FRONTEND_OBS_BOOTSTRAP_HPP_

// Brings libobs up inside the CEF-hosted browser process and tears it down.
// Start() initializes the core, the video pipeline (D3D11), and audio, then
// curated-loads obs-browser (frontend owns CEF) and creates a test scene with a
// browser source bound to output channel 0. Stop() reverses it, draining the
// deferred-destruction queue before obs_shutdown.
//
// TeardownScene() releases the test scene + browser source. It MUST be called
// while CEF is still pumpable: BrowserSource::Destroy() only *posts* its
// `delete this` to TID_UI, and ActuallyCloseBrowser posts the CEF close -- both
// drain only via the CEF message loop. The caller (main.cpp) runs this and then
// pumps CefDoMessageLoopWork() before Stop()/CefShutdown().
namespace ObsBootstrap {
bool Start();
// Re-fire OBS_FRONTEND_EVENT_SCENE_CHANGED through the shim so the loaded UI page
// observes a forwarded obs.event (proves obs->shim->bridge->JS post-load).
void FireSceneChanged();
void TeardownScene();
void Stop();
} // namespace ObsBootstrap

#endif // OBS_MULTISTREAM_FRONTEND_OBS_BOOTSTRAP_HPP_
