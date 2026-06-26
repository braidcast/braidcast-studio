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
class StreamProfileStore;
class OutputBindingStore;
class MultistreamEngine;
class CanvasRuntime;
class AudioMonitor;
class SceneCollections;

namespace ObsBootstrap {
bool Start();

// The scene-collection registry (per-collection scene sets, persisted to
// scene_collections.json + one scene file each). Owned by the bootstrap (loaded
// + migrated in Start, cleared in Stop). Exposed so the bridge can serve
// collection CRUD over it and scene_persistence can target the active file.
// Valid between Start() and Stop().
SceneCollections &SceneCollections();

// The global native-multistream canvas model, owned by the bootstrap (loaded in
// Start, cleared in Stop). Exposed so the bridge can serve canvas CRUD over it.
// Valid between Start() and Stop().
CanvasStore &Canvases();

// The global stream-profile registry (reusable destination credentials, persisted
// to streams.json). Owned by the bootstrap; exposed so the bridge can serve
// stream-profile CRUD over it. Valid between Start() and Stop().
StreamProfileStore &StreamProfiles();

// The global output-binding registry (profile x canvas routing edges, persisted
// to output_bindings.json). Owned by the bootstrap; exposed so the bridge can
// serve output-binding CRUD over it. Valid between Start() and Stop().
OutputBindingStore &OutputBindings();

// The encode-once / fan-out streaming engine, owned by the bootstrap
// (constructed in Start after the stores load, torn down in Stop before they
// clear). Exposed so the bridge can drive streaming + report live status over
// it. Valid between Start() and Stop().
MultistreamEngine &Multistream();

// The live obs_canvas_t mixes for the additional (non-Default) canvases, owned by
// the bootstrap (built in Start after the model loads, torn down in Stop before
// the stores clear). Exposed so the canvas CRUD bridge methods can keep the mixes
// in sync with the definitions. Valid between Start() and Stop().
CanvasRuntime &CanvasRuntime();

// The audio mixer's per-source fader/volmeter manager, owned by the bootstrap
// (built in Start after the default scene + modules, torn down in Stop BEFORE
// obs_shutdown with its volmeter callbacks removed first). Exposed so the audio.*
// bridge methods + the throttled audio.levels emit can read/write over it. Valid
// between Start() and Stop().
AudioMonitor &AudioMonitor();

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
// Headless proof for 4.4.2: drive streamProfile.list / serviceTypes.list and a
// streamProfile.create+update+setPrimary+remove round-trip through the bridge,
// confirm each persists to streams.json and restores, that the duplicate guard
// rejects a clashing create, and that properties.get(kind:"service") returns
// descriptors. Gated by the caller to the smoke path; restores the user's file
// unchanged.
void RunStreamProfileBridgeSelfTest();
// Headless proof for 4.4.3: drive outputBinding.list and an
// outputBinding.create+setEnabled+update+remove round-trip through the bridge,
// confirming each persists to output_bindings.json and restores, that the
// list joins profile/canvas uuids to display names, that the duplicate guard
// rejects a clashing create, and that AnyEnabledForCanvas reflects the toggle.
// Gated by the caller to the smoke path; restores the user's file unchanged.
void RunOutputBindingBridgeSelfTest();
// Headless proof for 4.4.0: round-trip the multistream model stores. Add a
// canvas + a stream profile, Save, reload into a fresh store, confirm each
// persisted, then Remove + Save to restore the user's real files unchanged.
// Gated by the caller to the smoke path.
void RunMultistreamModelSelfTest();
// Headless proof for 4.4.4: drive the fan-out engine end-to-end without a real
// broadcast. Create a temp profile (dead local RTMP host) + a temp enabled
// binding on the Default canvas in the in-memory stores, StartOutput, log the
// result + IsCanvasLive + first status, StopOutput, assert it left the live set,
// then remove the temp profile/binding so the model returns to baseline. Never
// Saves, so the user's files are untouched. Gated by the caller to the smoke
// path.
void RunMultistreamEngineSelfTest();
// Headless proof for 4.4.5a: bring up an additional canvas's live obs_canvas_t mix
// via the runtime, assert the uuid is preserved and VideoFor returns the mix, then
// drive StartOutput on a temp enabled binding pointed at it (which can only succeed
// because the mix now exists) and confirm IsCanvasLive flips for that canvas.
// Removes the temp canvas/profile/binding from the in-memory stores afterward;
// never Saves, so the user's files are untouched. Gated by the caller to the smoke
// path.
void RunCanvasRuntimeSelfTest();
// Headless proof for 4.4.5b sub-phase A: bring up a temporary additional canvas,
// then drive the canvas-scoped scene/source bridge path (scenes.create/list/
// setCurrent + sources.create with a `canvas` uuid) and assert the new scene is
// listed in the canvas yet ISOLATED from the global scene list, and that a source
// added to the canvas lands in the canvas's current scene, not output 0. Removes
// the temp canvas afterward; never Saves, so the user's files are untouched. Gated
// by the caller to the smoke path.
void RunCanvasSceneSelfTest();
// Headless proof for 4.4.5b sub-phase B: bring up an additional canvas with a live
// mix + a source in its current scene, address its preview surface by uuid, and
// drive a hit-test + a select + a move on it. Assert the edit lands on the
// additional surface's OWN state (selection flips, item moves) while the Default
// surface's selection and the global output-0 scene are untouched -- i.e. no
// cross-surface bleed. Removes the temp canvas afterward; never Saves. Gated by
// the caller to the smoke path.
void RunPreviewSurfaceIsolationSelfTest();
// Headless proof for the audio mixer: drive audio.list, a setDeflection
// round-trip, and a setMuted round-trip through the bridge, and confirm a
// volmeter attaches to a real audio source. The default color source has no
// audio, so this creates a temporary audio-capable source (added to output 0's
// scene so it activates), rebuilds the monitor, exercises the methods, then
// removes the temp source + restores state. Gated by the caller to the smoke path.
void RunAudioMixerSelfTest();
// Headless proof for native projectors: open a WINDOWED PROGRAM projector via the
// manager directly (no monitor needed), confirm it got a live obs_display, then
// close it, leaving no state behind. Also logs the monitor count. Gated by the
// caller to the smoke path.
void RunProjectorSelfTest();
// Headless proof for hotkeys: drive hotkeys.list (expect > 0, including the
// frontend Start/Stop Streaming), then a hotkeys.set -> hotkeys.list round-trip on
// one hotkey (bind Ctrl+Shift+F12 via a {code:"F12",ctrl,shift} call, confirm it
// reads back), then clear it so the run leaves the binding as it found it. Gated by
// the caller to the smoke path.
void RunHotkeysSelfTest();
// Headless proof for the stats snapshot: drive stats.get through the bridge and
// confirm general.fps / general.cpu are present and 'outputs' is an array sized to
// the enabled-binding count. Live bitrate/drop numbers need a real broadcast, so
// this only verifies shape + that the general stats populate. Gated by the caller
// to the smoke path.
void RunStatsSelfTest();
// Headless proof for the embedded MCP server: spin up a test McpServer (in-process
// HandleRequest, no real socket), then drive initialize / tools/list / a
// tools/call obs_call(scenes.list) and assert the round-trips, plus a go-live
// gating check (allowGoLive=false blocks multistream.startOutput before it
// executes). Touches no config file (StartForTest does not Save). Gated by the
// caller to the smoke path.
void RunMcpSelfTest();
void Stop();
} // namespace ObsBootstrap

#endif // OBS_MULTISTREAM_FRONTEND_OBS_BOOTSTRAP_HPP_
