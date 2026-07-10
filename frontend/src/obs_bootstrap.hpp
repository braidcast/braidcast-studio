#ifndef OBS_MULTISTREAM_FRONTEND_OBS_BOOTSTRAP_HPP_
#define OBS_MULTISTREAM_FRONTEND_OBS_BOOTSTRAP_HPP_

#include <string>

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
class SceneLinkStore;
class MultistreamEngine;
class CanvasRuntime;
class CanvasService;
class AudioMonitor;
class SceneCollections;
class UndoManager;
class VirtualCamManager;
class GlobalAudioChannels;
class StreamMetaStore;
struct GeneralSettings;
struct AdvancedSettings;

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

// The per-scene-collection scene-link model (main-scene -> per-canvas scene
// activation map, persisted to scenes/<slug>.scene_links.json). Owned by the
// bootstrap (loaded in Start, cleared in Stop). Exposed so the bridge can serve
// scene-link CRUD over it. Valid between Start() and Stop().
SceneLinkStore &SceneLinks();

// Switch every non-default canvas to the scene linked from `mainSceneUuid`
// (CanvasSceneLink::Resolve). Canvases with no link for this main scene are
// left untouched. Safe to call with an empty/unknown uuid (no-op).
void ApplyCanvasSceneLinks(const std::string &mainSceneUuid);

// Remove links referencing a deleted entity, then persist. Call from the
// scene/canvas removal paths so no dangling link survives.
void PruneSceneLinksForMainScene(const std::string &mainSceneUuid);
void PruneSceneLinksForCanvas(const std::string &canvasUuid);
void PruneSceneLinksForCanvasScene(const std::string &canvasUuid, const std::string &canvasSceneUuid);

// Remove every output binding in the ACTIVE collection that routes the deleted
// stream profile, then persist. Call from the stream-profile removal path so no
// dangling (profile x canvas) edge survives. Returns the number pruned (0 = no
// change, so callers can skip the outputBinding.changed emit). Only the active
// collection's in-memory store is reachable here; bindings in other collections
// live on disk and keep the ProfileLabelFor "(deleted)" fallback until loaded.
size_t PruneOutputBindingsForProfile(const std::string &profileUuid);

// The encode-once / fan-out streaming engine, owned by the bootstrap
// (constructed in Start after the stores load, torn down in Stop before they
// clear). Exposed so the bridge can drive streaming + report live status over
// it. Valid between Start() and Stop().
MultistreamEngine &Multistream();

// Whether the streaming engine is currently constructed (true between Start()
// and Stop()). Lets the deferred multistream.changed emit skip a build that a
// CEF task queued (from an async output-stop signal) before Stop() runs during
// CefShutdown, after the engine was reset.
bool MultistreamAlive();

// The live obs_canvas_t mixes for the additional (non-Default) canvases, owned by
// the bootstrap (built in Start after the model loads, torn down in Stop before
// the stores clear). Exposed so the canvas CRUD bridge methods can keep the mixes
// in sync with the definitions. Valid between Start() and Stop().
CanvasRuntime &CanvasRuntime();

// The canvas update/reconciliation domain service (structural diff, live-refusal
// gate, Default->global-video coupling, commit + reset ordering, inheritor-aware
// encoder invalidation). Owned by the bootstrap (constructed in Start after the
// CanvasRuntime + MultistreamEngine, reset in Stop before them). Exposed so the
// bridge's canvas.update handler can drive it as a thin JSON adapter. Valid between
// Start() and Stop().
CanvasService &CanvasService();

// The audio mixer's per-source fader/volmeter manager, owned by the bootstrap
// (built in Start after the default scene + modules, torn down in Stop BEFORE
// obs_shutdown with its volmeter callbacks removed first). Exposed so the audio.*
// bridge methods + the throttled audio.levels emit can read/write over it. Valid
// between Start() and Stop().
AudioMonitor &AudioMonitor();

// Whether the audio monitor is currently constructed (true between Start() and
// Stop()). Lets the throttled audio.levels emit skip a drain that a CEF task
// queued before Stop() runs during CefShutdown, after the monitor was reset.
bool AudioMonitorAlive();

// The per-scene-collection undo/redo stack (deque-based action history). Owned by
// the bootstrap (cleared in Stop and on scene-collection switch). Exposed so the
// bridge can serve undo.undo/redo/state over it and wire its onChanged to the
// undo.changed event. Valid between Start() and Stop().
UndoManager &Undo();

// The virtual-camera output manager (start/stop the OBS virtual camera, fed from
// a chosen canvas's video). Owned by the bootstrap (Load() in Start after the
// CanvasRuntime is up, Shutdown() in Stop before the canvases it feeds are torn
// down). Exposed so the bridge can serve virtualCam.* over it. Valid between
// Start() and Stop().
VirtualCamManager &VirtualCam();

// The global desktop/mic audio channels (wasapi capture sources on OBS output
// channels 1..6), owned by the bootstrap. SeedOrRestore() runs in Start (before the
// AudioMonitor's first Rebuild), Clear() in Stop (after the monitor teardown, before
// obs_shutdown). Exposed so the audio.* bridge methods can read/set the per-channel
// devices over it. Valid between Start() and Stop().
GlobalAudioChannels &GlobalAudioChannels();

// The remembered stream-metadata store (per-channel defaults keyed by accountId,
// per-stream overrides keyed by profileUuid, persisted to stream_meta.json).
// Owned by the bootstrap; it loads in its ctor, so it is valid for the whole
// process. Exposed so the streamMeta.* bridge methods can read/write over it.
StreamMetaStore &StreamMeta();

// The global "General settings" bag (projector always-on-top, snapping prefs,
// tray/multiview/importer prefs), persisted to general.json. Owned by the
// bootstrap (Load() early in Start). Exposed so the bridge can serve
// settings.getGeneral/setGeneral over it and other systems can read prefs.
// Valid between Start() and Stop().
GeneralSettings &General();

// The global "Advanced settings" bag (process priority, per-output stream delay /
// reconnect / network options, dynamic bitrate, browser HW accel), persisted to
// advanced.json. Owned by the bootstrap (Load() early in Start, where the loaded
// process priority is also applied once). Exposed so the bridge can serve
// settings.getAdvanced/setAdvanced over it and the engine can read per-output
// options at StartOutput. Valid between Start() and Stop().
AdvancedSettings &Advanced();

void TeardownScene();
// Stand up a fresh placeholder Default scene bound to channel 0, owned by libobs's
// source list (NOT tracked by the boot g_scene). Used by the scene-collection
// switch when the target collection has no saved scene file yet, so it comes up
// with a usable scene instead of an empty world.
void CreateDefaultSceneDetached();
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
// Headless proof for scenes.duplicateToCanvas: bring up two temporary additional
// canvases (source + destination), create a scene with one color source on the
// source canvas, then drive scenes.duplicateToCanvas onto the destination canvas.
// Asserts the new scene lands on the destination with exactly one item whose
// source has a DIFFERENT uuid than the original (a real deep copy, not a shared
// ref), that Undo() removes the duplicated scene, and that Redo() restores it
// with the SAME item source uuid captured after the initial duplicate (proving
// restore-from-snapshot rather than a fresh re-duplicate). Removes both temp
// canvases afterward; never Saves explicitly (the bridge calls it drives do their
// own normal Save, matching the other canvas-scene self-tests). Gated by the
// caller to the smoke path.
void RunSceneDuplicateSelfTest();
// Headless proof for the transform pivot/clamp fixes: bring up a temporary
// additional canvas with one wide, off-center, non-uniformly scaled color
// source, then drive setTransform{rot:90} and transformAction{rotate90cw}
// and assert (via a local AABB read mirroring bridge.cpp's GetSceneItemBox)
// that the item's visual center barely moves -- proof rotation pivots around
// the center rather than libobs's default alignment-anchor corner. Also
// drives an extreme pos-only setTransform that would leave the item fully
// off-canvas and asserts the resulting box regains positive overlap with the
// canvas -- proof the off-canvas clamp fired. Removes the temp canvas
// afterward; never Saves. Gated by the caller to the smoke path.
void RunTransformPivotSelfTest();
// Headless proof for the outline/aspect-mismatch-after-rotation investigation
// (transform-ux-fixes plan Task 5): bring up a temporary additional canvas with
// two non-square color-source items -- one plain-scaled (OBS_BOUNDS_NONE), one
// bounds-locked (OBS_BOUNDS_SCALE_INNER) -- each given a known 400x100 footprint,
// then rotate each 90 degrees and re-read its AABB (via the same local box-read
// helper RunTransformPivotSelfTest uses). Asserts the AABB's width/height swap
// within ~1px for BOTH bounds modes, since a 90-degree rotation of a non-square
// box should always flip which axis is longer. This directly tests whether the
// "box footprint" that a selection outline would draw from actually updates
// across rotation for the bounds-locked case, which is the specific hypothesis
// behind the user-reported "outline stays sized for the old orientation" bug.
// Also drives the bounds-locked item through sceneItems.transformAction's
// rotate90cw entry point (the actual "Rotate 90 CW" quick-action), not just
// setTransform, to prove both real rotation entry points agree. Removes the
// temp canvas afterward; never Saves. Gated by the caller to the smoke path.
void RunRotationBoundsSelfTest();
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
// Headless proof for the Phase 9.2a events foundation: Ingest three synthetic
// NormalizedEvents through EventHub -> EventStore -> emit, assert events.list returns
// them newest-first, that a re-Ingest is deduped (count stays 3), and that a fresh
// store round-trips them from events.json. Snapshots + restores the user's real
// events.json (+ .bak) so the smoke run leaves it exactly as found. Gated by the
// caller to the smoke path.
void RunEventSelfTest();
// Overlay-server smoke: bind an ephemeral port, GET an assembled document for an
// injected test widget, open a real SSE client socket, broadcast a synthetic event,
// assert it arrives as a data: frame, then tear the server down. Injected state is
// in-memory only (never persisted), so it leaves overlays.json untouched. Gated by the
// caller to the smoke path.
void RunOverlaySelfTest();
void Stop();
} // namespace ObsBootstrap

#endif // OBS_MULTISTREAM_FRONTEND_OBS_BOOTSTRAP_HPP_
