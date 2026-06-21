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
class CanvasStore;

namespace ObsBootstrap {
bool Start();

// The global native-multistream canvas model, owned by the bootstrap (loaded in
// Start, cleared in Stop). Exposed so the bridge can serve canvas CRUD over it.
// Valid between Start() and Stop().
CanvasStore &Canvases();

// Re-fire OBS_FRONTEND_EVENT_SCENE_CHANGED through the shim so the loaded UI page
// observes a forwarded obs.event (proves obs->shim->bridge->JS post-load).
void FireSceneChanged();
void TeardownScene();
// Headless proof for 4.3.2: drive properties.get/properties.set on the default
// color source through the bridge and log the round-trip. Gated by the caller to
// the smoke path; no-op effect on normal runs (it only reads/restores).
void RunPropertiesSelfTest();
// Headless proof for 4.3.4: select the first scene-item via the same entry point
// the bridge uses, prove hit-testing returns its id at its center, then exercise
// the move math (offset pos, then restore). Gated by the caller to the smoke
// path; leaves no visible change.
void RunPreviewEditSelfTest();
// Headless proof for 4.3.5: round-trip settings.getVideo/setVideo (to 1280x720@30
// and back) and settings.getAudio/setAudio (to 44100 and back) through the
// bridge, verifying the preview display survives the video reset. Gated by the
// caller to the smoke path; restores the original config so the run leaves no
// change.
void RunSettingsSelfTest();
// Headless proof for 4.4.1: drive canvas.list / encoderTypes.list and a
// canvas.create+update+remove round-trip through the bridge, confirming each
// persists to canvases.json and restores. Also exercises an encoder
// properties.get when wired. Gated by the caller to the smoke path; restores the
// user's file unchanged.
void RunCanvasBridgeSelfTest();
// Headless proof for 4.4.0: round-trip the multistream model stores. Add a
// canvas + a stream profile, Save, reload into a fresh store, confirm each
// persisted, then Remove + Save to restore the user's real files unchanged.
// Gated by the caller to the smoke path.
void RunMultistreamModelSelfTest();
void Stop();
} // namespace ObsBootstrap

#endif // OBS_MULTISTREAM_FRONTEND_OBS_BOOTSTRAP_HPP_
