#ifndef OBS_MULTISTREAM_FRONTEND_BRIDGE_HPP_
#define OBS_MULTISTREAM_FRONTEND_BRIDGE_HPP_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "include/cef_browser.h"
#include "include/wrapper/cef_message_router.h"

// The JS<->C++ bridge: a typed request/response surface plus server->client
// event push, the contract the Svelte UI sits on (4.1.5+).
//
// Request/response (JS->C++): the renderer-side CefMessageRouter sends an
// envelope JSON {"method":<name>,"params":<any>}; ObsQueryHandler dispatches it
// through a method registry and answers Success(<result-json>) / Failure(code,
// msg). Methods are data, not branches: adding one is a single registry
// insertion in bridge.cpp.
//
// Event push (C++->JS): Bridge::EmitEvent posts to the CEF UI thread and
// broadcasts window.__obsEmit(name, payload) to every registered browser's main
// frame, which the JS client (obs-bridge.js) fans out to obs.on() subscribers.
// Browsers register via Bridge::AddBrowser / drop via Bridge::RemoveBrowser;
// emits no-op safely before any browser exists or after all are gone. With a
// single registered browser this is behavior-identical to a single-target emit.
struct CanvasDefinition;
struct obs_source;
typedef struct obs_source obs_source_t;

namespace Bridge {

using json = nlohmann::json;

// Build the method registry. Idempotent; call once during bootstrap. Safe to
// call before the UI browser exists.
void Init();

// Tear down the bridge (stop async workers, hubs, and pollers). Call during
// teardown while libobs is still up (before obs_shutdown), on the UI thread.
void Shutdown();

// Register / unregister a live browser as an EmitEvent target. Called from the
// CEF UI thread (Client life-span callbacks). EmitEvent broadcasts to ALL
// registered browsers so a state change in one window updates every window.
void AddBrowser(CefRefPtr<CefBrowser> browser);
void RemoveBrowser(CefRefPtr<CefBrowser> browser);

// Dispatch one envelope. Returns true on success (result populated), false on
// failure (error populated with a human-readable message). Runs on the browser
// UI thread (the message-router callback thread).
bool Dispatch(const std::string &method, const json &params, json &result, std::string &error);

// Try the deferred-callback async lane (Phase 8b). If `method` is registered as an
// async method, hand off the ref-counted `callback` to its off-thread handler and
// return true -- the callback resolves later, on the UI thread, after the network
// work completes (same request/response contract as Dispatch, just non-blocking).
// Returns false if `method` is not async, so the caller falls through to the
// synchronous Dispatch. Runs on the browser UI thread.
bool DispatchAsync(const std::string &method, const json &params,
		   CefRefPtr<CefMessageRouterBrowserSide::Callback> callback);

// Fan a server-push event to JS. Thread-safe: posts to TID_UI if not already
// there. payload is any JSON value (object/array/scalar/null).
void EmitEvent(const std::string &name, const json &payload);

// Render `renderFn` into an outW*outH BGRA texture (ortho'd to srcW*srcH source
// units, so an out smaller than src downscales on the GPU) and return the packed
// pixels, then encode those pixels as a PNG in memory. Exposed for the history
// thumbnail sampler; both run synchronously inside one obs graphics block and must
// be called on the UI thread.
bool RenderToBgraPixels(uint32_t srcW, uint32_t srcH, uint32_t outW, uint32_t outH,
			const std::function<void()> &renderFn, bool opaqueBackground, std::vector<uint8_t> &pixels,
			std::string &errOut);
bool EncodePngMemory(const uint8_t *pixels, uint32_t w, uint32_t h, std::vector<unsigned char> &out,
		     std::string &errOut);

// Record metadata at the moment it is accepted by a platform, keyed by stream
// profile uuid. The go-live prelude is the only caller that matters; a session
// opened moments later reads back what was actually sent, which is not the same
// as what the profile says by the time anyone looks at the history.
//
// A record belongs to one go-live attempt and nothing expires it, so an attempt that
// ends without a session MUST drop what it recorded -- see ForgetSentMetadata.
//
// Thread-safe: written from both the async method lane (streamMeta.set) and the
// prelude's worker, read and taken on the UI thread. Taking clears the entry.
void RecordSentMetadata(const std::string &profileUuid, const json &fields);
// Peek without consuming: the go-live prelude asks whether a platform has already
// accepted this destination's metadata, and the session that will consume the entry
// has not opened yet.
bool HasSentMetadata(const std::string &profileUuid);
// Drop records for an attempt that will not open a session -- the prelude cancelled or
// refused, or the modal abandoning the go-live after a partial push or a refused start.
// Nothing would consume these, and left in place they make the prelude skip those
// destinations on every later go-live, which is the exact push a scheduled start
// depends on.
void ForgetSentMetadata(const std::vector<std::string> &profileUuids);
// The stop edge's sweep: a session that opened consumed its own entries, so anything
// still here belongs to a destination that never came up.
void ClearSentMetadata();
json TakeSentMetadata(const std::string &profileUuid);

// Register an in-process consumer of the 1 Hz stats tick. Called on the CEF UI
// thread with the same snapshot pushed to JS, so a consumer never takes a
// sample of its own -- the encode counters are rebased against shared mutable
// baselines and a second sampler would split the deltas.
//
// One slot, set at startup and cleared at shutdown. Pass nullptr to clear.
void SetStatsTickObserver(std::function<void(const json &)> observer);

// Boot-time reclaim: remove every stored OAuth account that no stream profile
// references (an orphan stranded when its owning profile was deleted before the delete
// path cleaned up accounts). Runs the shared account teardown per orphan and pushes
// oauth.status. Call once at bootstrap, after the profile + account stores load and the
// provider registry is populated, before the chat/events hubs start. UI thread only.
void ReconcileOrphanedAccounts();

// Launch-time self-heal: seed "server=auto" (and, if the key itself is missing,
// re-fetch it) for rtmp_common stream profiles that predate the OAuth connect
// flow's key writeback, so a stale profile's obs_output_start no longer silently
// bails. Only touches rtmp_common profiles linked to a currently connected account;
// custom-RTMP/WHIP and YouTube profiles are unaffected. The key-present case heals
// inline; a missing key is re-fetched on a detached background worker. Call once,
// on the UI thread, after the profile + account stores load and the provider
// registry is populated (same seam as ReconcileOrphanedAccounts).
void SelfHealStreamCredentials();

// Push the current OAuth account connection state as the "oauth.status" event, so a
// state change discovered off the UI thread (e.g. the broker rejecting a refresh
// token as dead) reaches the account chips without waiting for the next poll. Safe
// to call from any thread; marshals to TID_UI and no-ops after teardown.
void EmitOAuthStatus();

// Apply a Default-canvas definition's resolution/color to the global/main video
// pipeline (obs_reset_video, preserving the non-color fields and re-letterboxing
// the preview + resizing the program transition). Injected into CanvasService as
// its GlobalVideoApplier so the domain layer owns the ordering while the bridge
// keeps the preview/transition side-effects. Returns false + sets `error` on a
// failed reset (the config is rolled back first). Runs on the UI thread.
bool ApplyDefaultCanvasVideo(const CanvasDefinition &desired, std::string &error);

// The reverse direction: copy the live global video config (base size, output size,
// fps) onto the Default canvas def. The Default def is what the pipeline is rebuilt
// from on the next launch and what canvas.list reports, so a global reset that skips
// this leaves canvases.json describing a pipeline that no longer exists. Returns true
// when a field actually changed, in which case the def was saved; the caller owns
// whatever its own path needs next (an event emit, an encoder-cache drop). Runs on
// the UI thread.
bool MirrorGlobalVideoToDefaultCanvas();

// Announce a change to a SOURCE OBJECT (as opposed to one canvas's scene item) to every
// canvas currently listing it, as "sceneItems.changed" on each. A source is shared by
// every scene that holds it across every canvas and a dock row addresses it by NAME, so a
// canvas-scoped emit would leave the other canvases holding a name that no longer
// resolves. Bridge scope rather than file-local because the overlay viewport follow
// (Overlay::CommitForSource) announces the same thing from outside this TU. UI thread.
void EmitSceneItemsChangedForSource(obs_source_t *src);

// Has Bridge::Shutdown() begun? The same latch the stats sampler probes, exposed so a
// delayed task owned by another subsystem can make the identical bail. A task that ran
// past this point would touch libobs state Stop() has already freed.
bool IsShuttingDown();

// Push the current multistream output statuses as the "multistream.changed"
// event. Wired as the engine's onStatusChanged; safe to call off the UI thread
// (EmitEvent marshals to TID_UI).
void EmitMultistreamChanged();

// Push the current transport health rows as the "transports.healthChanged" event.
// Wired as Transports::Health().onChanged; safe to call off the UI thread (it
// marshals to TID_UI before reading the aggregator snapshot). Mirrors
// EmitMultistreamChanged.
void EmitTransportsHealthChanged();

// Push the virtual-camera state ({active, canvas}) as the "virtualCam.changed"
// event. Wired as the VirtualCamManager's onChanged; safe to call off the UI
// thread (it marshals to TID_UI before reading the manager + emitting).
void EmitVirtualCamChanged();

// Push the coalesced per-source audio levels as the "audio.levels" event. Called
// by the AudioMonitor's volmeter callback FROM THE AUDIO THREAD; this marshals to
// TID_UI, throttles to ~30 Hz, then snapshots the monitor's levels and builds the
// payload on the UI thread (the 4.4.4 cross-thread-read discipline).
void EmitAudioLevels();

// Push "audio.changed" so the UI re-lists when the active audio source set
// changes. Safe to call off the UI thread (EmitEvent marshals to TID_UI).
void EmitAudioChanged();

// Switch the DEFAULT (global channel-0) program scene to the scene with source
// uuid `sceneUuid`, running the SAME seam scenes.setCurrent uses: the program
// transition (Transitions::SetProgramScene) + ApplyCanvasSceneLinks (so linked
// additional-canvas scenes follow) + scenes.changed + collection save. Returns
// false if the uuid no longer resolves to a scene. UI thread only. Shared by
// scenes.setCurrent and the per-scene switch hotkeys so neither drives a raw
// channel-0 bind that would skip the canvas scene links.
bool SwitchDefaultProgramScene(const std::string &sceneUuid);

// The WHOLE-STREAM lifecycle, shared by streaming.start/stop, the tray menu's Start
// all / Stop all, and the start/stop hotkeys, so no entry point can drive a bare
// MultistreamEngine::StartAllEnabled/StopAll again. The tray and the hotkeys used to do
// exactly that, which meant a stop through either never stopped the live-only chat
// transports or the viewer poller and never dropped the platforms' active-broadcast
// state: the poller then kept spending one YouTube videos.list per cached broadcast
// every cycle, against a stream that had ended, until the process exited.
//
// Both marshal onto the UI thread (AsyncTask::PostToUi runs INLINE when the caller is
// already there, so a bridge method still completes its sequence before returning) and
// are safe from the libobs hotkey thread and a win32 menu handler. Every step is
// idempotent and they serialize on the one UI task queue, so a double hotkey press or a
// tray stop racing a bridge stop is a no-op the second time through.
void StartStreamingAll();
void StopStreamingAll();

// StartStreamingAll for a caller that has no way to ask the user whether a due
// schedule entry should take over the broadcast: the go-live hotkey, the tray's
// Start all, and a bridge go-live with "Ask for stream info" turned off. Loads the
// imminent armed entry's destinations and metadata into the go-live path (see
// ScheduleRunner::AdoptImminentArmed) before starting, so a go-live during that
// entry's arm window claims it -- the plan the calendar shows is the one that
// actually airs -- rather than starting outputs against whatever was configured
// before and leaving the entry to settle as missed under a broadcast that was, in
// fact, it. The Go Live modal does NOT go through here: what the user just
// configured on screen is what airs, never overridden by a schedule behind it.
//
// Adoption and the start it belongs to run as ONE posted task on the UI thread, not
// two. Posted at all because ScheduleRunner is UI-thread-only (like the engine state
// the prelude reads) and the go-live hotkey runs on the libobs hotkey thread; posted
// ONCE because applyEntry (inside adoption) flips the output bindings that
// CollectBroadcastPrelude reads, so anything landing in between goes live against a
// routing nothing prepared for it. As two enqueues an unrelated start could interleave:
// it would collect the old bindings under the new routing, and its own start would then
// be swallowed as a duplicate prelude, leaving this entry's destinations streaming with
// no broadcast created and no metadata pushed.
//
// Adoption is skipped outright while a prelude is in flight: that start is dropped,
// so rewriting the routing for it would only hand the running prelude a set of
// destinations it prepared nothing for.
void StartStreamingAllAdoptingSchedule();

// True while a go-live prelude is running. StartStreamingAll silently drops a start
// issued in that window (only one prelude may be in flight), which is right for the
// hotkey and the tray but wrong for a caller that reports success back to the user --
// see MethodStreamingStart. Ask before starting if you can surface a refusal. UI
// thread only.
bool GoLivePreludeInFlight();

// Holds the flag above up for as long as it lives, so the headless self-tests can prove
// the refusals that key off it. They cannot reach a real prelude: one is collected only
// for an account-backed destination, and driving a go-live to raise the flag would create
// broadcasts on -- and then stream to -- whatever the machine actually has connected. UI
// thread only.
class ScopedGoLivePrelude {
public:
	ScopedGoLivePrelude();
	~ScopedGoLivePrelude();
	ScopedGoLivePrelude(const ScopedGoLivePrelude &) = delete;
	ScopedGoLivePrelude &operator=(const ScopedGoLivePrelude &) = delete;

private:
	bool previous;
};

// Flip one output binding's enabled flag and run everything that has to follow it:
// the single-live-stream refusal, the go-live-prelude refusal, the persist, stopping an
// output that just lost its binding, the canvas-mix reconcile, and the
// outputBinding.changed push. Shared by outputBinding.setEnabled and the schedule
// runner's arm step, so a scheduled entry cannot route itself through a bare
// `enabled = x` that skips half of that tail. Returns false + `error` on a refusal or a
// failed persist. UI thread only.
//
// Arming while a session is running starts NOTHING. The destination has to be prepared
// against its platform before an encoder may attach to it, and that preparation is gated
// on the user having validated its stream info -- multistream.startOutput is what runs it.
bool SetOutputBindingEnabled(const std::string &bindingUuid, bool enabled, std::string &error);

// Write/read a single string value under `key` to/from a MultistreamBasicPath
// JSON file (atomic, with a .bak). Shared by the per-feature key/value stores
// (audio_devices.json / theme.json / layout.json / transitions.json).
bool WriteJsonString(const char *file, const char *key, const std::string &value);
std::string ReadJsonString(const char *file, const char *key);

} // namespace Bridge

// Browser-side query handler for the window.cefQuery() bridge. Parses the
// envelope, dispatches through Bridge::Dispatch, and answers the callback. Runs
// on the browser-process UI thread.
class ObsQueryHandler : public CefMessageRouterBrowserSide::Handler {
public:
	bool OnQuery(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int64_t query_id,
		     const CefString &request, bool persistent, CefRefPtr<Callback> callback) override;
};

#endif // OBS_MULTISTREAM_FRONTEND_BRIDGE_HPP_
