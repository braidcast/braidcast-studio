#include "bridge.hpp"
#include "event_names.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#pragma comment(lib, "windowscodecs.lib")

#include <obs.h>
#include <obs.hpp>
#include <obs-frontend-api.h>

#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#include "util/async_task.hpp"
#include "audio/AudioMonitor.hpp"
#include "chat/chat_hub.hpp"
#include "chat/channel_stats_poller.hpp"
#include "chat/viewer_poller.hpp"
#include "chat/ws_client.hpp"
#include "events/event_hub.hpp"
#include "events/transport_health.hpp"
#include "ingest_writeback.hpp"
#include "log.hpp"
#include "overlay/overlay_server.hpp"
#include "overlay/overlay_store.hpp"
#include "mcp/McpServer.hpp"
#include "windowing/interact_window.hpp"
#include "util/json_util.hpp"
#include "obs_bootstrap.hpp"
#include "obs_importer.hpp"
#include "settings/AdvancedSettings.hpp"
#include "settings/DiagnosticsSettings.hpp"
#include "settings/GeneralSettings.hpp"
#include "windowing/preview_window.hpp"
#include "windowing/projector_window.hpp"
#include "util/properties_serializer.hpp"
#include "scene/scene_collections.hpp"
#include "scene/scene_persistence.hpp"
#include "util/session_log.hpp"
#include "scene/transitions.hpp"
#include "windowing/window_manager.hpp"
#include "UndoManager.hpp"

#include "multistream/CanvasRuntime.hpp"
#include "multistream/CanvasService.hpp"
#include "multistream/CanvasStore.hpp"
#include "multistream/GlobalAudioChannels.hpp"
#include "multistream/Hotkeys.hpp"
#include "multistream/MultistreamEngine.hpp"
#include "multistream/OutputBindingStore.hpp"
#include "multistream/SceneLinkStore.hpp"
#include "multistream/StorePaths.hpp"
#include "multistream/StreamMetaStore.hpp"
#include "multistream/StreamProfileStore.hpp"
#include "multistream/VirtualCamManager.hpp"
#include "oauth/provider.hpp"
#include "oauth/registry.hpp"
#include "oauth/account_store.hpp"
#include "overlay/overlay_server.hpp"
#include "overlay/overlay_store.hpp"
#include <util/dstr.h>
#include <util/platform.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <graphics/matrix4.h>

namespace Bridge {

// The single OAuth-account teardown path (defined in the OAuth section below). Shared
// by manual disconnect, profile-delete cleanup, and the boot orphan reconcile; declared
// here so MethodStreamProfileRemove (anonymous namespace, above the definition) can call
// it. Bridge scope so all three call sites resolve to the same function.
void TeardownAccount(const std::string &accountId);

namespace {

// The param-guard helpers (defined with OptString further down) are called by the
// settings handlers above their definition, so forward-declare them here.
static bool RequireStr(const json &params, const char *method, const char *key, std::string &out, std::string &error);
static bool RequireObject(const json &params, const char *method, std::string &error);

// One method: (params) -> (result | error). Returns false and fills `error` on
// failure. Runs on the browser UI thread.
using MethodFn = std::function<bool(const json &params, json &result, std::string &error)>;

std::unordered_map<std::string, MethodFn> g_methods;

// One async method: handed the ref-counted CEF callback to resolve later. The
// handler runs the work off-thread (see RunAsyncMethod) and answers the callback
// back on the UI thread, so TID_UI never blocks on the network. Phase 8b.
using AsyncMethodFn =
	std::function<void(const json &params, CefRefPtr<CefMessageRouterBrowserSide::Callback> callback)>;

// The deferred-callback async lane: methods registered here are dispatched off the
// UI thread instead of through the synchronous g_methods/Dispatch path. The same
// JS `await obs.call(...)` request/response contract holds -- the promise just
// resolves after the off-thread network work completes. 8c/8d's long platform
// calls reuse this lane.
std::unordered_map<std::string, AsyncMethodFn> g_asyncMethods;

// Cleared at Shutdown so an in-flight OAuth device-code poll loop running on a
// detached worker bails instead of polling on after teardown (its emits already
// no-op via the AsyncTask alive-guard; this stops the loop body too).
std::atomic<bool> g_oauthRunning{true};

// Set true at the very top of Shutdown(), before the chat/events hubs are stopped.
// A detached OAuth connect worker that finished authorize() just as teardown began
// would otherwise resurrect a just-stopped hub (StartAccount/Start) -- it probes
// this flag immediately before that block and bails if teardown has begun.
std::atomic<bool> g_bridgeShutdown{false};

// Per-connect cancel signal: oauth.cancelConnect sets it so an in-flight
// device-code poll worker bails when the user closes the connect modal. Reset to
// false when a new oauth.connect starts.
std::atomic<bool> g_oauthCancel{false};

// Monotonic connect generation. Each oauth.connect bumps it and captures the new
// value; a worker only owns the flow while g_oauthGen still equals its captured
// gen. This lets a newer attempt tear down an older worker's wait (folded into the
// cancel probe) and prevents a superseded worker from flipping the UI after a
// newer attempt has started.
std::atomic<uint64_t> g_oauthGen{0};

// All live UI browsers; EmitEvent broadcasts to each. Registered/unregistered on
// the UI thread; read on the UI thread (EmitEvent posts there first). The mutex
// guards the registry against the (off-thread) registration paths and snapshots
// it before iterating so an emit never holds the lock across ExecuteJavaScript.
std::mutex g_browser_mutex;
std::vector<CefRefPtr<CefBrowser>> g_browsers;

// Build the multistream status JSON array (one object per enabled binding) from
// the engine's current Statuses(). The state enum is carried as its lowercase
// name (StateName); the Svelte side maps it to a status-dot color.
json BuildStatusArray()
{
	json arr = json::array();
	for (const MultistreamEngine::OutputStatus &st : ObsBootstrap::Multistream().Statuses()) {
		arr.push_back(json{
			{"bindingUuid", st.bindingUuid},
			{"canvasUuid", st.canvasUuid},
			{"profileLabel", st.profileLabel},
			{"canvasName", st.canvasName},
			{"state", MultistreamEngine::StateName(st.state)},
			{"lastError", st.lastError},
		});
	}
	return arr;
}

// Build the transport health JSON array (one object per reporting transport) from
// the aggregator's current Snapshot(). The state enum is carried as its lowercase
// name (StateName); the Svelte side maps it to a badge. Mirrors BuildStatusArray.
json BuildTransportHealthArray()
{
	json arr = json::array();
	for (const Transports::TransportHealth::Entry &e : Transports::Health().Snapshot()) {
		arr.push_back(json{
			{"id", e.id},
			{"state", Transports::TransportHealth::StateName(e.state)},
			{"lastError", e.lastError},
			{"updatedAt", e.updatedAtMs},
		});
	}
	return arr;
}

// Serialize the undo stack's current state for both undo.state and the
// undo.changed push, so the method and the event report an identical shape.
json UndoStateJson()
{
	const UndoManager::State st = ObsBootstrap::Undo().GetState();
	return json{
		{"canUndo", st.canUndo},
		{"canRedo", st.canRedo},
		{"undoName", st.undoName},
		{"redoName", st.redoName},
	};
}

// Push the current undo state as the "undo.changed" event. Wired as the
// UndoManager's onChanged in Init; runs on the UI thread (the only mutator), so a
// direct EmitEvent suffices.
void EmitUndoChanged()
{
	EmitEvent(EventNames::kUndoChanged, UndoStateJson());
}

// Turn a store save that returned false (disk full / permission -- already logged by
// ReportSaveResult with the path) into the handler's caller-visible failure, so the
// web's window.obs.call rejects instead of resolving on a silently-dropped edit. A
// handler keeps its post-save side effects, then `return PersistOrFail(saved, error)`
// at its existing terminal return: on success (saved==true) `error` is untouched and
// the same true is returned, so the success path is unchanged.
bool PersistOrFail(bool saved, std::string &error)
{
	if (!saved) {
		error = "failed to save the change to disk; it may be lost on restart";
	}
	return saved;
}

// --- method bodies ----------------------------------------------------------

bool MethodGetVersion(const json & /*params*/, json &result, std::string & /*error*/)
{
	const char *version = obs_get_version_string();
	result = version ? std::string(version) : std::string();
	return true;
}

bool MethodGetCurrentScene(const json & /*params*/, json &result, std::string & /*error*/)
{
	// The shim answers obs_frontend_get_current_scene from output channel 0
	// (already addref'd). null when no scene is bound.
	obs_source_t *scene = obs_frontend_get_current_scene();
	if (!scene) {
		result = nullptr;
		return true;
	}
	const char *name = obs_source_get_name(scene);
	result = name ? json(std::string(name)) : json(nullptr);
	obs_source_release(scene);
	return true;
}

bool MethodListScenes(const json & /*params*/, json &result, std::string & /*error*/)
{
	// The frontend-api shim's obs_frontend_get_scenes is an empty stub, so we
	// enumerate scene sources directly via obs_enum_scenes (obs_enum_sources
	// only yields OBS_SOURCE_TYPE_INPUT, not scenes). Returns the 4.1.2 test
	// scene plus any others.
	json scenes = json::array();
	obs_enum_scenes(
		[](void *param, obs_source_t *source) -> bool {
			const char *name = obs_source_get_name(source);
			if (name) {
				static_cast<json *>(param)->push_back(name);
			}
			return true; // keep enumerating
		},
		&scenes);
	result = std::move(scenes);
	return true;
}

bool MethodGetStreamingState(const json & /*params*/, json &result, std::string & /*error*/)
{
	result = json{{"active", ObsBootstrap::Multistream().AnyLive()}};
	return true;
}

// streaming.start/stop drive the fan-out engine: start every enabled binding /
// stop everything. `active` reports whether anything is live afterward; the
// streaming.changed push proves the server->client event end-to-end (the engine
// also pushes multistream.changed per-output via onStatusChanged).
bool MethodStreamingStart(const json & /*params*/, json &result, std::string & /*error*/)
{
	ObsBootstrap::Multistream().StartAllEnabled();
	const bool active = ObsBootstrap::Multistream().AnyLive();
	result = json{{"active", active}};
	EmitEvent(EventNames::kStreamingChanged, result);
	// Chat and the viewer poller are live-only: start them here on go-live so every
	// connected platform's chat transport connects together (YouTube's liveChatId exists
	// only once the broadcast is live). Both Start()s are idempotent.
	if (active) {
		Chat::Hub().Start();
		Chat::Viewers().Start();
	}
	return true;
}

bool MethodStreamingStop(const json & /*params*/, json &result, std::string & /*error*/)
{
	// Chat and the viewer poller are live-only, so tear them down on stop. Reset each
	// provider's active-broadcast target so the next go-live re-resolves YouTube's
	// liveChatId fresh (clearActiveBroadcast is a no-op except on YouTube).
	Chat::Viewers().Stop();
	Chat::Hub().Stop();
	for (const auto &entry : OAuth::Accounts().All()) {
		OAuth::StreamProvider *provider = OAuth::Registry().Get(entry.second.providerId);
		if (provider) {
			provider->clearActiveBroadcast(entry.first); // entry.first == accountId
		}
	}
	ObsBootstrap::Multistream().StopAll();
	result = json{{"active", false}};
	EmitEvent(EventNames::kStreamingChanged, result);
	return true;
}

// Position the native preview overlay over the UI's reported region. params:
// {x,y,w,h} in CSS pixels (host-client space) + dpr (devicePixelRatio). Convert
// CSS px -> device px so the HWND lands exactly on the UI region under any DPI
// scale. Runs on the UI thread (router callback), the same thread that owns the
// HWND, so the SetWindowPos is direct. Lazily creates the overlay on first call.
// Read the optional `canvas` uuid that addresses one preview surface. Absent,
// empty, or the Default canvas uuid all map to the Default surface, so today's
// single-preview caller (which sends no `canvas`) is unchanged.
std::string PreviewCanvasParam(const json &params);

// The originating window id for a preview surface. Absent => 0 (main window), so
// single-window callers (no `window`) address the main window's surfaces. Carried
// so per-window preview surfaces stay keyed by (windowId, canvasUuid) when
// additional windows are introduced.
int PreviewWindowParam(const json &params)
{
	if (params.is_object()) {
		auto it = params.find("window");
		if (it != params.end() && it->is_number_integer()) {
			return it->get<int>();
		}
	}
	return 0; // main window
}

bool MethodPreviewSetRect(const json &params, json & /*result*/, std::string &error)
{
	PreviewManager *pm = Preview::Instance();
	if (!pm) {
		error = "preview not ready";
		return false;
	}
	if (!params.is_object()) {
		error = "setRect expects an object {x,y,w,h,dpr,canvas?}";
		return false;
	}

	auto num = [&](const char *key, double fallback) -> double {
		auto it = params.find(key);
		return (it != params.end() && it->is_number()) ? it->get<double>() : fallback;
	};

	const double dpr = num("dpr", 1.0) > 0.0 ? num("dpr", 1.0) : 1.0;
	const int x = int(num("x", 0.0) * dpr + 0.5);
	const int y = int(num("y", 0.0) * dpr + 0.5);
	const int w = int(num("w", 0.0) * dpr + 0.5);
	const int h = int(num("h", 0.0) * dpr + 0.5);
	const std::string canvas = PreviewCanvasParam(params);
	const int windowId = PreviewWindowParam(params);

	HostLog("[bridge] preview.setRect window=" + std::to_string(windowId) + " dev-px x=" + std::to_string(x) +
		" y=" + std::to_string(y) + " w=" + std::to_string(w) + " h=" + std::to_string(h) +
		" (dpr=" + std::to_string(dpr) + (canvas.empty() ? ")" : ", canvas=" + canvas + ")"));
	pm->SetRect(windowId, canvas, x, y, w, h);
	return true;
}

bool MethodPreviewHide(const json &params, json & /*result*/, std::string &error)
{
	PreviewManager *pm = Preview::Instance();
	if (!pm) {
		error = "preview not ready";
		return false;
	}
	pm->Hide(PreviewWindowParam(params), PreviewCanvasParam(params));
	return true;
}

// Fully tear down a canvas's preview surface (display + overlay HWND), not just
// hide it. The UI calls this when a canvas panel unmounts (the canvas left the
// enabled set), so a disabled canvas's surface does not linger until shutdown.
bool MethodPreviewDestroy(const json &params, json & /*result*/, std::string &error)
{
	PreviewManager *pm = Preview::Instance();
	if (!pm) {
		error = "preview not ready";
		return false;
	}
	pm->Destroy(PreviewWindowParam(params), PreviewCanvasParam(params));
	return true;
}

// --- settings (video / audio) -----------------------------------------------

// Live-change guard: refuse global video/audio resets while any output is live,
// since the global mix backs the Default canvas's encoders and resetting it would
// free the mix out from under them (UAF).
bool AnyOutputActive()
{
	return ObsBootstrap::Multistream().AnyLive();
}

// speaker_layout <-> string. Data-driven so a new layout is one row. The set is
// what obs_audio_info accepts; the UI offers at least mono/stereo.
struct SpeakerName {
	speaker_layout layout;
	const char *name;
};
const SpeakerName kSpeakerNames[] = {
	{SPEAKERS_MONO, "mono"},   {SPEAKERS_STEREO, "stereo"}, {SPEAKERS_2POINT1, "2.1"}, {SPEAKERS_4POINT0, "4.0"},
	{SPEAKERS_4POINT1, "4.1"}, {SPEAKERS_5POINT1, "5.1"},   {SPEAKERS_7POINT1, "7.1"},
};

const char *SpeakerLayoutName(speaker_layout layout)
{
	for (const auto &s : kSpeakerNames) {
		if (s.layout == layout) {
			return s.name;
		}
	}
	return "stereo";
}

bool SpeakerLayoutFromName(const std::string &name, speaker_layout &out)
{
	for (const auto &s : kSpeakerNames) {
		if (name == s.name) {
			out = s.layout;
			return true;
		}
	}
	return false;
}

json VideoInfoToJson(const obs_video_info &ovi)
{
	return json{
		{"baseWidth", ovi.base_width},       {"baseHeight", ovi.base_height}, {"outputWidth", ovi.output_width},
		{"outputHeight", ovi.output_height}, {"fpsNum", ovi.fps_num},         {"fpsDen", ovi.fps_den},
	};
}

bool MethodSettingsGetVideo(const json & /*params*/, json &result, std::string &error)
{
	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi)) {
		error = "video not initialized";
		return false;
	}
	result = VideoInfoToJson(ovi);
	return true;
}

// Read a required positive uint field. Caps at 16384 (the D3D11 max texture
// dimension) so a bad value can't allocate an absurd render target.
bool ReadDimension(const json &params, const char *key, uint32_t current, uint32_t &out, std::string &error)
{
	auto it = params.find(key);
	if (it == params.end()) {
		out = current; // absent -> keep current
		return true;
	}
	if (!it->is_number_integer() && !it->is_number_unsigned()) {
		error = std::string("'") + key + "' must be an integer";
		return false;
	}
	const int64_t v = it->get<int64_t>();
	if (v <= 0 || v > 16384) {
		error = std::string("'") + key + "' must be in 1..16384";
		return false;
	}
	out = uint32_t(v);
	return true;
}

// Reset the global/main video pipeline to the given base/output resolution and
// FPS, preserving the current graphics_module/colorspace/range/format/adapter/
// gpu_conversion/scale_type so only resolution+FPS change. outW/outH default to
// the base size when 0. On a failed reset restores the prior config so video is
// never left broken, then re-letterboxes the preview + resizes the program
// transition. Caller owns the live-active/live-canvas guard.
static bool ApplyGlobalVideo(uint32_t baseW, uint32_t baseH, uint32_t outW, uint32_t outH, uint32_t fpsNum,
			     uint32_t fpsDen, obs_scale_type scaleType, std::string &error,
			     const video_format *fmt = nullptr, const video_colorspace *cs = nullptr,
			     const video_range_type *range = nullptr)
{
	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi)) {
		error = "video not initialized";
		return false;
	}
	const obs_video_info previous = ovi; // for rollback

	ovi.base_width = baseW;
	ovi.base_height = baseH;
	ovi.output_width = outW ? outW : baseW;
	ovi.output_height = outH ? outH : baseH;
	ovi.fps_num = fpsNum;
	ovi.fps_den = fpsDen;
	ovi.scale_type = scaleType;
	// Color fields are only overridden for the Default canvas (which drives the
	// global pipeline); the resolution-only callers pass null and keep the current
	// format/colorspace/range.
	if (fmt) {
		ovi.output_format = *fmt;
	}
	if (cs) {
		ovi.colorspace = *cs;
	}
	if (range) {
		ovi.range = *range;
	}

	// All non-overridden fields are copied from the current config, so if these
	// already match there is genuinely nothing to reset. Skip the full pipeline
	// reset (and its preview flicker) -- this also makes the redundant double-apply
	// on a Settings Cancel a no-op.
	if (ovi.base_width == previous.base_width && ovi.base_height == previous.base_height &&
	    ovi.output_width == previous.output_width && ovi.output_height == previous.output_height &&
	    ovi.fps_num == previous.fps_num && ovi.fps_den == previous.fps_den &&
	    ovi.scale_type == previous.scale_type && ovi.output_format == previous.output_format &&
	    ovi.colorspace == previous.colorspace && ovi.range == previous.range) {
		return true;
	}

	const int rv = obs_reset_video(&ovi);
	if (rv != OBS_VIDEO_SUCCESS) {
		// Restore the prior config so we never leave video in a broken state.
		obs_video_info restore = previous;
		const int rb = obs_reset_video(&restore);
		HostLog("[bridge] ApplyGlobalVideo reset FAILED code=" + std::to_string(rv) +
			", rollback code=" + std::to_string(rb));
		error = "obs_reset_video failed (code " + std::to_string(rv) + ")";
		return false;
	}

	// obs_reset_video just freed and rebuilt the global video mix that the Default
	// canvas's cached encoders were bound to; drop that stale pair so the next start
	// rebuilds it against the new mix (mirrors CanvasService::Update's reset-then-
	// invalidate for runtime canvases). One call covers both the setVideo path and the
	// scene-collection restore, which share this chokepoint. Skipped above on a no-op or
	// failed reset, so an untouched mix keeps its still-valid cache.
	ObsBootstrap::Multistream().InvalidateCanvasEncoders(ObsBootstrap::Canvases().Default().uuid);

	// The obs_display swapchain survives a video reset; just invalidate the cached
	// letterbox transform so the next frame re-letterboxes to the new base size.
	Preview::OnVideoReset();

	// Re-size the program transition on ch0 to the new base canvas so the composited
	// output isn't clipped at the old dimensions until the next type-swap.
	Transitions::OnVideoReset();
	return true;
}

// Rebuild the current obs_video_info, override only the passed fields, and
// obs_reset_video via ApplyGlobalVideo. Refuses while an output is live and on a
// failed reset restores the prior config so video is never left broken. Mirrors
// the applied resolution onto the Default canvas def (canvas.list must reflect
// reality), emits settings.videoChanged + re-validates the preview.
bool MethodSettingsSetVideo(const json &params, json &result, std::string &error)
{
	if (!RequireObject(params, "setVideo", error)) {
		return false;
	}
	if (AnyOutputActive()) {
		error = "cannot change video while an output is active";
		return false;
	}

	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi)) {
		error = "video not initialized";
		return false;
	}

	if (!ReadDimension(params, "baseWidth", ovi.base_width, ovi.base_width, error) ||
	    !ReadDimension(params, "baseHeight", ovi.base_height, ovi.base_height, error) ||
	    !ReadDimension(params, "outputWidth", ovi.output_width, ovi.output_width, error) ||
	    !ReadDimension(params, "outputHeight", ovi.output_height, ovi.output_height, error)) {
		return false;
	}

	auto readFps = [&](const char *key, uint32_t current, uint32_t &out) -> bool {
		auto it = params.find(key);
		if (it == params.end()) {
			out = current;
			return true;
		}
		if (!it->is_number_integer() && !it->is_number_unsigned()) {
			error = std::string("'") + key + "' must be an integer";
			return false;
		}
		const int64_t v = it->get<int64_t>();
		if (v <= 0 || v > 1000) {
			error = std::string("'") + key + "' must be in 1..1000";
			return false;
		}
		out = uint32_t(v);
		return true;
	};
	if (!readFps("fpsNum", ovi.fps_num, ovi.fps_num) || !readFps("fpsDen", ovi.fps_den, ovi.fps_den)) {
		return false;
	}

	if (!ApplyGlobalVideo(ovi.base_width, ovi.base_height, ovi.output_width, ovi.output_height, ovi.fps_num,
			      ovi.fps_den, ovi.scale_type, error)) {
		return false;
	}

	obs_video_info applied = {};
	obs_get_video_info(&applied);

	// Mirror the applied global video onto the Default canvas def so canvas.list
	// (and the Settings UI, which now edits the Default canvas) always reflects the
	// real pipeline even when setVideo is called directly.
	CanvasStore &canvases = ObsBootstrap::Canvases();
	if (CanvasDefinition *def = canvases.Find(canvases.Default().uuid)) {
		if (def->width != applied.base_width || def->height != applied.base_height ||
		    def->fpsNum != applied.fps_num || def->fpsDen != applied.fps_den) {
			def->width = applied.base_width;
			def->height = applied.base_height;
			def->fpsNum = applied.fps_num;
			def->fpsDen = applied.fps_den;
			canvases.Save();
			EmitEvent(EventNames::kCanvasChanged, json::object());
		}
	}

	result = VideoInfoToJson(applied);
	EmitEvent(EventNames::kSettingsVideoChanged, result);
	HostLog("[bridge] settings.setVideo -> " + std::to_string(applied.base_width) + "x" +
		std::to_string(applied.base_height) + " out " + std::to_string(applied.output_width) + "x" +
		std::to_string(applied.output_height) + " @ " + std::to_string(applied.fps_num) + "/" +
		std::to_string(applied.fps_den));
	return true;
}

// Build the full General-settings wire object (camelCase keys) from the struct.
// Shared by settings.getGeneral and the settings.setGeneral response/event so the
// two can't drift; iterates the same descriptor tables the persistence layer uses.
json GeneralToJson(const GeneralSettings &g)
{
	json out = json::object();
	for (const GeneralBoolField &f : kGeneralBoolFields) {
		out[f.json] = g.*f.member;
	}
	for (const GeneralStringField &f : kGeneralStringFields) {
		out[f.json] = g.*f.member;
	}
	for (const GeneralDoubleField &f : kGeneralDoubleFields) {
		out[f.json] = g.*f.member;
	}
	return out;
}

bool MethodSettingsGetGeneral(const json & /*params*/, json &result, std::string & /*error*/)
{
	result = GeneralToJson(ObsBootstrap::General());
	return true;
}

bool MethodSettingsSetGeneral(const json &params, json &result, std::string &error)
{
	if (!RequireObject(params, "setGeneral", error)) {
		return false;
	}
	GeneralSettings &g = ObsBootstrap::General();
	// Apply ONLY present keys of the matching type; unknown keys are ignored.
	for (const GeneralBoolField &f : kGeneralBoolFields) {
		auto it = params.find(f.json);
		if (it != params.end() && it->is_boolean()) {
			g.*f.member = it->get<bool>();
		}
	}
	for (const GeneralStringField &f : kGeneralStringFields) {
		auto it = params.find(f.json);
		if (it != params.end() && it->is_string()) {
			g.*f.member = it->get<std::string>();
		}
	}
	for (const GeneralDoubleField &f : kGeneralDoubleFields) {
		auto it = params.find(f.json);
		if (it != params.end() && it->is_number()) {
			double v = it->get<double>();
			v = v < f.min ? f.min : (v > f.max ? f.max : v);
			g.*f.member = v;
		}
	}
	const bool saved = g.Save();

	// Live-apply the one wired effect: re-pin open projectors' always-on-top.
	if (Projector::Instance()) {
		Projector::Instance()->ApplyAlwaysOnTop(g.projectorAlwaysOnTop);
	}

	result = GeneralToJson(g);
	EmitEvent(EventNames::kSettingsGeneralChanged, result);
	return PersistOrFail(saved, error);
}

// Build the full Advanced-settings wire object (camelCase keys) from the struct.
// Shared by settings.getAdvanced and the settings.setAdvanced response/event so the
// two can't drift; iterates the same descriptor tables the persistence layer uses.
json AdvancedToJson(const AdvancedSettings &a)
{
	json out = json::object();
	for (const AdvancedBoolField &f : kAdvancedBoolFields) {
		out[f.json] = a.*f.member;
	}
	for (const AdvancedStringField &f : kAdvancedStringFields) {
		out[f.json] = a.*f.member;
	}
	for (const AdvancedUIntField &f : kAdvancedUIntFields) {
		out[f.json] = a.*f.member;
	}
	return out;
}

bool MethodSettingsGetAdvanced(const json & /*params*/, json &result, std::string & /*error*/)
{
	result = AdvancedToJson(ObsBootstrap::Advanced());
	return true;
}

bool MethodSettingsSetAdvanced(const json &params, json &result, std::string &error)
{
	if (!RequireObject(params, "setAdvanced", error)) {
		return false;
	}
	// Validate the one enum field up front so a bad token rejects the whole call
	// before any field is mutated.
	auto pp = params.find("processPriority");
	if (pp != params.end() && !pp->is_null()) {
		if (!pp->is_string()) {
			error = "processPriority must be a string";
			return false;
		}
		const std::string v = pp->get<std::string>();
		bool ok = false;
		for (const char *t : kProcessPriorityTokens) {
			if (v == t) {
				ok = true;
				break;
			}
		}
		if (!ok) {
			error = "processPriority must be one of normal, aboveNormal, high, auto";
			return false;
		}
	}

	AdvancedSettings &a = ObsBootstrap::Advanced();
	const std::string oldPriority = a.processPriority;
	const bool oldDisableAudioDucking = a.disableAudioDucking;
	// Apply ONLY present keys of the matching type; unknown keys are ignored.
	for (const AdvancedBoolField &f : kAdvancedBoolFields) {
		auto it = params.find(f.json);
		if (it != params.end() && it->is_boolean()) {
			a.*f.member = it->get<bool>();
		}
	}
	for (const AdvancedStringField &f : kAdvancedStringFields) {
		auto it = params.find(f.json);
		if (it != params.end() && it->is_string()) {
			a.*f.member = it->get<std::string>();
		}
	}
	for (const AdvancedUIntField &f : kAdvancedUIntFields) {
		auto it = params.find(f.json);
		if (it != params.end() && it->is_number()) {
			int64_t v = it->get<int64_t>();
			v = v < (int64_t)f.min ? (int64_t)f.min : (v > (int64_t)f.max ? (int64_t)f.max : v);
			a.*f.member = (uint32_t)v;
		}
	}
	const bool saved = a.Save();

	// Live-apply the one wired side effect: re-pin the process priority when it
	// changed. Stream delay / reconnect / network options are read per output at
	// StartOutput, so they apply to newly started outputs (live ones on restart).
	if (a.processPriority != oldPriority) {
		// UI thread: safe to read AnyLive() directly. "auto" resolves against it now
		// and re-pins on later live transitions via the engine's onLiveStateChanged.
		ApplyEffectivePriority(a.processPriority, ObsBootstrap::Multistream().AnyLive());
	}
	if (a.disableAudioDucking != oldDisableAudioDucking) {
		DisableAudioDucking(a.disableAudioDucking);
	}

	result = AdvancedToJson(a);
	EmitEvent(EventNames::kSettingsAdvancedChanged, result);
	return PersistOrFail(saved, error);
}

bool MethodSettingsGetAudio(const json & /*params*/, json &result, std::string &error)
{
	obs_audio_info oai = {};
	if (!obs_get_audio_info(&oai)) {
		error = "audio not initialized";
		return false;
	}
	const char *monName = nullptr;
	const char *monId = nullptr;
	obs_get_audio_monitoring_device(&monName, &monId);
	result = json{
		{"sampleRate", oai.samples_per_sec},
		{"speakers", SpeakerLayoutName(oai.speakers)},
		{"monitoringDevice", json{{"id", monId ? std::string(monId) : std::string("default")},
					  {"name", monName ? std::string(monName) : std::string("Default")}}},
	};
	return true;
}

bool MethodSettingsSetAudio(const json &params, json &result, std::string &error)
{
	if (!RequireObject(params, "setAudio", error)) {
		return false;
	}

	// Monitoring device is independent of the audio mix and is safe to change
	// while outputs are active, so apply it before the active-output guard.
	if (auto md = params.find("monitoringDevice"); md != params.end() && md->is_object()) {
		const std::string id = md->value("id", std::string());
		const std::string name = md->value("name", std::string());
		if (!id.empty()) {
			obs_set_audio_monitoring_device(name.empty() ? id.c_str() : name.c_str(), id.c_str());
		}
	}

	// Sample rate / channel layout require resetting the audio mix, which fails
	// while audio is active -- only gate (and reset) when one is actually set.
	const bool changeMix = params.contains("sampleRate") || params.contains("speakers");
	if (changeMix) {
		if (AnyOutputActive()) {
			error = "cannot change sample rate or channels while an output is active";
			return false;
		}

		obs_audio_info oai = {};
		if (!obs_get_audio_info(&oai)) {
			error = "audio not initialized";
			return false;
		}

		auto sr = params.find("sampleRate");
		if (sr != params.end()) {
			if (!sr->is_number_integer() && !sr->is_number_unsigned()) {
				error = "'sampleRate' must be an integer";
				return false;
			}
			const int64_t v = sr->get<int64_t>();
			// OBS supports 44100 and 48000; reject anything else.
			if (v != 44100 && v != 48000) {
				error = "'sampleRate' must be 44100 or 48000";
				return false;
			}
			oai.samples_per_sec = uint32_t(v);
		}

		auto sp = params.find("speakers");
		if (sp != params.end()) {
			if (!sp->is_string()) {
				error = "'speakers' must be a string layout name";
				return false;
			}
			speaker_layout layout;
			if (!SpeakerLayoutFromName(sp->get<std::string>(), layout)) {
				error = "unknown speaker layout '" + sp->get<std::string>() + "'";
				return false;
			}
			oai.speakers = layout;
		}

		// obs_reset_audio fails when audio is active; with no outputs yet it succeeds.
		// Active audio sources are re-attached by libobs to the new mix.
		if (!obs_reset_audio(&oai)) {
			error = "obs_reset_audio failed (audio may be active)";
			return false;
		}
	}

	obs_audio_info applied = {};
	if (!obs_get_audio_info(&applied)) {
		error = "audio not initialized";
		return false;
	}
	const char *monName = nullptr;
	const char *monId = nullptr;
	obs_get_audio_monitoring_device(&monName, &monId);
	result = json{
		{"sampleRate", applied.samples_per_sec},
		{"speakers", SpeakerLayoutName(applied.speakers)},
		{"monitoringDevice", json{{"id", monId ? std::string(monId) : std::string("default")},
					  {"name", monName ? std::string(monName) : std::string("Default")}}},
	};
	EmitEvent(EventNames::kSettingsAudioChanged, result);
	HostLog("[bridge] settings.setAudio -> " + std::to_string(applied.samples_per_sec) + "Hz " +
		SpeakerLayoutName(applied.speakers));
	return true;
}

// --- scenes / scene-items helpers -------------------------------------------

// Read an optional string field from params; returns "" when absent/not a
// string. Treats an empty string the same as absent for scene resolution.
std::string OptString(const json &params, const char *key)
{
	return JsonUtil::Str(params, key);
}

// Read a required string param via the tolerant JsonUtil::Str reader; on
// missing/empty set the bridge's uniform "<method> requires a non-empty
// '<key>'" error and return false so the caller returns straight through.
static bool RequireStr(const json &params, const char *method, const char *key, std::string &out, std::string &error)
{
	out = JsonUtil::Str(params, key);
	if (!out.empty()) {
		return true;
	}
	error = std::string(method) + " requires a non-empty '" + key + "'";
	return false;
}

// Guard that params is a JSON object, setting the uniform "<method> expects an
// object" error otherwise.
static bool RequireObject(const json &params, const char *method, std::string &error)
{
	if (params.is_object()) {
		return true;
	}
	error = std::string(method) + " expects an object";
	return false;
}

// Resolve a scene source by name, addref'd (caller releases). When `name` is
// empty, falls back to the scene bound to output channel 0 (the current scene),
// also addref'd. null when nothing resolves or the named source is not a scene.
obs_source_t *ResolveSceneSource(const std::string &name)
{
	if (name.empty()) {
		return Transitions::GetProgramScene(); // addref'd; unwraps the ch0 transition; null if unbound
	}
	obs_source_t *source = obs_get_source_by_name(name.c_str()); // addref'd
	if (!source) {
		return nullptr;
	}
	if (!obs_scene_from_source(source)) {
		obs_source_release(source); // not a scene
		return nullptr;
	}
	return source;
}

// A canvas-scoped scene request: the resolved canvas uuid (empty for the global
// channel-0 path) plus whether the request addresses an additional canvas. The
// `canvas` param is purely additive -- absent, empty, or equal to the Default
// canvas uuid all resolve exactly as before (global output-0), keeping the
// Default/absent path byte-identical.
struct CanvasTarget {
	std::string uuid;          // the additional canvas's uuid; empty => global path
	bool isAdditional = false; // true only when uuid names a non-Default canvas
};

CanvasTarget ResolveCanvasTarget(const json &params)
{
	CanvasTarget t;
	const std::string canvas = OptString(params, "canvas");
	if (canvas.empty() || canvas == ObsBootstrap::Canvases().Default().uuid) {
		return t; // global channel-0 path
	}
	t.uuid = canvas;
	t.isAdditional = true;
	return t;
}

// Resolve the scene a scene/source operation should act on, addref'd (caller
// releases), honoring an optional `canvas` param. For an additional canvas the
// scene is that canvas's current channel-0 scene; otherwise the existing global
// resolution (named `scene`, or output 0) applies unchanged. null when nothing
// resolves.
obs_source_t *ResolveTargetScene(const json &params)
{
	const CanvasTarget target = ResolveCanvasTarget(params);
	if (target.isAdditional) {
		return ObsBootstrap::CanvasRuntime().CurrentScene(target.uuid); // addref'd
	}
	return ResolveSceneSource(OptString(params, "scene")); // addref'd
}

// The canvas uuid that addresses one preview surface: empty for the Default
// surface (absent/empty/Default-uuid `canvas`), else the additional canvas's uuid.
// Reuses the scene resolver's Default-vs-additional rule so preview and scene ops
// agree on what "Default" means.
std::string PreviewCanvasParam(const json &params)
{
	return ResolveCanvasTarget(params).uuid;
}

// Locate a scene item by id within a scene. Returns the item WITHOUT an added
// ref (it is owned by the scene); valid only while the scene is held. null when
// no item matches.
struct ItemFind {
	int64_t id;
	obs_sceneitem_t *found;
};

obs_sceneitem_t *FindSceneItem(obs_scene_t *scene, int64_t id)
{
	ItemFind ctx{id, nullptr};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
			auto *c = static_cast<ItemFind *>(param);
			if (obs_sceneitem_get_id(item) == c->id) {
				c->found = item;
				return false; // stop
			}
			return true;
		},
		&ctx);
	return ctx.found;
}

// Parse the required scene-item id from params (accepts number or numeric
// string). Returns false and fills `error` when missing/unparseable.
bool ItemIdFromParams(const json &params, int64_t &id, std::string &error)
{
	if (params.is_object()) {
		auto it = params.find("id");
		if (it != params.end()) {
			if (it->is_number_integer()) {
				id = it->get<int64_t>();
				return true;
			}
			if (it->is_string()) {
				try {
					id = std::stoll(it->get<std::string>());
					return true;
				} catch (...) {
				}
			}
		}
	}
	error = "missing or invalid 'id'";
	return false;
}

// Drive preview selection from the UI (SourcesPanel). params: {scene?, id?,
// canvas?}. A null/absent id deselects. The addressed surface's scene (output 0
// for the Default surface, else the canvas's current scene) is authoritative;
// `scene` is only validated against it. Returns {selected: id|null}.
bool MethodPreviewSelect(const json &params, json &result, std::string &error)
{
	const std::string scene = OptString(params, "scene");

	int64_t id = 0;
	bool hasId = false;
	if (params.is_object()) {
		auto it = params.find("id");
		if (it != params.end() && !it->is_null()) {
			std::string idErr;
			if (!ItemIdFromParams(params, id, idErr)) {
				error = idErr;
				return false;
			}
			hasId = true;
		}
	}

	if (!Preview::SelectFromBridge(PreviewCanvasParam(params), scene, id, hasId, PreviewWindowParam(params))) {
		error = "preview selection failed (no scene or scene mismatch)";
		return false;
	}
	result = json{{"selected", hasId ? json(id) : json(nullptr)}};
	return true;
}

// --- scenes -----------------------------------------------------------------

// Emit scenes.changed, tagged with the addressed canvas uuid (empty/null for the
// global path) so a per-canvas panel filters to its own canvas's scene list.
void EmitScenesChanged(const std::string &canvasUuid)
{
	EmitEvent(EventNames::kScenesChanged, json{{"canvas", canvasUuid.empty() ? json(nullptr) : json(canvasUuid)}});
	if (canvasUuid.empty()) {
		// The global scene set may have changed (create/remove/duplicate/rename):
		// reconcile the per-scene switch hotkeys. Idempotent + no-op on a pure switch.
		Hotkeys::SyncSceneHotkeys();
	}
}

bool MethodScenesList(const json &params, json &result, std::string &error)
{
	const CanvasTarget target = ResolveCanvasTarget(params);
	if (target.isAdditional) {
		// List the additional canvas's own scenes (isolated from the global
		// registry), flagging the one bound to its channel 0 as current.
		json scenes = json::array();
		for (const CanvasRuntime::SceneInfo &s : ObsBootstrap::CanvasRuntime().Scenes(target.uuid)) {
			scenes.push_back(json{{"name", s.name}, {"current", s.current}});
		}
		result = std::move(scenes);
		return true;
	}
	(void)error;
	// List in the persisted user order (SceneCollection::SceneOrder) rather than
	// obs_enum_scenes' creation order -- libobs has no scene-ordering primitive of
	// its own; SceneOrder() is the only record of the user's chosen order.
	// `current` flags the scene bound to output channel 0 (unwrapped from the
	// program transition).
	OBSSourceAutoRelease current = Transitions::GetProgramScene(); // addref'd; may be null
	const char *currentName = current ? obs_source_get_name(current) : nullptr;
	const std::string currentStr = currentName ? currentName : std::string();

	json scenes = json::array();
	for (const std::string &uuid : SceneCollection::SceneOrder()) {
		OBSSourceAutoRelease scene = obs_get_source_by_uuid(uuid.c_str()); // addref'd
		const char *name = scene ? obs_source_get_name(scene) : nullptr;
		if (name) {
			scenes.push_back(json{{"name", name}, {"current", !currentStr.empty() && currentStr == name}});
		}
	}
	result = std::move(scenes);
	return true;
}

bool MethodScenesCreate(const json &params, json &result, std::string &error)
{
	std::string name;
	if (!RequireStr(params, "scenes.create", "name", name, error)) {
		return false;
	}

	const CanvasTarget target = ResolveCanvasTarget(params);
	if (target.isAdditional) {
		// Create the scene inside the additional canvas's own source namespace; the
		// runtime rejects a name already taken within that canvas.
		obs_source_t *created = ObsBootstrap::CanvasRuntime().CreateScene(target.uuid, name); // addref'd
		if (!created) {
			error = "could not create scene '" + name + "' in that canvas";
			return false;
		}
		obs_source_release(created); // the canvas owns the scene; drop our ref
		EmitScenesChanged(target.uuid);
		result = json{{"name", name}};
		return true;
	}

	// Reject duplicates (a source of any kind with that name collides).
	obs_source_t *existing = obs_get_source_by_name(name.c_str());
	if (existing) {
		obs_source_release(existing);
		error = "a source named '" + name + "' already exists";
		return false;
	}

	obs_scene_t *scene = obs_scene_create(name.c_str());
	if (!scene) {
		error = "obs_scene_create failed";
		return false;
	}
	obs_scene_release(scene); // creation ref; the scene source persists in the registry

	EmitScenesChanged(std::string());
	SceneCollection::Save();
	result = json{{"name", name}};
	return true;
}

// --- scene links ------------------------------------------------------------

// Resolve a GLOBAL (Default-canvas) scene name -> its source uuid (empty if none).
static std::string MainSceneUuidFromName(const std::string &name)
{
	OBSSourceAutoRelease s = obs_get_source_by_name(name.c_str());
	if (!s || !obs_scene_from_source(s)) {
		return {};
	}
	const char *u = obs_source_get_uuid(s);
	return u ? u : std::string();
}

// Resolve a GLOBAL scene uuid -> its current name (empty if none).
static std::string MainSceneNameFromUuid(const std::string &uuid)
{
	OBSSourceAutoRelease s = obs_get_source_by_uuid(uuid.c_str());
	if (!s) {
		return {};
	}
	const char *n = obs_source_get_name(s);
	return n ? n : std::string();
}

// Resolve a canvas scene name -> uuid within one canvas (empty if none).
static std::string CanvasSceneUuidFromName(const std::string &canvasUuid, const std::string &name)
{
	for (const CanvasRuntime::SceneInfo &s : ObsBootstrap::CanvasRuntime().Scenes(canvasUuid)) {
		if (s.name == name) {
			return s.uuid;
		}
	}
	return {};
}

// {links:[{mainScene,mainSceneName,canvas,canvasScene,canvasSceneName}]} -- flat
// rows. Names are resolved for display; rows whose uuids no longer resolve are
// still returned with empty names (the UI hides those / they get pruned on edit).
bool MethodSceneLinkList(const json &params, json &result, std::string &error)
{
	(void)params;
	(void)error;
	json rows = json::array();
	const CanvasSceneLink &link = ObsBootstrap::SceneLinks().Links();
	for (const auto &[mainUuid, perCanvas] : link.map) {
		const std::string mainName = MainSceneNameFromUuid(mainUuid);
		for (const auto &[canvasUuid, canvasSceneUuid] : perCanvas) {
			const std::string canvasSceneName =
				ObsBootstrap::CanvasRuntime().SceneNameForUuid(canvasUuid, canvasSceneUuid);
			rows.push_back(json{{"mainScene", mainUuid},
					    {"mainSceneName", mainName},
					    {"canvas", canvasUuid},
					    {"canvasScene", canvasSceneUuid},
					    {"canvasSceneName", canvasSceneName}});
		}
	}
	result = json{{"links", std::move(rows)}};
	return true;
}

// params: {mainScene:<name>, canvas:<uuid>, canvasScene:<name>}. Sets/moves the
// link, persists, applies immediately if `mainScene` is the live program scene,
// and broadcasts sceneLink.changed.
bool MethodSceneLinkSet(const json &params, json &result, std::string &error)
{
	const std::string mainName = OptString(params, "mainScene");
	const std::string canvasUuid = OptString(params, "canvas");
	const std::string canvasSceneName = OptString(params, "canvasScene");
	if (mainName.empty() || canvasUuid.empty() || canvasSceneName.empty()) {
		error = "sceneLink.set requires 'mainScene', 'canvas', 'canvasScene'";
		return false;
	}
	const std::string mainUuid = MainSceneUuidFromName(mainName);
	if (mainUuid.empty()) {
		error = "no main scene named '" + mainName + "'";
		return false;
	}
	const std::string canvasSceneUuid = CanvasSceneUuidFromName(canvasUuid, canvasSceneName);
	if (canvasSceneUuid.empty()) {
		error = "no scene named '" + canvasSceneName + "' in that canvas";
		return false;
	}

	ObsBootstrap::SceneLinks().Links().Set(mainUuid, canvasUuid, canvasSceneUuid);
	const bool saved = ObsBootstrap::SceneLinks().Save();

	// Instant feedback: if the linked main scene is live now, switch the canvas.
	OBSSourceAutoRelease program = Transitions::GetProgramScene();
	if (program) {
		const char *pu = obs_source_get_uuid(program);
		if (pu && mainUuid == pu) {
			ObsBootstrap::ApplyCanvasSceneLinks(mainUuid);
		}
	}

	EmitEvent(EventNames::kSceneLinkChanged, json::object());
	result = json::object();
	return PersistOrFail(saved, error);
}

// params: {mainScene:<name>, canvas:<uuid>}. Removes that canvas's link for the
// main scene, persists, broadcasts.
bool MethodSceneLinkClear(const json &params, json &result, std::string &error)
{
	const std::string mainName = OptString(params, "mainScene");
	const std::string canvasUuid = OptString(params, "canvas");
	if (mainName.empty() || canvasUuid.empty()) {
		error = "sceneLink.clear requires 'mainScene' and 'canvas'";
		return false;
	}
	const std::string mainUuid = MainSceneUuidFromName(mainName);
	CanvasSceneLink &link = ObsBootstrap::SceneLinks().Links();
	auto it = link.map.find(mainUuid);
	bool saved = true;
	if (it != link.map.end()) {
		it->second.erase(canvasUuid);
		if (it->second.empty()) {
			link.map.erase(it);
		}
		saved = ObsBootstrap::SceneLinks().Save();
	}
	EmitEvent(EventNames::kSceneLinkChanged, json::object());
	result = json::object();
	return PersistOrFail(saved, error);
}

bool MethodScenesRemove(const json &params, json &result, std::string &error)
{
	std::string name;
	if (!RequireStr(params, "scenes.remove", "name", name, error)) {
		return false;
	}

	const CanvasTarget canvasTarget = ResolveCanvasTarget(params);
	if (canvasTarget.isAdditional) {
		// Resolve the canvas scene's uuid before removal so the link prune below can
		// match it (the source is gone afterwards).
		const std::string goneSceneUuid = CanvasSceneUuidFromName(canvasTarget.uuid, name);
		// Remove from the additional canvas's own scenes; the runtime refuses the
		// last scene and switches channel 0 off the target first.
		if (!ObsBootstrap::CanvasRuntime().RemoveScene(canvasTarget.uuid, name)) {
			error = "could not remove scene '" + name + "' from that canvas";
			return false;
		}
		EmitScenesChanged(canvasTarget.uuid);
		ObsBootstrap::PruneSceneLinksForCanvasScene(canvasTarget.uuid, goneSceneUuid);
		result = json{{"removed", name}};
		return true;
	}

	// Count scenes and find a fallback (any scene other than the target) so we
	// can switch output 0 off the target before removing it. Refuse removing the
	// last scene.
	struct Ctx {
		std::string target;
		int count = 0;
		obs_source_t *fallback = nullptr; // addref'd
	} ctx;
	ctx.target = name;

	obs_enum_scenes(
		[](void *param, obs_source_t *source) -> bool {
			auto *c = static_cast<Ctx *>(param);
			c->count++;
			const char *n = obs_source_get_name(source);
			if (n && c->target != n && !c->fallback) {
				obs_source_get_ref(source); // keep for fallback
				c->fallback = source;
			}
			return true;
		},
		&ctx);

	if (ctx.count <= 1) {
		if (ctx.fallback) {
			obs_source_release(ctx.fallback);
		}
		error = "cannot remove the last scene";
		return false;
	}

	obs_source_t *target = obs_get_source_by_name(name.c_str()); // addref'd
	if (!target || !obs_scene_from_source(target)) {
		if (target) {
			obs_source_release(target);
		}
		if (ctx.fallback) {
			obs_source_release(ctx.fallback);
		}
		error = "no scene named '" + name + "'";
		return false;
	}

	// Capture the uuid before removal so links referencing it can be pruned after
	// the source is destroyed.
	const char *removedUuidC = obs_source_get_uuid(target);
	const std::string removedUuid = removedUuidC ? removedUuidC : std::string();

	// If the target is the current scene, switch the program scene to the fallback
	// first (non-animated -- the target is about to be removed).
	obs_source_t *current = Transitions::GetProgramScene(); // addref'd; unwraps the ch0 transition
	const bool removingCurrent = current && current == target;
	if (current) {
		obs_source_release(current);
	}
	if (removingCurrent && ctx.fallback) {
		Transitions::SetProgramScene(ctx.fallback, false);
	}

	obs_source_remove(target);
	obs_source_release(target);
	if (ctx.fallback) {
		obs_source_release(ctx.fallback);
	}

	EmitScenesChanged(std::string());
	SceneCollection::Save();
	ObsBootstrap::PruneSceneLinksForMainScene(removedUuid);
	result = json{{"removed", name}};
	return true;
}

bool MethodScenesSetCurrent(const json &params, json &result, std::string &error)
{
	std::string name;
	if (!RequireStr(params, "scenes.setCurrent", "name", name, error)) {
		return false;
	}

	const CanvasTarget target = ResolveCanvasTarget(params);
	if (target.isAdditional) {
		// Set the additional canvas's current scene (its channel 0), not output 0.
		if (!ObsBootstrap::CanvasRuntime().SetCurrentScene(target.uuid, name)) {
			error = "no scene named '" + name + "' in that canvas";
			return false;
		}
		EmitScenesChanged(target.uuid);
		result = json{{"name", name}};
		return true;
	}

	const std::string uuid = MainSceneUuidFromName(name);
	if (uuid.empty()) {
		error = "no scene named '" + name + "'";
		return false;
	}
	// Route through the shared seam so the per-scene switch hotkeys and this method
	// both run Transitions::SetProgramScene + ApplyCanvasSceneLinks identically.
	SwitchDefaultProgramScene(uuid);
	result = json{{"name", name}};
	return true;
}

bool MethodScenesRename(const json &params, json &result, std::string &error)
{
	const std::string from = OptString(params, "from");
	const std::string to = OptString(params, "to");
	if (from.empty() || to.empty()) {
		error = "scenes.rename requires 'from' and 'to'";
		return false;
	}
	if (from == to) {
		result = json{{"name", to}};
		return true;
	}

	const CanvasTarget target = ResolveCanvasTarget(params);
	if (target.isAdditional) {
		// Rename within the additional canvas's own source namespace; the runtime
		// rejects a clash with another scene in that canvas.
		if (!ObsBootstrap::CanvasRuntime().RenameScene(target.uuid, from, to)) {
			error = "could not rename '" + from + "' to '" + to + "' in that canvas";
			return false;
		}
		EmitScenesChanged(target.uuid);
		result = json{{"name", to}};
		return true;
	}

	obs_source_t *clash = obs_get_source_by_name(to.c_str());
	if (clash) {
		obs_source_release(clash);
		error = "a source named '" + to + "' already exists";
		return false;
	}
	obs_source_t *source = obs_get_source_by_name(from.c_str()); // addref'd
	if (!source || !obs_scene_from_source(source)) {
		if (source) {
			obs_source_release(source);
		}
		error = "no scene named '" + from + "'";
		return false;
	}
	obs_source_set_name(source, to.c_str());
	obs_source_release(source);

	EmitScenesChanged(std::string());
	SceneCollection::Save();
	result = json{{"name", to}};
	return true;
}

// scenes.duplicate {name, canvas?}: duplicate a global scene (Default path only)
// using shared source refs, matching OBS's "Duplicate Scene". Additional-canvas
// duplication is unsupported (same limitation as scenes.reorder).
bool MethodScenesDuplicate(const json &params, json &result, std::string &error)
{
	std::string name;
	if (!RequireStr(params, "scenes.duplicate", "name", name, error)) {
		return false;
	}
	const CanvasTarget target = ResolveCanvasTarget(params);
	if (target.isAdditional) {
		error = "scene duplication is not supported for additional canvases";
		return false;
	}
	obs_source_t *srcScene = obs_get_source_by_name(name.c_str()); // addref'd
	if (!srcScene || !obs_scene_from_source(srcScene)) {
		if (srcScene) {
			obs_source_release(srcScene);
		}
		error = "no scene named '" + name + "'";
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(srcScene);

	// Find a free "<name> N" (N starting at 2).
	std::string newName;
	for (int n = 2;; ++n) {
		std::string candidate = name + " " + std::to_string(n);
		obs_source_t *taken = obs_get_source_by_name(candidate.c_str());
		if (taken) {
			obs_source_release(taken);
			continue;
		}
		newName = std::move(candidate);
		break;
	}

	obs_scene_t *dup = obs_scene_duplicate(scene, newName.c_str(), OBS_SCENE_DUP_REFS); // new ref
	obs_source_release(srcScene);
	if (!dup) {
		error = "obs_scene_duplicate failed";
		return false;
	}
	obs_scene_release(dup); // the new scene source persists in the registry

	EmitScenesChanged(std::string());
	SceneCollection::Save();
	result = json{{"name", newName}};
	return true;
}

// Resolve a scene by name on an explicit source canvas (Default when
// srcCanvasUuid is empty or equals the Default canvas's uuid). Addref'd;
// caller releases. Mirrors MethodScenesDuplicate's own-canvas resolution but
// ALSO supports an additional canvas as the source (which MethodScenesDuplicate
// explicitly rejects).
obs_source_t *ResolveNamedSceneOnCanvas(const std::string &sceneName, const std::string &srcCanvasUuid)
{
	if (!srcCanvasUuid.empty() && srcCanvasUuid != ObsBootstrap::Canvases().Default().uuid) {
		obs_canvas_t *srcCanvas = ObsBootstrap::CanvasRuntime().Find(srcCanvasUuid);
		if (!srcCanvas) {
			return nullptr;
		}
		return obs_canvas_get_source_by_name(srcCanvas, sceneName.c_str()); // addref'd
	}
	return obs_get_source_by_name(sceneName.c_str()); // addref'd
}

// Resolve an obs_canvas_t* for a destination-canvas uuid (empty or the Default
// canvas's uuid => the main canvas). For the main-canvas case, the returned
// OBSCanvasAutoRelease `mainCanvasHolder` keeps the addref'd main-canvas pointer
// alive for the caller's use; ignore it when isAdditionalDest is true.
struct ResolvedDestCanvas {
	obs_canvas_t *canvas = nullptr;
	OBSCanvasAutoRelease mainCanvasHolder; // only populated for the Default/main case
};
ResolvedDestCanvas ResolveDestCanvas(const std::string &destCanvasUuid)
{
	ResolvedDestCanvas r;
	if (destCanvasUuid.empty() || destCanvasUuid == ObsBootstrap::Canvases().Default().uuid) {
		r.mainCanvasHolder = obs_get_main_canvas(); // addref'd
		r.canvas = r.mainCanvasHolder;
		return r;
	}
	r.canvas = ObsBootstrap::CanvasRuntime().Find(destCanvasUuid);
	return r;
}

// Find a free "<name> N" (N starting at 2) in the DESTINATION canvas's own
// namespace, mirroring MethodScenesDuplicate's auto-suffix but scoped per-canvas.
std::string FreeSceneNameOnCanvas(const std::string &baseName, const std::string &destCanvasUuid, bool destIsAdditional,
				  obs_canvas_t *destCanvas)
{
	for (int n = 2;; ++n) {
		std::string candidate = baseName + " " + std::to_string(n);
		OBSSourceAutoRelease taken = destIsAdditional
						     ? obs_canvas_get_source_by_name(destCanvas, candidate.c_str())
						     : obs_get_source_by_name(candidate.c_str());
		if (taken) {
			continue;
		}
		return candidate;
	}
}

// The shared duplicate-to-canvas core: deep-copies `sceneName` (on `srcCanvasUuid`,
// "" = Default) onto `destCanvasUuid` ("" = Default) via OBS_SCENE_DUP_COPY (scene
// filters + every item's source + that source's own filters, all fresh copies) then
// obs_canvas_move_scene to place it. Does NOT touch SceneLinkStore or UndoManager --
// callers (the bridge method, and later the redo path) own that. On success, fills
// `outNewSceneUuid`, `outNewName`, and `outUndoState` so callers can build undo state.
bool DuplicateSceneToCanvasCore(const std::string &sceneName, const std::string &srcCanvasUuid,
				const std::string &destCanvasUuid, std::string &outNewSceneUuid,
				std::string &outNewName, json &outUndoState, std::string &error)
{
	OBSSourceAutoRelease srcScene = ResolveNamedSceneOnCanvas(sceneName, srcCanvasUuid);
	if (!srcScene || !obs_scene_from_source(srcScene)) {
		error = "no scene named '" + sceneName + "'";
		return false;
	}

	ResolvedDestCanvas dest = ResolveDestCanvas(destCanvasUuid);
	if (!dest.canvas) {
		error = "unknown destination canvas";
		return false;
	}
	const bool destIsAdditional = !destCanvasUuid.empty() &&
				      destCanvasUuid != ObsBootstrap::Canvases().Default().uuid;

	const std::string newName = FreeSceneNameOnCanvas(sceneName, destCanvasUuid, destIsAdditional, dest.canvas);

	obs_scene_t *srcSceneObj = obs_scene_from_source(srcScene);

	// obs_source_duplicate (libobs/obs-source.c) does not always copy: for a
	// nested-scene item or one flagged OBS_SOURCE_DO_NOT_DUPLICATE, it hands back
	// a ref to the ORIGINAL source instead. Snapshot the source scene's own item
	// uuids first so the post-duplicate loop below can tell shared refs apart
	// from real copies, without re-deriving libobs's internal duplicate-vs-share
	// branching (which could drift or miss a flag).
	std::unordered_set<std::string> origItemUuids;
	obs_scene_enum_items(
		srcSceneObj,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
			auto *out = static_cast<std::unordered_set<std::string> *>(param);
			obs_source_t *src = obs_sceneitem_get_source(item); // borrowed
			if (!src) {
				return true;
			}
			const char *uuid = obs_source_get_uuid(src);
			if (uuid) {
				out->insert(uuid);
			}
			return true;
		},
		&origItemUuids);

	obs_scene_t *dup = obs_scene_duplicate(srcSceneObj, newName.c_str(), OBS_SCENE_DUP_COPY); // new ref
	if (!dup) {
		error = "obs_scene_duplicate failed";
		return false;
	}
	obs_canvas_move_scene(dup, dest.canvas);

	// Serialize every genuinely duplicated child source (uuid + full data) BEFORE
	// releasing `dup`, so undo/redo can restore them exactly, mirroring
	// CaptureItemSnapshot. Items whose uuid matches origItemUuids are shared refs
	// (see above) rather than copies: we don't own them, so they're excluded here
	// and must never be deleted/restored by our undo/redo handlers.
	json sourcesJson = json::array();
	struct Ctx {
		json *out;
		const std::unordered_set<std::string> *origUuids;
	} ctx{&sourcesJson, &origItemUuids};
	obs_scene_enum_items(
		dup,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
			auto *c = static_cast<Ctx *>(param);
			obs_source_t *src = obs_sceneitem_get_source(item); // borrowed
			if (!src) {
				return true;
			}
			const char *uuid = obs_source_get_uuid(src);
			if (uuid && c->origUuids->count(uuid)) {
				return true;
			}
			OBSDataAutoRelease data = obs_save_source(src);
			const char *jsonStr = data ? obs_data_get_json(data) : nullptr;
			c->out->push_back(json{{"uuid", uuid ? uuid : ""}, {"sourceData", jsonStr ? jsonStr : ""}});
			return true;
		},
		&ctx);

	obs_source_t *dupSource = obs_scene_get_source(dup); // borrowed from `dup`
	const char *dupUuidC = obs_source_get_uuid(dupSource);
	OBSDataAutoRelease sceneData = obs_save_source(dupSource);
	const char *sceneJsonStr = sceneData ? obs_data_get_json(sceneData) : nullptr;

	outNewSceneUuid = dupUuidC ? dupUuidC : std::string();
	outNewName = newName;
	outUndoState = json{
		{"newSceneUuid", outNewSceneUuid},
		{"sceneData", sceneJsonStr ? sceneJsonStr : ""},
		{"destCanvas", destCanvasUuid},
		{"sources", sourcesJson},
	};

	obs_scene_release(dup); // the registry holds it now, matching MethodScenesDuplicate's own release
	return true;
}

// Defined near the other Cb adapters (kAddItemFromSnapshot/kRemoveItemBySource);
// forward-declared here since MethodScenesDuplicateToCanvas below registers them
// with UndoManager before that point in the file.
extern const UndoManager::Cb kUndoDuplicateSceneToCanvas;
extern const UndoManager::Cb kRedoDuplicateSceneToCanvas;

// scenes.duplicateToCanvas {name, canvas?, destCanvas}: deep-copy a scene (its own
// scene-level filters + every item's SOURCE, incl. that source's filters, via
// OBS_SCENE_DUP_COPY) from its current canvas onto ANY other canvas (or the same
// one). Independent of the existing scenes.duplicate, which stays REFS-only,
// Default-canvas-only, and unchanged.
bool MethodScenesDuplicateToCanvas(const json &params, json &result, std::string &error)
{
	std::string name;
	if (!RequireStr(params, "scenes.duplicateToCanvas", "name", name, error)) {
		return false;
	}
	const std::string srcCanvasUuid = OptString(params, "canvas");
	const std::string destCanvasUuid = OptString(params, "destCanvas");

	std::string newSceneUuid, newName;
	json undoState;
	if (!DuplicateSceneToCanvasCore(name, srcCanvasUuid, destCanvasUuid, newSceneUuid, newName, undoState, error)) {
		return false;
	}

	const std::string emitCanvas =
		destCanvasUuid.empty() || destCanvasUuid == ObsBootstrap::Canvases().Default().uuid ? std::string()
												    : destCanvasUuid;
	EmitScenesChanged(emitCanvas);
	SceneCollection::Save();

	// Copy any scene-link mapping: every main scene currently linked to the SOURCE
	// scene (on srcCanvasUuid) gets an additional/updated link pointing at the new
	// duplicate on destCanvasUuid. Only meaningful when both the source and the
	// destination are real additional canvases (main scenes are never link
	// targets) and the destination differs from the source -- a same-canvas
	// duplicate must leave the original scene's link untouched.
	if (!srcCanvasUuid.empty() && srcCanvasUuid != ObsBootstrap::Canvases().Default().uuid &&
	    !destCanvasUuid.empty() && destCanvasUuid != ObsBootstrap::Canvases().Default().uuid &&
	    destCanvasUuid != srcCanvasUuid) {
		OBSSourceAutoRelease srcSceneForLink = ResolveNamedSceneOnCanvas(name, srcCanvasUuid);
		const char *srcSceneUuidC = srcSceneForLink ? obs_source_get_uuid(srcSceneForLink) : nullptr;
		if (srcSceneUuidC) {
			const std::string srcSceneUuid = srcSceneUuidC;
			for (const auto &[mainUuid, perCanvas] : ObsBootstrap::SceneLinks().Links().map) {
				auto it = perCanvas.find(srcCanvasUuid);
				if (it != perCanvas.end() && it->second == srcSceneUuid) {
					ObsBootstrap::SceneLinks().Links().Set(mainUuid, destCanvasUuid, newSceneUuid);
				}
			}
		}
		ObsBootstrap::SceneLinks().Save();
	}

	ObsBootstrap::Undo().AddAction("Duplicate '" + name + "' to canvas", kUndoDuplicateSceneToCanvas,
				       kRedoDuplicateSceneToCanvas, undoState.dump(), undoState.dump());

	result = json{{"name", newName}, {"uuid", newSceneUuid}};
	return true;
}

// scenes.reorder {name, direction:"up"|"down"|"top"|"bottom", canvas?} or
// {name, to, canvas?}: reorder a scene within the global scene list, either
// relatively (`direction`) or to an absolute index (`to`, a top-first UI index
// matching scenes.list order -- see below).
//
// HONEST LIMITATION: libobs has no scene-ordering primitive. Unlike scene ITEMS
// (obs_sceneitem_set_order), global scenes are plain sources enumerated by
// obs_enum_scenes in CREATION order -- it exposes no settable order. Mirroring
// the legacy Qt frontend's SaveSceneListOrder, the order lives OUTSIDE libobs: a
// "scene_order" array persisted alongside the rest of the scene collection (see
// SceneCollection::SceneOrder/ReorderScene/MoveSceneToIndex in
// scene_persistence.cpp), which scenes.list now consults instead of raw
// creation order. scenes.list pushes SceneOrder() straight through with no
// inversion (unlike sceneItems.list, which inverts libobs' bottom-to-top
// enumeration) -- SceneOrder()[0] IS the UI list's top entry, so a `to` index
// here maps directly onto MoveSceneToIndex with no inversion. An additional
// canvas's scenes (obs_canvas_enum_scenes, same limitation) aren't tracked by
// that order yet, so reordering there still isn't supported.
bool MethodScenesReorder(const json &params, json &result, std::string &error)
{
	std::string name;
	const std::string direction = OptString(params, "direction");
	if (!RequireStr(params, "scenes.reorder", "name", name, error)) {
		return false;
	}
	int to = -1;
	bool hasTo = false;
	if (auto it = params.find("to"); it != params.end() && it->is_number_integer()) {
		to = it->get<int>();
		hasTo = true;
	}
	if (!hasTo && direction != "up" && direction != "down" && direction != "top" && direction != "bottom") {
		error = "scenes.reorder needs 'direction' (up|down|top|bottom) or an integer 'to' index";
		return false;
	}
	if (ResolveCanvasTarget(params).isAdditional) {
		error = "scene reordering is not yet supported for additional canvases";
		return false;
	}
	OBSSourceAutoRelease scene = obs_get_source_by_name(name.c_str()); // addref'd
	if (!scene || !obs_scene_from_source(scene)) {
		error = "no scene named '" + name + "'";
		return false;
	}
	const char *uuid = obs_source_get_uuid(scene);
	const bool ok = uuid && (hasTo ? SceneCollection::MoveSceneToIndex(uuid, to)
					: SceneCollection::ReorderScene(uuid, direction));
	if (!ok) {
		error = "scene reordering failed";
		return false;
	}
	EmitScenesChanged(std::string());
	SceneCollection::Save();
	result = json{{"name", name}, {"direction", direction}, {"to", hasTo ? json(to) : json(nullptr)}};
	return true;
}

// --- scene items ------------------------------------------------------------

// Emit sceneItems.changed for the resolved scene. Passes the scene's actual name
// plus the addressed canvas uuid (empty for the global path) so a per-canvas panel
// filters the event to its own canvas before deciding whether the change applies.
void EmitSceneItemsChanged(obs_source_t *sceneSource, const std::string &canvasUuid)
{
	const char *name = sceneSource ? obs_source_get_name(sceneSource) : nullptr;
	EmitEvent(EventNames::kSceneItemsChanged,
		  json{{"scene", name ? json(name) : json(nullptr)},
		       {"canvas", canvasUuid.empty() ? json(nullptr) : json(canvasUuid)}});
}

// Scale-filter token <-> obs_scale_type. Shared by sceneItems.list (enum->token)
// and sceneItems.setScaleFilter (token->enum) so both speak one vocabulary.
struct ScaleFilterEntry {
	const char *token;
	obs_scale_type type;
};
static const ScaleFilterEntry kScaleFilters[] = {
	{"disable", OBS_SCALE_DISABLE}, {"point", OBS_SCALE_POINT},     {"bilinear", OBS_SCALE_BILINEAR},
	{"bicubic", OBS_SCALE_BICUBIC}, {"lanczos", OBS_SCALE_LANCZOS}, {"area", OBS_SCALE_AREA},
};

const char *ScaleFilterToToken(obs_scale_type type)
{
	for (const auto &e : kScaleFilters) {
		if (e.type == type) {
			return e.token;
		}
	}
	return "disable";
}

bool ScaleFilterFromToken(const std::string &token, obs_scale_type &type)
{
	for (const auto &e : kScaleFilters) {
		if (token == e.token) {
			type = e.type;
			return true;
		}
	}
	return false;
}

// Blend-mode token <-> obs_blending_type. Shared by sceneItems.list (enum->token)
// and sceneItems.setBlendingMode (token->enum) so both speak one vocabulary.
struct BlendModeEntry {
	const char *token;
	obs_blending_type type;
};
static const BlendModeEntry kBlendModes[] = {
	{"normal", OBS_BLEND_NORMAL},     {"additive", OBS_BLEND_ADDITIVE}, {"subtract", OBS_BLEND_SUBTRACT},
	{"screen", OBS_BLEND_SCREEN},     {"multiply", OBS_BLEND_MULTIPLY}, {"lighten", OBS_BLEND_LIGHTEN},
	{"darken", OBS_BLEND_DARKEN},
};

const char *BlendModeToToken(obs_blending_type type)
{
	for (const auto &e : kBlendModes) {
		if (e.type == type) {
			return e.token;
		}
	}
	return "normal";
}

bool BlendModeFromToken(const std::string &token, obs_blending_type &type)
{
	for (const auto &e : kBlendModes) {
		if (token == e.token) {
			type = e.type;
			return true;
		}
	}
	return false;
}

// Blend-method token <-> obs_blending_method. Shared by sceneItems.list (enum->token)
// and sceneItems.setBlendingMethod (token->enum) so both speak one vocabulary.
struct BlendMethodEntry {
	const char *token;
	obs_blending_method method;
};
static const BlendMethodEntry kBlendMethods[] = {
	{"default", OBS_BLEND_METHOD_DEFAULT},
	{"srgbOff", OBS_BLEND_METHOD_SRGB_OFF},
};

const char *BlendMethodToToken(obs_blending_method method)
{
	for (const auto &e : kBlendMethods) {
		if (e.method == method) {
			return e.token;
		}
	}
	return "default";
}

bool BlendMethodFromToken(const std::string &token, obs_blending_method &method)
{
	for (const auto &e : kBlendMethods) {
		if (token == e.token) {
			method = e.method;
			return true;
		}
	}
	return false;
}

// --- undo recording (apply-target-state) ------------------------------------
//
// For mutations that change an existing scene item WITHOUT altering structure,
// undo and redo are symmetric: both just apply a captured target state. So each
// mutation type has ONE Apply<X>(state) registered as both the undo and redo
// callback; the manager hands it the BEFORE payload on undo and the AFTER
// payload on redo. State carries {canvas, scene, source-uuid, ...} so the target
// re-resolves via the SAME path the bridge methods use; keying on source uuid
// (not item id) survives id churn from later add/remove undos.

// Locate a scene item by its source's uuid. Borrowed item (owned by the scene),
// valid only while `sceneSource` is held. null when none matches.
obs_sceneitem_t *FindItemBySourceUuid(obs_source_t *sceneSource, const std::string &uuid)
{
	obs_scene_t *scene = sceneSource ? obs_scene_from_source(sceneSource) : nullptr;
	if (!scene || uuid.empty()) {
		return nullptr;
	}
	struct Ctx {
		const std::string &uuid;
		obs_sceneitem_t *found;
	} ctx{uuid, nullptr};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
			auto *c = static_cast<Ctx *>(param);
			obs_source_t *src = obs_sceneitem_get_source(item);
			const char *u = src ? obs_source_get_uuid(src) : nullptr;
			if (u && c->uuid == u) {
				c->found = item;
				return false; // stop
			}
			return true;
		},
		&ctx);
	return ctx.found;
}

// Resolve scene + item from a captured state json (keys: canvas, scene, source).
// On success `sceneSource` is addref'd (caller releases) and `item` is borrowed.
bool ResolveStateItem(const json &state, obs_source_t *&sceneSource, obs_sceneitem_t *&item)
{
	sceneSource = ResolveTargetScene(state); // reads canvas/scene; addref'd
	if (!sceneSource) {
		return false;
	}
	item = FindItemBySourceUuid(sceneSource, OptString(state, "source"));
	if (!item) {
		obs_source_release(sceneSource);
		sceneSource = nullptr;
		return false;
	}
	return true;
}

// Persist a mutated source to whichever store owns it. A source bound to a
// global output channel (1..6) is excluded from the scene-collection save (see
// SaveFilter in scene_persistence.cpp) and must persist via GlobalAudioChannels
// instead; every other source -- including additional-canvas scene items, since
// SceneCollection::Save() already covers every canvas's scenes in the one
// collection file (SaveFilter keeps both main- and additional-canvas scoped
// sources) -- persists via SceneCollection::Save(). The single choke point every
// mutating handler (scene items, audio, source/filter properties) should call
// exactly once after applying its change.
void PersistSourceState(obs_source_t *source)
{
	// A filter isn't itself bound to an output channel; resolve to its parent so a
	// filter edit on a global audio channel (e.g. a noise-suppression filter on
	// Desktop Audio) persists via the channel's own store too -- SaveFilter
	// excludes channel 1..6 sources (and everything attached to them) from the
	// scene-collection save entirely.
	obs_source_t *owner = source;
	if (obs_source_t *parent = obs_filter_get_parent(source)) {
		owner = parent;
	}
	for (int ch = GlobalAudioChannels::kFirstChannel; ch <= GlobalAudioChannels::kLastChannel; ch++) {
		OBSSourceAutoRelease channelSource = obs_get_output_source(ch); // addref'd; may be null
		if (channelSource && channelSource.Get() == owner) {
			ObsBootstrap::GlobalAudioChannels().Persist();
			return;
		}
	}
	SceneCollection::Save();
}

// Emit + persist after a scene-item mutation (or an undo Apply), matching what
// the original mutation does so the UI refreshes and the undo persists.
void CommitSceneItemChange(const json &params, obs_source_t *sceneSource)
{
	EmitSceneItemsChanged(sceneSource, ResolveCanvasTarget(params).uuid);
	PersistSourceState(sceneSource);
}

// {canvas, scene, source-uuid} -- the re-resolution keys shared by every state.
json StateBase(const json &params, obs_source_t *src)
{
	const char *uuid = src ? obs_source_get_uuid(src) : nullptr;
	return json{
		{"canvas", OptString(params, "canvas")},
		{"scene", OptString(params, "scene")},
		{"source", uuid ? std::string(uuid) : std::string()},
	};
}

// Record an apply-target-state action: undo==redo==apply; the manager picks the
// data string (BEFORE on undo, AFTER on redo).
void RecordUndo(const std::string &name, const UndoManager::Cb &apply, const json &before, const json &after)
{
	ObsBootstrap::Undo().AddAction(name, apply, apply, before.dump(), after.dump());
}

// Full item geometry (info2 + crop) plus the re-resolution keys.
json CaptureTransformState(const json &params, obs_sceneitem_t *item)
{
	obs_transform_info info;
	obs_sceneitem_get_info2(item, &info);
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);

	json s = StateBase(params, obs_sceneitem_get_source(item));
	s["pos"] = json{{"x", info.pos.x}, {"y", info.pos.y}};
	s["rot"] = info.rot;
	s["scale"] = json{{"x", info.scale.x}, {"y", info.scale.y}};
	s["alignment"] = info.alignment;
	s["boundsType"] = static_cast<int>(info.bounds_type);
	s["boundsAlignment"] = info.bounds_alignment;
	s["bounds"] = json{{"x", info.bounds.x}, {"y", info.bounds.y}};
	s["cropToBounds"] = info.crop_to_bounds;
	s["crop"] = json{{"left", crop.left}, {"top", crop.top}, {"right", crop.right}, {"bottom", crop.bottom}};
	return s;
}

// Overlay the geometry fields present in `g` (info2 + crop, plus optional
// visible/locked) onto `item`; absent keys keep the item's current value. Shared
// by ApplyTransform (top-level keys) and AddItemFromSnapshot (nested "geometry").
void SetItemGeometry(obs_sceneitem_t *item, const json &g)
{
	obs_transform_info info;
	obs_sceneitem_get_info2(item, &info); // seed, then overlay the snapshot
	auto fnum = [](const json &o, const char *k, float def) -> float {
		auto it = o.find(k);
		return (it != o.end() && it->is_number()) ? it->get<float>() : def;
	};
	auto subVec = [&](const char *key, float &x, float &y) {
		auto it = g.find(key);
		if (it != g.end() && it->is_object()) {
			x = fnum(*it, "x", x);
			y = fnum(*it, "y", y);
		}
	};
	subVec("pos", info.pos.x, info.pos.y);
	info.rot = fnum(g, "rot", info.rot);
	subVec("scale", info.scale.x, info.scale.y);
	if (auto it = g.find("alignment"); it != g.end() && it->is_number_integer()) {
		info.alignment = it->get<uint32_t>();
	}
	if (auto it = g.find("boundsType"); it != g.end() && it->is_number_integer()) {
		info.bounds_type = static_cast<obs_bounds_type>(it->get<int>());
	}
	if (auto it = g.find("boundsAlignment"); it != g.end() && it->is_number_integer()) {
		info.bounds_alignment = it->get<uint32_t>();
	}
	subVec("bounds", info.bounds.x, info.bounds.y);
	if (auto it = g.find("cropToBounds"); it != g.end() && it->is_boolean()) {
		info.crop_to_bounds = it->get<bool>();
	}

	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	if (auto it = g.find("crop"); it != g.end() && it->is_object()) {
		auto cropInt = [&](const char *k, int &out) {
			auto c = it->find(k);
			if (c != it->end() && c->is_number_integer()) {
				out = c->get<int>();
			}
		};
		cropInt("left", crop.left);
		cropInt("top", crop.top);
		cropInt("right", crop.right);
		cropInt("bottom", crop.bottom);
	}

	obs_sceneitem_defer_update_begin(item);
	obs_sceneitem_set_info2(item, &info);
	obs_sceneitem_set_crop(item, &crop);
	obs_sceneitem_defer_update_end(item);

	if (auto it = g.find("visible"); it != g.end() && it->is_boolean()) {
		obs_sceneitem_set_visible(item, it->get<bool>());
	}
	if (auto it = g.find("locked"); it != g.end() && it->is_boolean()) {
		obs_sceneitem_set_locked(item, it->get<bool>());
	}
}

void ApplyTransform(const std::string &data)
{
	json state = json::parse(data, nullptr, false);
	if (state.is_discarded()) {
		return;
	}
	obs_source_t *sceneSource = nullptr;
	obs_sceneitem_t *item = nullptr;
	if (!ResolveStateItem(state, sceneSource, item)) {
		return;
	}

	SetItemGeometry(item, state);

	CommitSceneItemChange(state, sceneSource);
	obs_source_release(sceneSource);
}

void ApplyVisible(const std::string &data)
{
	json state = json::parse(data, nullptr, false);
	if (state.is_discarded()) {
		return;
	}
	obs_source_t *sceneSource = nullptr;
	obs_sceneitem_t *item = nullptr;
	if (!ResolveStateItem(state, sceneSource, item)) {
		return;
	}
	obs_sceneitem_set_visible(item, state.value("visible", false));
	CommitSceneItemChange(state, sceneSource);
	obs_source_release(sceneSource);
}

void ApplyLocked(const std::string &data)
{
	json state = json::parse(data, nullptr, false);
	if (state.is_discarded()) {
		return;
	}
	obs_source_t *sceneSource = nullptr;
	obs_sceneitem_t *item = nullptr;
	if (!ResolveStateItem(state, sceneSource, item)) {
		return;
	}
	obs_sceneitem_set_locked(item, state.value("locked", false));
	CommitSceneItemChange(state, sceneSource);
	obs_source_release(sceneSource);
}

void ApplyScaleFilter(const std::string &data)
{
	json state = json::parse(data, nullptr, false);
	if (state.is_discarded()) {
		return;
	}
	obs_source_t *sceneSource = nullptr;
	obs_sceneitem_t *item = nullptr;
	if (!ResolveStateItem(state, sceneSource, item)) {
		return;
	}
	obs_scale_type type;
	if (ScaleFilterFromToken(OptString(state, "filter"), type)) {
		obs_sceneitem_set_scale_filter(item, type);
		CommitSceneItemChange(state, sceneSource);
	}
	obs_source_release(sceneSource);
}

void ApplyBlendingMode(const std::string &data)
{
	json state = json::parse(data, nullptr, false);
	if (state.is_discarded()) {
		return;
	}
	obs_source_t *sceneSource = nullptr;
	obs_sceneitem_t *item = nullptr;
	if (!ResolveStateItem(state, sceneSource, item)) {
		return;
	}
	obs_blending_type type;
	if (BlendModeFromToken(OptString(state, "mode"), type)) {
		obs_sceneitem_set_blending_mode(item, type);
		CommitSceneItemChange(state, sceneSource);
	}
	obs_source_release(sceneSource);
}

void ApplyBlendingMethod(const std::string &data)
{
	json state = json::parse(data, nullptr, false);
	if (state.is_discarded()) {
		return;
	}
	obs_source_t *sceneSource = nullptr;
	obs_sceneitem_t *item = nullptr;
	if (!ResolveStateItem(state, sceneSource, item)) {
		return;
	}
	obs_blending_method method;
	if (BlendMethodFromToken(OptString(state, "method"), method)) {
		obs_sceneitem_set_blending_method(item, method);
		CommitSceneItemChange(state, sceneSource);
	}
	obs_source_release(sceneSource);
}

void ApplyRename(const std::string &data)
{
	json state = json::parse(data, nullptr, false);
	if (state.is_discarded()) {
		return;
	}
	const std::string name = OptString(state, "name");
	if (name.empty()) {
		return;
	}
	obs_source_t *sceneSource = nullptr;
	obs_sceneitem_t *item = nullptr;
	if (!ResolveStateItem(state, sceneSource, item)) {
		return;
	}
	obs_source_t *src = obs_sceneitem_get_source(item); // borrowed
	if (src) {
		obs_source_set_name(src, name.c_str());
	}
	CommitSceneItemChange(state, sceneSource);
	obs_source_release(sceneSource);
}

// The full scene order as source uuids in native (bottom-to-top) enumeration
// order -- the order obs_scene_reorder_items expects (index 0 == first_item).
json CaptureOrderState(const json &params, obs_source_t *sceneSource)
{
	json order = json::array();
	obs_scene_enum_items(
		obs_scene_from_source(sceneSource),
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
			auto *arr = static_cast<json *>(param);
			obs_source_t *src = obs_sceneitem_get_source(item);
			const char *u = src ? obs_source_get_uuid(src) : nullptr;
			arr->push_back(u ? json(u) : json(""));
			return true;
		},
		&order);
	return json{
		{"canvas", OptString(params, "canvas")},
		{"scene", OptString(params, "scene")},
		{"order", std::move(order)},
	};
}

void ApplyOrder(const std::string &data)
{
	json state = json::parse(data, nullptr, false);
	if (state.is_discarded()) {
		return;
	}
	obs_source_t *sceneSource = ResolveTargetScene(state); // addref'd
	if (!sceneSource) {
		return;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	auto orderIt = state.find("order");
	if (orderIt == state.end() || !orderIt->is_array()) {
		obs_source_release(sceneSource);
		return;
	}

	std::unordered_map<std::string, obs_sceneitem_t *> byUuid;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
			auto *m = static_cast<std::unordered_map<std::string, obs_sceneitem_t *> *>(param);
			obs_source_t *src = obs_sceneitem_get_source(item);
			const char *u = src ? obs_source_get_uuid(src) : nullptr;
			if (u) {
				(*m)[u] = item;
			}
			return true;
		},
		&byUuid);

	std::vector<obs_sceneitem_t *> ordered;
	ordered.reserve(orderIt->size());
	for (const auto &u : *orderIt) {
		if (!u.is_string()) {
			continue;
		}
		auto f = byUuid.find(u.get<std::string>());
		if (f != byUuid.end()) {
			ordered.push_back(f->second);
		}
	}
	// Only reorder when the captured set still matches the scene exactly;
	// obs_scene_reorder_items no-ops (returns false) if the order is unchanged.
	if (!ordered.empty() && ordered.size() == byUuid.size()) {
		obs_scene_reorder_items(scene, ordered.data(), ordered.size());
	}
	CommitSceneItemChange(state, sceneSource);
	obs_source_release(sceneSource);
}

// Structural add/remove undo: ADD and REMOVE are mirror images, so two shared
// primitives are swapped between the undo and redo slots. State keys: the usual
// {canvas, scene, source-uuid}; AddItemFromSnapshot additionally carries
// {sourceData, geometry, order} produced by CaptureItemSnapshot.

// Remove the scene item whose source matches state.source, then emit + persist.
void RemoveItemBySource(const json &state)
{
	obs_source_t *sceneSource = nullptr;
	obs_sceneitem_t *item = nullptr;
	if (!ResolveStateItem(state, sceneSource, item)) {
		return;
	}
	obs_sceneitem_remove(item);
	CommitSceneItemChange(state, sceneSource);
	obs_source_release(sceneSource);
}

// Re-add a removed item from its snapshot. Prefers the still-live source (shared
// source, or a source not yet destroyed) over reloading, so a source is never
// duplicated; only loads from the saved data when the uuid no longer resolves.
// obs_load_source restores the saved uuid (verified) as long as no live source
// holds it, so a remove->undo recreates the SAME uuid and a following redo stays
// valid.
void AddItemFromSnapshot(const json &state)
{
	obs_source_t *sceneSource = ResolveTargetScene(state); // addref'd
	if (!sceneSource) {
		return;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);

	const std::string uuid = OptString(state, "source");
	OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str()); // addref'd or null
	if (!source) {
		const std::string sourceData = OptString(state, "sourceData");
		if (!sourceData.empty()) {
			OBSDataAutoRelease data = obs_data_create_from_json(sourceData.c_str());
			if (data) {
				source = obs_load_source(data); // create-ref
			}
		}
	}
	if (!source) {
		HostLog("[bridge] AddItemFromSnapshot: cannot resolve or load source " + uuid);
		obs_source_release(sceneSource);
		return;
	}

	obs_sceneitem_t *item = obs_scene_add(scene, source); // scene takes its own ref
	if (!item) {
		obs_source_release(sceneSource);
		return;
	}

	if (auto geo = state.find("geometry"); geo != state.end() && geo->is_object()) {
		SetItemGeometry(item, *geo);
	}

	// Restore stacking order (index 0 == first_item; out-of-range clamps).
	if (auto it = state.find("order"); it != state.end() && it->is_number_integer()) {
		int pos = it->get<int>();
		obs_sceneitem_set_order_position(item, pos < 0 ? 0 : pos);
	}

	CommitSceneItemChange(state, sceneSource);
	obs_source_release(sceneSource);
}

// Capture everything AddItemFromSnapshot needs to recreate `item`: the
// re-resolution keys, the full serialized source (so it survives destruction),
// its geometry (info2 + crop + visible + locked), and its stacking index.
json CaptureItemSnapshot(const json &params, obs_source_t *sceneSource, obs_sceneitem_t *item)
{
	obs_source_t *src = obs_sceneitem_get_source(item); // borrowed
	json s = StateBase(params, src);

	if (src) {
		OBSDataAutoRelease data = obs_save_source(src); // addref'd
		const char *jsonStr = data ? obs_data_get_json(data) : nullptr;
		s["sourceData"] = jsonStr ? std::string(jsonStr) : std::string();
	}

	obs_transform_info info;
	obs_sceneitem_get_info2(item, &info);
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	s["geometry"] = json{
		{"pos", {{"x", info.pos.x}, {"y", info.pos.y}}},
		{"rot", info.rot},
		{"scale", {{"x", info.scale.x}, {"y", info.scale.y}}},
		{"alignment", info.alignment},
		{"boundsType", static_cast<int>(info.bounds_type)},
		{"boundsAlignment", info.bounds_alignment},
		{"bounds", {{"x", info.bounds.x}, {"y", info.bounds.y}}},
		{"cropToBounds", info.crop_to_bounds},
		{"crop", {{"left", crop.left}, {"top", crop.top}, {"right", crop.right}, {"bottom", crop.bottom}}},
		{"visible", obs_sceneitem_visible(item)},
		{"locked", obs_sceneitem_locked(item)},
	};

	// 0-based index in native (bottom-to-top) enumeration; first_item == 0.
	struct OrderCtx {
		obs_sceneitem_t *target;
		int idx;
		int found;
	} octx{item, 0, -1};
	obs_scene_enum_items(
		obs_scene_from_source(sceneSource),
		[](obs_scene_t *, obs_sceneitem_t *it, void *param) -> bool {
			auto *c = static_cast<OrderCtx *>(param);
			if (it == c->target) {
				c->found = c->idx;
				return false; // stop
			}
			c->idx++;
			return true;
		},
		&octx);
	s["order"] = octx.found < 0 ? 0 : octx.found;
	return s;
}

// Cb adapters: parse the payload, then dispatch to the matching primitive.
const UndoManager::Cb kAddItemFromSnapshot = [](const std::string &d) {
	json s = json::parse(d, nullptr, false);
	if (!s.is_discarded()) {
		AddItemFromSnapshot(s);
	}
};
const UndoManager::Cb kRemoveItemBySource = [](const std::string &d) {
	json s = json::parse(d, nullptr, false);
	if (!s.is_discarded()) {
		RemoveItemBySource(s);
	}
};

// Find any scene other than `target` to use as a fallback before removing a scene
// that might be the active program/channel-0 scene -- `canvas == nullptr` scopes
// the search to the main canvas (obs_enum_scenes), otherwise to that canvas's own
// scenes (obs_canvas_enum_scenes). Returned addref'd; null if `target` is the only
// scene in scope.
obs_source_t *FindFallbackScene(obs_canvas_t *canvas, obs_source_t *target)
{
	struct Ctx {
		obs_source_t *target;
		obs_source_t *fallback = nullptr; // addref'd
	} ctx{target};
	auto proc = [](void *param, obs_source_t *source) -> bool {
		auto *c = static_cast<Ctx *>(param);
		if (source != c->target && !c->fallback) {
			obs_source_get_ref(source);
			c->fallback = source;
		}
		return !c->fallback;
	};
	if (canvas) {
		obs_canvas_enum_scenes(canvas, proc, &ctx);
	} else {
		obs_enum_scenes(proc, &ctx);
	}
	return ctx.fallback;
}

// Undo for scenes.duplicateToCanvas: remove the duplicated scene and every one of
// its duplicated child sources by uuid, in one step (not the per-item undo used
// elsewhere -- this is a genuinely new grouped/composite undo shape).
void RemoveDuplicatedCanvasScene(const json &state)
{
	const std::string newSceneUuid = OptString(state, "newSceneUuid");
	const std::string destCanvasUuid = OptString(state, "destCanvas");

	OBSSourceAutoRelease sceneSource = obs_get_source_by_uuid(newSceneUuid.c_str());
	if (sceneSource && obs_scene_from_source(sceneSource)) {
		// If the target is the active program/channel-0 scene, switch to a
		// fallback first -- mirrors MethodScenesRemove's safeguard ("the target
		// is about to be removed"), scoped to whichever canvas it lives on.
		const bool isMainCanvas = destCanvasUuid.empty() ||
					  destCanvasUuid == ObsBootstrap::Canvases().Default().uuid;
		if (isMainCanvas) {
			OBSSourceAutoRelease current = Transitions::GetProgramScene();
			if (current && current == sceneSource) {
				OBSSourceAutoRelease fallback = FindFallbackScene(nullptr, sceneSource);
				if (fallback) {
					Transitions::SetProgramScene(fallback, false);
				}
			}
		} else if (obs_canvas_t *canvas = ObsBootstrap::CanvasRuntime().Find(destCanvasUuid)) {
			OBSSourceAutoRelease current = obs_canvas_get_channel(canvas, 0);
			if (current && current == sceneSource) {
				OBSSourceAutoRelease fallback = FindFallbackScene(canvas, sceneSource);
				if (fallback) {
					obs_canvas_set_channel(canvas, 0, fallback);
				}
			}
		}
		obs_source_remove(sceneSource);
	}

	if (auto it = state.find("sources"); it != state.end() && it->is_array()) {
		for (const auto &entry : *it) {
			const std::string srcUuid = entry.value("uuid", std::string());
			if (srcUuid.empty()) {
				continue;
			}
			OBSSourceAutoRelease src = obs_get_source_by_uuid(srcUuid.c_str());
			if (src) {
				obs_source_remove(src);
			}
		}
	}

	ObsBootstrap::PruneSceneLinksForCanvasScene(destCanvasUuid, newSceneUuid); // no-op if none was set
	EmitScenesChanged(destCanvasUuid.empty() || destCanvasUuid == ObsBootstrap::Canvases().Default().uuid
				  ? std::string()
				  : destCanvasUuid);
	SceneCollection::Save();
}

// Redo for scenes.duplicateToCanvas: restore the scene and its child sources from
// their captured obs_save_source data, preserving the SAME uuids captured at
// initial-duplicate time (obs_load_source restores the saved uuid as long as
// nothing currently holds it -- same contract AddItemFromSnapshot already relies
// on), then re-place the scene on its destination canvas.
//
// obs_load_source alone does NOT populate a scene's item list: scene_create
// ignores its settings, and the item list is only built by the .load callback
// (scene_load), which nothing but obs_load_sources's own two-pass loop invokes.
// So this mirrors that loop: create every child source first and hold all of
// them alive (in `restoredSources`, for the rest of this function) so they're
// still resolvable by uuid, THEN explicitly trigger every created source's
// .load callback via obs_source_load2 -- including restoredScene itself and
// any restoredSources entry that is itself a nested scene/group (from an
// OBS_SCENE_DUP_COPY duplicate), which otherwise ends up with an empty item
// list of its own.
void RestoreDuplicatedCanvasScene(const json &state)
{
	std::vector<OBSSourceAutoRelease> restoredSources;
	if (auto it = state.find("sources"); it != state.end() && it->is_array()) {
		for (const auto &entry : *it) {
			const std::string sourceData = entry.value("sourceData", std::string());
			if (sourceData.empty()) {
				continue;
			}
			OBSDataAutoRelease data = obs_data_create_from_json(sourceData.c_str());
			if (data) {
				restoredSources.emplace_back(obs_load_source(data)); // create-ref
			}
		}
	}

	const std::string sceneData = OptString(state, "sceneData");
	if (sceneData.empty()) {
		HostLog("[bridge] RestoreDuplicatedCanvasScene: empty sceneData, cannot restore scene");
		return;
	}
	OBSDataAutoRelease data = obs_data_create_from_json(sceneData.c_str());
	if (!data) {
		HostLog("[bridge] RestoreDuplicatedCanvasScene: failed to parse sceneData");
		return;
	}
	OBSSourceAutoRelease restoredScene = obs_load_source(data); // create-ref
	if (!restoredScene || !obs_scene_from_source(restoredScene)) {
		HostLog("[bridge] RestoreDuplicatedCanvasScene: obs_load_source failed to recreate the scene");
		return;
	}
	// Load every child source first (matching obs_load_sources's two-pass order) so
	// that any of them which is itself a nested scene/group gets its own item list
	// populated, then the top-level scene.
	for (auto &src : restoredSources) {
		if (src) {
			obs_source_load2(src);
		}
	}
	obs_source_load2(restoredScene); // fires scene_load, populating items from restoredSources above

	const std::string destCanvasUuid = OptString(state, "destCanvas");
	ResolvedDestCanvas dest = ResolveDestCanvas(destCanvasUuid);
	if (dest.canvas) {
		obs_canvas_move_scene(obs_scene_from_source(restoredScene), dest.canvas);
	}

	EmitScenesChanged(destCanvasUuid.empty() || destCanvasUuid == ObsBootstrap::Canvases().Default().uuid
				  ? std::string()
				  : destCanvasUuid);
	SceneCollection::Save();
}

const UndoManager::Cb kUndoDuplicateSceneToCanvas = [](const std::string &d) {
	json s = json::parse(d, nullptr, false);
	if (!s.is_discarded()) {
		RemoveDuplicatedCanvasScene(s);
	}
};
const UndoManager::Cb kRedoDuplicateSceneToCanvas = [](const std::string &d) {
	json s = json::parse(d, nullptr, false);
	if (!s.is_discarded()) {
		RestoreDuplicatedCanvasScene(s);
	}
};

// {type, duration} for a per-item show/hide transition, or null when unset.
// Shared by sceneItems.list and sceneItems.setShowTransition/setHideTransition.
json SceneItemTransitionJson(obs_sceneitem_t *item, bool show)
{
	obs_source_t *transition = obs_sceneitem_get_transition(item, show); // borrowed
	if (!transition) {
		return nullptr;
	}
	return json{
		{"type", obs_source_get_unversioned_id(transition)},
		{"duration", obs_sceneitem_get_transition_duration(item, show)},
	};
}

bool MethodSceneItemsList(const json &params, json &result, std::string &error)
{
	obs_source_t *sceneSource = ResolveTargetScene(params); // addref'd
	if (!sceneSource) {
		error = "no scene to list";
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);

	// obs_scene_enum_items yields bottom-to-top; we build top-first (topmost
	// draw-order source at index 0) to match the OBS Sources-list convention.
	json items = json::array();
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
			auto *arr = static_cast<json *>(param);
			obs_source_t *src = obs_sceneitem_get_source(item);
			const char *srcName = src ? obs_source_get_name(src) : nullptr;
			// Row color tag lives in the item's private settings under "color"
			// (hex string, "" when unset). See sceneItems.setColor.
			OBSDataAutoRelease priv = obs_sceneitem_get_private_settings(item);
			const char *color = priv ? obs_data_get_string(priv, "color") : "";
			// Prepend to invert bottom-first enumeration into top-first.
			arr->insert(arr->begin(),
				    json{
					    {"id", obs_sceneitem_get_id(item)},
					    {"source", srcName ? json(srcName) : json(nullptr)},
					    {"visible", obs_sceneitem_visible(item)},
					    {"locked", obs_sceneitem_locked(item)},
					    {"scaleFilter", ScaleFilterToToken(obs_sceneitem_get_scale_filter(item))},
					    {"blendMode", BlendModeToToken(obs_sceneitem_get_blending_mode(item))},
					    {"blendMethod",
					     BlendMethodToToken(obs_sceneitem_get_blending_method(item))},
					    {"interactive",
					     src ? ((obs_source_get_output_flags(src) & OBS_SOURCE_INTERACTION) != 0)
						 : false},
					    {"color", color ? color : ""},
					    {"showTransition", SceneItemTransitionJson(item, true)},
					    {"hideTransition", SceneItemTransitionJson(item, false)},
				    });
			return true;
		},
		&items);

	obs_source_release(sceneSource);
	result = std::move(items);
	return true;
}

bool MethodSceneItemsSetVisible(const json &params, json &result, std::string &error)
{
	int64_t id = 0;
	if (!ItemIdFromParams(params, id, error)) {
		return false;
	}
	const bool visible = params.is_object() && params.value("visible", false);
	obs_source_t *sceneSource = ResolveTargetScene(params);
	if (!sceneSource) {
		error = "no scene";
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_sceneitem_t *item = FindSceneItem(scene, id);
	if (!item) {
		obs_source_release(sceneSource);
		error = "no scene item with id " + std::to_string(id);
		return false;
	}
	obs_source_t *itemSrc = obs_sceneitem_get_source(item);
	json before = StateBase(params, itemSrc);
	before["visible"] = obs_sceneitem_visible(item);
	json after = StateBase(params, itemSrc);
	after["visible"] = visible;
	obs_sceneitem_set_visible(item, visible);
	CommitSceneItemChange(params, sceneSource);
	obs_source_release(sceneSource);
	RecordUndo(visible ? "Show" : "Hide", ApplyVisible, before, after);
	result = json{{"id", id}, {"visible", visible}};
	return true;
}

bool MethodSceneItemsSetLocked(const json &params, json &result, std::string &error)
{
	int64_t id = 0;
	if (!ItemIdFromParams(params, id, error)) {
		return false;
	}
	const bool locked = params.is_object() && params.value("locked", false);
	obs_source_t *sceneSource = ResolveTargetScene(params);
	if (!sceneSource) {
		error = "no scene";
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_sceneitem_t *item = FindSceneItem(scene, id);
	if (!item) {
		obs_source_release(sceneSource);
		error = "no scene item with id " + std::to_string(id);
		return false;
	}
	obs_source_t *itemSrc = obs_sceneitem_get_source(item);
	json before = StateBase(params, itemSrc);
	before["locked"] = obs_sceneitem_locked(item);
	json after = StateBase(params, itemSrc);
	after["locked"] = locked;
	obs_sceneitem_set_locked(item, locked);
	CommitSceneItemChange(params, sceneSource);
	obs_source_release(sceneSource);
	RecordUndo(locked ? "Lock" : "Unlock", ApplyLocked, before, after);
	result = json{{"id", id}, {"locked", locked}};
	return true;
}

bool MethodSceneItemsRemove(const json &params, json &result, std::string &error)
{
	int64_t id = 0;
	if (!ItemIdFromParams(params, id, error)) {
		return false;
	}
	obs_source_t *sceneSource = ResolveTargetScene(params);
	if (!sceneSource) {
		error = "no scene";
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_sceneitem_t *item = FindSceneItem(scene, id);
	if (!item) {
		obs_source_release(sceneSource);
		error = "no scene item with id " + std::to_string(id);
		return false;
	}
	obs_source_t *itemSrc = obs_sceneitem_get_source(item); // borrowed
	const char *srcName = itemSrc ? obs_source_get_name(itemSrc) : nullptr;
	const std::string name = srcName ? srcName : "";
	// Snapshot the full item (source data + geometry + order) BEFORE removal so
	// undo can faithfully recreate it; redo just removes by source uuid again.
	const json before = CaptureItemSnapshot(params, sceneSource, item);
	const json after = StateBase(params, itemSrc);

	obs_sceneitem_remove(item);
	CommitSceneItemChange(params, sceneSource);
	obs_source_release(sceneSource);
	// undo == re-add the snapshot; redo == remove by source uuid.
	ObsBootstrap::Undo().AddAction("Remove " + name, kAddItemFromSnapshot, kRemoveItemBySource, before.dump(),
				       after.dump());
	result = json{{"removed", id}};
	return true;
}

bool MethodSceneItemsReorder(const json &params, json &result, std::string &error)
{
	int64_t id = 0;
	if (!ItemIdFromParams(params, id, error)) {
		return false;
	}
	const std::string direction = OptString(params, "direction");
	// Map UI direction -> libobs movement, data-driven.
	struct Move {
		const char *name;
		obs_order_movement movement;
	};
	static const Move kMoves[] = {
		{"up", OBS_ORDER_MOVE_UP},
		{"down", OBS_ORDER_MOVE_DOWN},
		{"top", OBS_ORDER_MOVE_TOP},
		{"bottom", OBS_ORDER_MOVE_BOTTOM},
	};
	const obs_order_movement *movement = nullptr;
	for (const auto &m : kMoves) {
		if (direction == m.name) {
			movement = &m.movement;
			break;
		}
	}
	// Drag-and-drop alternative to `direction`: an explicit target index `to` in
	// the UI's top-first ordering (as returned by sceneItems.list). Same undo
	// record as the direction path, so a multi-slot drag is one undo action.
	int uiTo = -1;
	bool hasTo = false;
	if (auto it = params.find("to"); it != params.end() && it->is_number_integer()) {
		uiTo = it->get<int>();
		hasTo = true;
	}
	if (!movement && !hasTo) {
		error = "reorder needs 'direction' (up|down|top|bottom) or an integer 'to' index";
		return false;
	}

	obs_source_t *sceneSource = ResolveTargetScene(params);
	if (!sceneSource) {
		error = "no scene";
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_sceneitem_t *item = FindSceneItem(scene, id);
	if (!item) {
		obs_source_release(sceneSource);
		error = "no scene item with id " + std::to_string(id);
		return false;
	}
	json before = CaptureOrderState(params, sceneSource);
	if (hasTo) {
		// `to` is a top-first UI index (the sceneItems.list order); libobs order
		// positions are bottom-first, so invert against the item count and clamp
		// a drag past either end back into range.
		const int count = static_cast<int>(before["order"].size());
		int pos = count - 1 - uiTo;
		if (pos < 0) {
			pos = 0;
		}
		if (pos > count - 1) {
			pos = count - 1;
		}
		obs_sceneitem_set_order_position(item, pos);
	} else {
		obs_sceneitem_set_order(item, *movement);
	}
	json after = CaptureOrderState(params, sceneSource);
	CommitSceneItemChange(params, sceneSource);
	obs_source_release(sceneSource);
	RecordUndo("Reorder", ApplyOrder, before, after);
	result = json{{"id", id}, {"direction", direction}, {"to", hasTo ? json(uiTo) : json(nullptr)}};
	return true;
}

bool MethodSceneItemsSetScaleFilter(const json &params, json &result, std::string &error)
{
	int64_t id = 0;
	if (!ItemIdFromParams(params, id, error)) {
		return false;
	}
	const std::string filter = OptString(params, "filter");
	obs_scale_type type;
	if (!ScaleFilterFromToken(filter, type)) {
		error = "'filter' must be one of disable|point|bilinear|bicubic|lanczos|area";
		return false;
	}
	obs_source_t *sceneSource = ResolveTargetScene(params);
	if (!sceneSource) {
		error = "no scene";
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_sceneitem_t *item = FindSceneItem(scene, id);
	if (!item) {
		obs_source_release(sceneSource);
		error = "no scene item with id " + std::to_string(id);
		return false;
	}
	obs_source_t *itemSrc = obs_sceneitem_get_source(item);
	json before = StateBase(params, itemSrc);
	before["filter"] = ScaleFilterToToken(obs_sceneitem_get_scale_filter(item));
	json after = StateBase(params, itemSrc);
	after["filter"] = filter;
	obs_sceneitem_set_scale_filter(item, type);
	CommitSceneItemChange(params, sceneSource);
	obs_source_release(sceneSource);
	RecordUndo("Scale Filtering", ApplyScaleFilter, before, after);
	result = json{{"id", id}, {"filter", filter}};
	return true;
}

bool MethodSceneItemsSetBlendingMode(const json &params, json &result, std::string &error)
{
	int64_t id = 0;
	if (!ItemIdFromParams(params, id, error)) {
		return false;
	}
	const std::string mode = OptString(params, "mode");
	obs_blending_type type;
	if (!BlendModeFromToken(mode, type)) {
		error = "'mode' must be one of normal|additive|subtract|screen|multiply|lighten|darken";
		return false;
	}
	obs_source_t *sceneSource = ResolveTargetScene(params);
	if (!sceneSource) {
		error = "no scene";
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_sceneitem_t *item = FindSceneItem(scene, id);
	if (!item) {
		obs_source_release(sceneSource);
		error = "no scene item with id " + std::to_string(id);
		return false;
	}
	obs_source_t *itemSrc = obs_sceneitem_get_source(item);
	json before = StateBase(params, itemSrc);
	before["mode"] = BlendModeToToken(obs_sceneitem_get_blending_mode(item));
	json after = StateBase(params, itemSrc);
	after["mode"] = mode;
	obs_sceneitem_set_blending_mode(item, type);
	CommitSceneItemChange(params, sceneSource);
	obs_source_release(sceneSource);
	RecordUndo("Blending Mode", ApplyBlendingMode, before, after);
	result = json{{"id", id}, {"mode", mode}};
	return true;
}

bool MethodSceneItemsSetBlendingMethod(const json &params, json &result, std::string &error)
{
	int64_t id = 0;
	if (!ItemIdFromParams(params, id, error)) {
		return false;
	}
	const std::string method = OptString(params, "method");
	obs_blending_method blendMethod;
	if (!BlendMethodFromToken(method, blendMethod)) {
		error = "'method' must be one of default|srgbOff";
		return false;
	}
	obs_source_t *sceneSource = ResolveTargetScene(params);
	if (!sceneSource) {
		error = "no scene";
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_sceneitem_t *item = FindSceneItem(scene, id);
	if (!item) {
		obs_source_release(sceneSource);
		error = "no scene item with id " + std::to_string(id);
		return false;
	}
	obs_source_t *itemSrc = obs_sceneitem_get_source(item);
	json before = StateBase(params, itemSrc);
	before["method"] = BlendMethodToToken(obs_sceneitem_get_blending_method(item));
	json after = StateBase(params, itemSrc);
	after["method"] = method;
	obs_sceneitem_set_blending_method(item, blendMethod);
	CommitSceneItemChange(params, sceneSource);
	obs_source_release(sceneSource);
	RecordUndo("Blending Method", ApplyBlendingMethod, before, after);
	result = json{{"id", id}, {"method", method}};
	return true;
}

// Shared implementation for sceneItems.setShowTransition / setHideTransition.
// params: {scene, id, canvas?, transition: <registered type id, or null/"" to
// clear>, duration: <ms>}. `show` selects which of the item's two independent
// transitions (show vs. hide) is set.
//
// Ownership: obs_sceneitem_set_transition() takes its own ref on the transition
// source (obs_source_get_ref internally) and releases whatever was previously
// set, so the create-ref from obs_source_create_private() must be released
// right after handing it to set_transition -- mirrors the exact pattern
// obs_sceneitem_transition_load() uses when restoring a saved item transition
// (libobs/obs-scene.c). Passing NULL clears the item's transition and releases
// the old one; no separate release call is needed for the clear path.
//
// Persistence: per-item transitions are serialized by libobs itself (scene_save
// _item/scene_load_item -> obs_sceneitem_transition_save/load), so no extra
// wiring is needed here beyond the same CommitSceneItemChange() every sibling
// sceneItems.set* method already calls to persist + notify.
bool SetSceneItemTransition(const json &params, json &result, std::string &error, bool show)
{
	int64_t id = 0;
	if (!ItemIdFromParams(params, id, error)) {
		return false;
	}

	std::string typeId;
	bool clear = true;
	if (auto it = params.find("transition"); it != params.end() && it->is_string()) {
		typeId = it->get<std::string>();
		clear = typeId.empty();
	}

	uint32_t duration = 0;
	if (auto it = params.find("duration"); it != params.end() && it->is_number_integer()) {
		duration = static_cast<uint32_t>(std::max<int64_t>(0, it->get<int64_t>()));
	}

	// Reuse the transitions dock's own type list -- no second enumeration.
	std::string typeName = typeId;
	if (!clear) {
		bool known = false;
		for (const auto &[tid, name] : Transitions::TypeList()) {
			if (tid == typeId) {
				known = true;
				typeName = name;
				break;
			}
		}
		if (!known) {
			error = "unknown transition type '" + typeId + "'";
			return false;
		}
	}

	obs_source_t *sceneSource = ResolveTargetScene(params);
	if (!sceneSource) {
		error = "no scene";
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_sceneitem_t *item = FindSceneItem(scene, id);
	if (!item) {
		obs_source_release(sceneSource);
		error = "no scene item with id " + std::to_string(id);
		return false;
	}

	if (clear) {
		obs_sceneitem_set_transition(item, show, nullptr);
	} else {
		obs_source_t *transition =
			obs_source_create_private(typeId.c_str(), typeName.c_str(), nullptr); // create-ref
		if (!transition) {
			obs_source_release(sceneSource);
			error = "failed to create transition '" + typeId + "'";
			return false;
		}
		obs_sceneitem_set_transition(item, show, transition); // set_transition takes its own ref
		obs_source_release(transition);                       // drop the create-ref
	}
	obs_sceneitem_set_transition_duration(item, show, duration);

	CommitSceneItemChange(params, sceneSource);
	obs_source_release(sceneSource);

	result = json{{"id", id}, {"transition", clear ? json(nullptr) : json(typeId)}, {"duration", duration}};
	return true;
}

bool MethodSceneItemsSetShowTransition(const json &params, json &result, std::string &error)
{
	return SetSceneItemTransition(params, result, error, true);
}

bool MethodSceneItemsSetHideTransition(const json &params, json &result, std::string &error)
{
	return SetSceneItemTransition(params, result, error, false);
}

// Set a row color tag on a scene item. The color is a hex string ("#RRGGBB" /
// "#AARRGGBB"); an empty string clears it. Stored in the item's private settings
// under "color" -- the same store sceneItems.list reads back -- so it persists
// with the scene collection. Resolution mirrors setScaleFilter ({canvas,scene,id}).
bool MethodSceneItemsSetColor(const json &params, json &result, std::string &error)
{
	int64_t id = 0;
	if (!ItemIdFromParams(params, id, error)) {
		return false;
	}
	const std::string color = OptString(params, "color");   // empty string clears the tag
	obs_source_t *sceneSource = ResolveTargetScene(params); // addref'd
	if (!sceneSource) {
		error = "no scene";
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_sceneitem_t *item = FindSceneItem(scene, id);
	if (!item) {
		obs_source_release(sceneSource);
		error = "no scene item with id " + std::to_string(id);
		return false;
	}
	// obs_sceneitem_get_private_settings returns an addref'd ref; OBSDataAutoRelease releases it.
	OBSDataAutoRelease priv = obs_sceneitem_get_private_settings(item);
	obs_data_set_string(priv, "color", color.c_str());
	CommitSceneItemChange(params, sceneSource);
	obs_source_release(sceneSource);
	result = json{{"ok", true}};
	return true;
}

// --- scene-item transform ---------------------------------------------------

// The base (canvas) size a transform op centers/fits against. For an additional
// canvas this is the canvas definition's resolution; otherwise the global video
// info. Returns false only when the global video info is unavailable.
bool ResolveBaseSize(const json &params, uint32_t &baseWidth, uint32_t &baseHeight)
{
	const CanvasTarget target = ResolveCanvasTarget(params);
	if (target.isAdditional) {
		if (CanvasDefinition *def = ObsBootstrap::Canvases().Find(target.uuid)) {
			baseWidth = def->width;
			baseHeight = def->height;
			return true;
		}
	}
	obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		return false;
	}
	baseWidth = ovi.base_width;
	baseHeight = ovi.base_height;
	return true;
}

// Serialize an item's full transform (info2 + crop + source/base sizes) into the
// shape getTransform/setTransform/transformAction all return.
json SceneItemTransformToJson(obs_sceneitem_t *item, uint32_t baseWidth, uint32_t baseHeight)
{
	obs_transform_info info;
	obs_sceneitem_get_info2(item, &info);

	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);

	obs_source_t *src = obs_sceneitem_get_source(item);
	const uint32_t srcW = src ? obs_source_get_width(src) : 0;
	const uint32_t srcH = src ? obs_source_get_height(src) : 0;

	return json{
		{"pos", json{{"x", info.pos.x}, {"y", info.pos.y}}},
		{"rot", info.rot},
		{"scale", json{{"x", info.scale.x}, {"y", info.scale.y}}},
		{"alignment", info.alignment},
		{"boundsType", static_cast<int>(info.bounds_type)},
		{"boundsAlignment", info.bounds_alignment},
		{"bounds", json{{"x", info.bounds.x}, {"y", info.bounds.y}}},
		{"cropToBounds", info.crop_to_bounds},
		{"crop", json{{"left", crop.left}, {"top", crop.top}, {"right", crop.right}, {"bottom", crop.bottom}}},
		{"sourceWidth", srcW},
		{"sourceHeight", srcH},
		{"baseWidth", baseWidth},
		{"baseHeight", baseHeight},
	};
}

// Axis-aligned bounding box of an item's drawn quad, in scene space. Mirrors the
// old frontend's GetItemBox so center math accounts for rotation/bounds.
void GetSceneItemBox(obs_sceneitem_t *item, vec3 &tl, vec3 &br)
{
	matrix4 boxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);

	vec3_set(&tl, M_INFINITE, M_INFINITE, 0.0f);
	vec3_set(&br, -M_INFINITE, -M_INFINITE, 0.0f);

	const float corners[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
	for (const auto &c : corners) {
		vec3 pos;
		vec3_set(&pos, c[0], c[1], 0.0f);
		vec3_transform(&pos, &pos, &boxTransform);
		vec3_min(&tl, &tl, &pos);
		vec3_max(&br, &br, &pos);
	}
}

// After a rotation change has already been committed (box_transform reflects the
// NEW rot), shift pos so the item's visual center matches where it was BEFORE
// the rotation -- overriding libobs's default alignment-anchor pivot with a
// center pivot, so rotating never flings an item far from where it started.
void RepositionForCenterPivot(obs_sceneitem_t *item, const vec3 &beforeTl, const vec3 &beforeBr)
{
	vec3 afterTl, afterBr;
	GetSceneItemBox(item, afterTl, afterBr);

	vec3 centerBefore, centerAfter;
	vec3_set(&centerBefore, (beforeTl.x + beforeBr.x) / 2.0f, (beforeTl.y + beforeBr.y) / 2.0f, 0.0f);
	vec3_set(&centerAfter, (afterTl.x + afterBr.x) / 2.0f, (afterTl.y + afterBr.y) / 2.0f, 0.0f);

	vec3 delta;
	vec3_sub(&delta, &centerBefore, &centerAfter);

	vec2 pos;
	obs_sceneitem_get_pos(item, &pos);
	pos.x += delta.x;
	pos.y += delta.y;
	obs_sceneitem_set_pos(item, &pos);
}

// If the item's bounding box has near-zero overlap with the canvas after a
// transform, nudge it back so at least kMinVisiblePx of it stays reachable --
// prevents an item from becoming invisible/unselectable until Undo.
constexpr float kMinVisiblePx = 32.0f;
void ClampItemToCanvas(obs_sceneitem_t *item, uint32_t baseWidth, uint32_t baseHeight)
{
	vec3 tl, br;
	GetSceneItemBox(item, tl, br);

	const float overlapW = std::min(br.x, float(baseWidth)) - std::max(tl.x, 0.0f);
	const float overlapH = std::min(br.y, float(baseHeight)) - std::max(tl.y, 0.0f);
	if (overlapW > 0.0f && overlapH > 0.0f) {
		return;
	}

	vec2 pos;
	obs_sceneitem_get_pos(item, &pos);
	float dx = 0.0f, dy = 0.0f;

	if (overlapW <= 0.0f) {
		const float itemW = br.x - tl.x;
		const float minX = kMinVisiblePx - itemW;
		const float maxX = float(baseWidth) - kMinVisiblePx;
		if (tl.x < minX) {
			dx = minX - tl.x;
		} else if (tl.x > maxX) {
			dx = maxX - tl.x;
		}
	}
	if (overlapH <= 0.0f) {
		const float itemH = br.y - tl.y;
		const float minY = kMinVisiblePx - itemH;
		const float maxY = float(baseHeight) - kMinVisiblePx;
		if (tl.y < minY) {
			dy = minY - tl.y;
		} else if (tl.y > maxY) {
			dy = maxY - tl.y;
		}
	}

	if (dx != 0.0f || dy != 0.0f) {
		pos.x += dx;
		pos.y += dy;
		obs_sceneitem_set_pos(item, &pos);
	}
}

// Resolve the scene + item shared by all three transform methods. On success
// `sceneSource` is addref'd (caller releases) and `item` is borrowed from it.
bool ResolveTransformTarget(const json &params, obs_source_t *&sceneSource, obs_sceneitem_t *&item, std::string &error)
{
	int64_t id = 0;
	if (!ItemIdFromParams(params, id, error)) {
		return false;
	}
	sceneSource = ResolveTargetScene(params); // addref'd
	if (!sceneSource) {
		error = "no scene";
		return false;
	}
	item = FindSceneItem(obs_scene_from_source(sceneSource), id);
	if (!item) {
		obs_source_release(sceneSource);
		sceneSource = nullptr;
		error = "no scene item with id " + std::to_string(id);
		return false;
	}
	return true;
}

bool MethodSceneItemsGetTransform(const json &params, json &result, std::string &error)
{
	obs_source_t *sceneSource = nullptr;
	obs_sceneitem_t *item = nullptr;
	if (!ResolveTransformTarget(params, sceneSource, item, error)) {
		return false;
	}
	uint32_t baseW = 0, baseH = 0;
	ResolveBaseSize(params, baseW, baseH);
	result = SceneItemTransformToJson(item, baseW, baseH);
	obs_source_release(sceneSource);
	return true;
}

bool MethodSceneItemsSetTransform(const json &params, json &result, std::string &error)
{
	obs_source_t *sceneSource = nullptr;
	obs_sceneitem_t *item = nullptr;
	if (!ResolveTransformTarget(params, sceneSource, item, error)) {
		return false;
	}

	const json undoBefore = CaptureTransformState(params, item);
	obs_source_t *undoSrc = obs_sceneitem_get_source(item);
	const char *undoSrcName = undoSrc ? obs_source_get_name(undoSrc) : nullptr;

	// Partial update: start from the current transform and overlay only the
	// fields the caller supplied so the UI can send just what changed.
	obs_transform_info info;
	obs_sceneitem_get_info2(item, &info);
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	const float rotBefore = info.rot;

	const json *t = nullptr;
	if (params.is_object()) {
		auto it = params.find("transform");
		if (it != params.end() && it->is_object()) {
			t = &*it;
		}
	}
	if (t) {
		auto num = [](const json &o, const char *k, float &out) {
			auto it = o.find(k);
			if (it != o.end() && it->is_number()) {
				out = it->get<float>();
			}
		};
		auto subVec = [&](const char *key, float &x, float &y) {
			auto it = t->find(key);
			if (it != t->end() && it->is_object()) {
				num(*it, "x", x);
				num(*it, "y", y);
			}
		};

		subVec("pos", info.pos.x, info.pos.y);

		float rot = info.rot;
		num(*t, "rot", rot);
		info.rot = rot;

		// Clamp scale to avoid a zero (degenerate) component: keep current.
		float sx = info.scale.x, sy = info.scale.y;
		subVec("scale", sx, sy);
		info.scale.x = (sx != 0.0f) ? sx : info.scale.x;
		info.scale.y = (sy != 0.0f) ? sy : info.scale.y;

		if (auto it = t->find("alignment"); it != t->end() && it->is_number_integer()) {
			info.alignment = it->get<uint32_t>();
		}
		if (auto it = t->find("boundsType"); it != t->end() && it->is_number_integer()) {
			info.bounds_type = static_cast<obs_bounds_type>(it->get<int>());
		}
		if (auto it = t->find("boundsAlignment"); it != t->end() && it->is_number_integer()) {
			info.bounds_alignment = it->get<uint32_t>();
		}
		subVec("bounds", info.bounds.x, info.bounds.y);
		if (auto it = t->find("cropToBounds"); it != t->end() && it->is_boolean()) {
			info.crop_to_bounds = it->get<bool>();
		}

		auto it = t->find("crop");
		if (it != t->end() && it->is_object()) {
			auto cropInt = [&](const char *k, int &out) {
				auto c = it->find(k);
				if (c != it->end() && c->is_number_integer()) {
					out = c->get<int>();
				}
			};
			cropInt("left", crop.left);
			cropInt("top", crop.top);
			cropInt("right", crop.right);
			cropInt("bottom", crop.bottom);
		}
	}

	const bool rotChanged = fabsf(info.rot - rotBefore) > 0.0001f;
	const bool posProvided = t && t->find("pos") != t->end();

	// Capture the box BEFORE any mutation so a rotation change below can be
	// corrected to pivot around the item's visual center (see Step 1).
	vec3 boxBeforeTl, boxBeforeBr;
	if (rotChanged) {
		GetSceneItemBox(item, boxBeforeTl, boxBeforeBr);
	}

	obs_sceneitem_defer_update_begin(item);
	obs_sceneitem_set_info2(item, &info);
	obs_sceneitem_set_crop(item, &crop);
	obs_sceneitem_defer_update_end(item);

	if (rotChanged && !posProvided) {
		RepositionForCenterPivot(item, boxBeforeTl, boxBeforeBr);
	}

	uint32_t clampBaseW = 0, clampBaseH = 0;
	if (ResolveBaseSize(params, clampBaseW, clampBaseH)) {
		ClampItemToCanvas(item, clampBaseW, clampBaseH);
	}

	CommitSceneItemChange(params, sceneSource);

	RecordUndo(std::string("Transform ") + (undoSrcName ? undoSrcName : ""), ApplyTransform, undoBefore,
		   CaptureTransformState(params, item));

	uint32_t baseW = 0, baseH = 0;
	ResolveBaseSize(params, baseW, baseH);
	result = SceneItemTransformToJson(item, baseW, baseH);
	obs_source_release(sceneSource);
	return true;
}

// Center the item along the requested axes in the base canvas, replicating the
// old frontend's CenterSelectedSceneItems: shift the item's axis-aligned box so
// its center lands on the canvas center (accounts for rotation/bounds). Applies
// the offset only on the axes requested so "center horizontally/vertically" move
// a single axis while leaving the other coordinate untouched.
void CenterItemAxis(obs_sceneitem_t *item, uint32_t baseWidth, uint32_t baseHeight, bool horizontal, bool vertical)
{
	vec3 tl, br;
	GetSceneItemBox(item, tl, br);

	vec3 center;
	vec3_set(&center, (tl.x + br.x) / 2.0f, (tl.y + br.y) / 2.0f, 0.0f);

	vec3 screenCenter;
	vec3_set(&screenCenter, float(baseWidth), float(baseHeight), 0.0f);
	vec3_mulf(&screenCenter, &screenCenter, 0.5f);

	vec3 offset;
	vec3_sub(&offset, &screenCenter, &center);

	// Translate the existing top-left by the offset (SetItemTL math).
	vec2 pos;
	obs_sceneitem_get_pos(item, &pos);
	if (horizontal) {
		pos.x += offset.x;
	}
	if (vertical) {
		pos.y += offset.y;
	}
	obs_sceneitem_set_pos(item, &pos);
}

void CenterItemInBase(obs_sceneitem_t *item, uint32_t baseWidth, uint32_t baseHeight)
{
	CenterItemAxis(item, baseWidth, baseHeight, true, true);
}

bool MethodSceneItemsTransformAction(const json &params, json &result, std::string &error)
{
	const std::string action = OptString(params, "action");
	static const char *kActions[] = {"reset",     "center",         "fitToScreen",     "stretchToScreen",
					 "flipH",     "flipV",          "rotate90cw",      "rotate90ccw",
					 "rotate180", "centerVertical", "centerHorizontal"};
	bool known = false;
	for (const char *a : kActions) {
		if (action == a) {
			known = true;
			break;
		}
	}
	if (!known) {
		error = "transformAction 'action' must be one of "
			"reset|center|fitToScreen|stretchToScreen|flipH|flipV|"
			"rotate90cw|rotate90ccw|rotate180|centerVertical|centerHorizontal";
		return false;
	}

	obs_source_t *sceneSource = nullptr;
	obs_sceneitem_t *item = nullptr;
	if (!ResolveTransformTarget(params, sceneSource, item, error)) {
		return false;
	}

	const json undoBefore = CaptureTransformState(params, item);
	obs_source_t *undoSrc = obs_sceneitem_get_source(item);
	const char *undoSrcName = undoSrc ? obs_source_get_name(undoSrc) : nullptr;

	uint32_t baseW = 0, baseH = 0;
	const bool haveBaseSize = ResolveBaseSize(params, baseW, baseH);

	vec3 boxBeforeTl, boxBeforeBr;
	const bool isRotateAction = (action == "rotate90cw" || action == "rotate90ccw" || action == "rotate180");
	if (isRotateAction) {
		GetSceneItemBox(item, boxBeforeTl, boxBeforeBr);
	}

	obs_sceneitem_defer_update_begin(item);

	if (action == "reset") {
		obs_transform_info info;
		vec2_set(&info.pos, 0.0f, 0.0f);
		info.rot = 0.0f;
		vec2_set(&info.scale, 1.0f, 1.0f);
		info.alignment = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
		info.bounds_type = OBS_BOUNDS_NONE;
		info.bounds_alignment = OBS_ALIGN_CENTER;
		vec2_set(&info.bounds, 0.0f, 0.0f);
		info.crop_to_bounds = false;
		obs_sceneitem_set_info2(item, &info);

		obs_sceneitem_crop crop = {};
		obs_sceneitem_set_crop(item, &crop);
	} else if (action == "flipH" || action == "flipV") {
		obs_transform_info info;
		obs_sceneitem_get_info2(item, &info);
		if (action == "flipH") {
			info.scale.x = -info.scale.x;
		} else {
			info.scale.y = -info.scale.y;
		}
		obs_sceneitem_set_info2(item, &info);
	} else if (action == "fitToScreen" || action == "stretchToScreen") {
		// Mirror the old frontend's CenterAlignSelectedItems: identity
		// pos/scale, left/top alignment, bounds = base size, centered.
		obs_transform_info info;
		vec2_set(&info.pos, 0.0f, 0.0f);
		info.rot = 0.0f;
		vec2_set(&info.scale, 1.0f, 1.0f);
		info.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
		info.bounds_type = (action == "fitToScreen") ? OBS_BOUNDS_SCALE_INNER : OBS_BOUNDS_STRETCH;
		info.bounds_alignment = OBS_ALIGN_CENTER;
		vec2_set(&info.bounds, float(baseW), float(baseH));
		info.crop_to_bounds = obs_sceneitem_get_bounds_crop(item);
		obs_sceneitem_set_info2(item, &info);
	} else if (action == "rotate90cw" || action == "rotate90ccw" || action == "rotate180") {
		// Rotation only, matching classic OBS: leave pos/scale untouched and
		// normalize the result to [0, 360).
		float rot = obs_sceneitem_get_rot(item);
		if (action == "rotate90cw") {
			rot += 90.0f;
		} else if (action == "rotate90ccw") {
			rot -= 90.0f;
		} else {
			rot += 180.0f;
		}
		rot = std::fmod(rot, 360.0f);
		if (rot < 0.0f) {
			rot += 360.0f;
		}
		obs_sceneitem_set_rot(item, rot);
	} else if (action == "centerHorizontal" || action == "centerVertical") {
		CenterItemAxis(item, baseW, baseH, action == "centerHorizontal", action == "centerVertical");
	} else { // center
		CenterItemInBase(item, baseW, baseH);
	}

	obs_sceneitem_defer_update_end(item);

	if (isRotateAction) {
		RepositionForCenterPivot(item, boxBeforeTl, boxBeforeBr);
	}

	if (haveBaseSize) {
		ClampItemToCanvas(item, baseW, baseH);
	}

	CommitSceneItemChange(params, sceneSource);

	RecordUndo(std::string("Transform ") + (undoSrcName ? undoSrcName : ""), ApplyTransform, undoBefore,
		   CaptureTransformState(params, item));

	result = SceneItemTransformToJson(item, baseW, baseH);
	obs_source_release(sceneSource);
	return true;
}

// --- source types / source creation -----------------------------------------

// List CREATABLE input source types: id, display name, and coarse capability
// flags. Skips deprecated/disabled types so filters/transitions and obsolete
// inputs are never offered as scene sources. Sorted by display name.
bool MethodSourceTypesList(const json & /*params*/, json &result, std::string & /*error*/)
{
	json types = json::array();
	const char *id = nullptr;
	for (size_t idx = 0; obs_enum_input_types(idx, &id); ++idx) {
		if (!id) {
			continue;
		}
		const uint32_t flags = obs_get_source_output_flags(id);
		// Skip types OBS itself hides from the Add-Source menu.
		if (flags & (OBS_SOURCE_DEPRECATED | OBS_SOURCE_CAP_DISABLED)) {
			continue;
		}
		const char *display = obs_source_get_display_name(id);
		types.push_back(json{
			{"id", id},
			{"name", display ? json(display) : json(id)},
			{"caps",
			 json{
				 {"video", (flags & OBS_SOURCE_VIDEO) != 0},
				 {"audio", (flags & OBS_SOURCE_AUDIO) != 0},
			 }},
		});
	}

	std::sort(types.begin(), types.end(), [](const json &a, const json &b) {
		return a.value("name", std::string()) < b.value("name", std::string());
	});

	result = std::move(types);
	return true;
}

// Create a new input source with default settings and add it to a scene
// (current scene when `scene` is omitted). params: {type, name?, scene?}.
// Returns {id, source} (the new sceneitem id + final source name). Rejects a
// name that collides with an existing source.
bool MethodSourcesCreate(const json &params, json &result, std::string &error)
{
	std::string type;
	if (!RequireStr(params, "sources.create", "type", type, error)) {
		return false;
	}
	std::string name = OptString(params, "name");
	if (name.empty()) {
		const char *display = obs_source_get_display_name(type.c_str());
		name = display ? std::string(display) : type;
	}

	// Reject a duplicate name (a source of any kind collides).
	obs_source_t *clash = obs_get_source_by_name(name.c_str());
	if (clash) {
		obs_source_release(clash);
		error = "a source named '" + name + "' already exists";
		return false;
	}

	obs_source_t *sceneSource = ResolveTargetScene(params); // addref'd
	if (!sceneSource) {
		error = "no scene to add the source to";
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);

	obs_source_t *source = obs_source_create(type.c_str(), name.c_str(), nullptr, nullptr); // create-ref
	if (!source) {
		obs_source_release(sceneSource);
		error = "obs_source_create failed for type '" + type + "'";
		return false;
	}

	obs_sceneitem_t *item = obs_scene_add(scene, source); // scene takes its own ref
	const int64_t itemId = item ? obs_sceneitem_get_id(item) : 0;

	// Capture undo state while the scene + item are still held: undo removes by
	// source uuid, redo re-adds from the snapshot.
	json before, after;
	if (item) {
		before = StateBase(params, source);
		after = CaptureItemSnapshot(params, sceneSource, item);
	}
	obs_source_release(source); // drop the create-ref; scene holds the source

	EmitSceneItemsChanged(sceneSource, ResolveCanvasTarget(params).uuid);

	if (!item) {
		obs_source_release(sceneSource);
		error = "obs_scene_add failed";
		return false;
	}
	PersistSourceState(sceneSource);
	obs_source_release(sceneSource);
	ObsBootstrap::Undo().AddAction("Add " + name, kRemoveItemBySource, kAddItemFromSnapshot, before.dump(),
				       after.dump());
	result = json{{"id", itemId}, {"source", name}};
	return true;
}

// List existing input sources NOT already present in the target scene, so the
// UI can offer "Add existing". params: {scene?}. Each entry carries the source
// name, its type id, and coarse video/audio caps so the UI can pick an icon.
bool MethodSourcesListExisting(const json &params, json &result, std::string &error)
{
	obs_source_t *sceneSource = ResolveTargetScene(params); // addref'd
	if (!sceneSource) {
		error = "no scene";
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);

	// Collect names already in the scene so we can exclude them.
	std::unordered_map<std::string, bool> inScene;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
			obs_source_t *src = obs_sceneitem_get_source(item);
			const char *n = src ? obs_source_get_name(src) : nullptr;
			if (n) {
				(*static_cast<std::unordered_map<std::string, bool> *>(param))[n] = true;
			}
			return true;
		},
		&inScene);

	json sources = json::array();
	struct Ctx {
		json *arr;
		std::unordered_map<std::string, bool> *inScene;
	} ctx{&sources, &inScene};
	obs_enum_sources(
		[](void *param, obs_source_t *source) -> bool {
			auto *c = static_cast<Ctx *>(param);
			const char *n = obs_source_get_name(source);
			if (n && c->inScene->find(n) == c->inScene->end()) {
				const char *typeId = obs_source_get_id(source);
				const uint32_t flags = obs_source_get_output_flags(source);
				c->arr->push_back(json{
					{"name", n},
					{"typeId", typeId ? json(typeId) : json("")},
					{"caps",
					 json{
						 {"video", (flags & OBS_SOURCE_VIDEO) != 0},
						 {"audio", (flags & OBS_SOURCE_AUDIO) != 0},
					 }},
				});
			}
			return true;
		},
		&ctx);

	obs_source_release(sceneSource);
	result = std::move(sources);
	return true;
}

// Add an already-existing source to a scene. params: {name, scene?}.
bool MethodSourcesAddExisting(const json &params, json &result, std::string &error)
{
	std::string name;
	if (!RequireStr(params, "sources.addExisting", "name", name, error)) {
		return false;
	}
	obs_source_t *source = obs_get_source_by_name(name.c_str()); // addref'd
	if (!source) {
		error = "no source named '" + name + "'";
		return false;
	}
	obs_source_t *sceneSource = ResolveTargetScene(params); // addref'd
	if (!sceneSource) {
		obs_source_release(source);
		error = "no scene to add the source to";
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);

	obs_sceneitem_t *item = obs_scene_add(scene, source); // scene takes its own ref
	const int64_t itemId = item ? obs_sceneitem_get_id(item) : 0;

	// Capture undo state while the scene + item are still held; AddItemFromSnapshot
	// will REUSE the pre-existing source via obs_get_source_by_uuid on redo.
	json before, after;
	if (item) {
		before = StateBase(params, source);
		after = CaptureItemSnapshot(params, sceneSource, item);
	}
	obs_source_release(source); // drop our lookup ref

	EmitSceneItemsChanged(sceneSource, ResolveCanvasTarget(params).uuid);

	if (!item) {
		obs_source_release(sceneSource);
		error = "obs_scene_add failed";
		return false;
	}
	PersistSourceState(sceneSource);
	obs_source_release(sceneSource);
	ObsBootstrap::Undo().AddAction("Add " + name, kRemoveItemBySource, kAddItemFromSnapshot, before.dump(),
				       after.dump());
	result = json{{"id", itemId}, {"source", name}};
	return true;
}

// Compute a source name not already taken globally: `base`, then "base 2",
// "base 3", ... Mirrors the dup-name handling in MethodSourcesCreate's collision
// check (any source of any kind collides) but resolves rather than rejects.
std::string UniqueSourceName(const std::string &base)
{
	OBSSourceAutoRelease taken = obs_get_source_by_name(base.c_str());
	if (!taken) {
		return base;
	}
	for (int n = 2;; ++n) {
		std::string candidate = base + " " + std::to_string(n);
		OBSSourceAutoRelease t = obs_get_source_by_name(candidate.c_str());
		if (!t) {
			return candidate;
		}
	}
}

// Duplicate the source of a scene item and add the copy to the SAME scene (powers
// "Paste Duplicate"). params: {scene?, id, canvas?, name?}. The copy is a normal
// (non-private) duplicate so it shows in source lists; its transform is copied so
// it lands in place. Recorded as an Add for undo, exactly like sources.create.
// Returns {id, source}.
bool MethodSourcesDuplicate(const json &params, json &result, std::string &error)
{
	int64_t id = 0;
	if (!ItemIdFromParams(params, id, error)) {
		return false;
	}
	obs_source_t *sceneSource = ResolveTargetScene(params); // addref'd
	if (!sceneSource) {
		error = "no scene";
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_sceneitem_t *item = FindSceneItem(scene, id);
	if (!item) {
		obs_source_release(sceneSource);
		error = "no scene item with id " + std::to_string(id);
		return false;
	}
	obs_source_t *src = obs_sceneitem_get_source(item); // borrowed
	if (!src) {
		obs_source_release(sceneSource);
		error = "scene item has no source";
		return false;
	}

	// New name: explicit `name` if given, else "<src> copy"; either way uniquified.
	std::string base = OptString(params, "name");
	if (base.empty()) {
		const char *srcName = obs_source_get_name(src);
		base = std::string(srcName ? srcName : "Source") + " copy";
	}
	const std::string uniqueName = UniqueSourceName(base);

	// Capture the original item's transform (top-level geometry keys) so the copy
	// lands in place, matching native OBS duplicate behavior.
	const json transform = CaptureTransformState(params, item);

	OBSSourceAutoRelease dup = obs_source_duplicate(src, uniqueName.c_str(), false); // create-ref
	if (!dup) {
		obs_source_release(sceneSource);
		error = "obs_source_duplicate failed";
		return false;
	}

	obs_sceneitem_t *newItem = obs_scene_add(scene, dup); // scene takes its own ref
	const int64_t newId = newItem ? obs_sceneitem_get_id(newItem) : 0;

	// Capture undo state while the scene + item are still held: undo removes by
	// source uuid, redo re-adds from the snapshot (identical to sources.create).
	json before, after;
	if (newItem) {
		SetItemGeometry(newItem, transform);
		before = StateBase(params, dup);
		after = CaptureItemSnapshot(params, sceneSource, newItem);
	}
	// `dup`'s create-ref is dropped by OBSSourceAutoRelease at scope exit; the
	// scene holds its own ref via obs_scene_add.

	EmitSceneItemsChanged(sceneSource, ResolveCanvasTarget(params).uuid);

	if (!newItem) {
		obs_source_release(sceneSource);
		error = "obs_scene_add failed";
		return false;
	}
	PersistSourceState(sceneSource);
	obs_source_release(sceneSource);
	ObsBootstrap::Undo().AddAction("Add " + uniqueName, kRemoveItemBySource, kAddItemFromSnapshot, before.dump(),
				       after.dump());
	result = json{{"id", newId}, {"source", uniqueName}};
	return true;
}

// Shared uuid-preferred / name-fallback source resolver (defined near the audio
// handlers). Reused here so name-addressed duplicate/rename resolve identically.
obs_source_t *ResolveAudioSource(const json &params);

// Duplicate a source addressed by uuid/name and add the copy to the TARGET scene
// (powers cross-scene "Paste (Duplicate)"). params: {uuid?|source?, scene, canvas?,
// name?}. Combines sources.duplicate's uniquified copy with sources.addExisting's
// add-to-target-scene + undo. The copy lands at the scene's default transform; the
// caller applies any carried transform/appearance afterward. Returns {id, source}.
bool MethodSourcesDuplicateInto(const json &params, json &result, std::string &error)
{
	OBSSourceAutoRelease src = ResolveAudioSource(params); // addref'd or null; uuid|name
	if (!src) {
		error = "sources.duplicateInto: no source for the given 'uuid'/'source'";
		return false;
	}
	obs_source_t *sceneSource = ResolveTargetScene(params); // addref'd
	if (!sceneSource) {
		error = "no scene to add the duplicate to";
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);

	// New name: explicit `name` if given, else "<src> copy"; either way uniquified.
	std::string base = OptString(params, "name");
	if (base.empty()) {
		const char *srcName = obs_source_get_name(src);
		base = std::string(srcName ? srcName : "Source") + " copy";
	}
	const std::string uniqueName = UniqueSourceName(base);

	OBSSourceAutoRelease dup = obs_source_duplicate(src, uniqueName.c_str(), false); // create-ref
	if (!dup) {
		obs_source_release(sceneSource);
		error = "obs_source_duplicate failed";
		return false;
	}

	obs_sceneitem_t *newItem = obs_scene_add(scene, dup); // scene takes its own ref
	const int64_t newId = newItem ? obs_sceneitem_get_id(newItem) : 0;

	// Capture undo state while the scene + item are still held (identical to
	// sources.duplicate / sources.addExisting): undo removes by source uuid, redo
	// re-adds from the snapshot. `dup`'s create-ref is dropped at scope exit; the
	// scene holds its own ref via obs_scene_add.
	json before, after;
	if (newItem) {
		before = StateBase(params, dup);
		after = CaptureItemSnapshot(params, sceneSource, newItem);
	}

	EmitSceneItemsChanged(sceneSource, ResolveCanvasTarget(params).uuid);

	if (!newItem) {
		obs_source_release(sceneSource);
		error = "obs_scene_add failed";
		return false;
	}
	PersistSourceState(sceneSource);
	obs_source_release(sceneSource);
	ObsBootstrap::Undo().AddAction("Add " + uniqueName, kRemoveItemBySource, kAddItemFromSnapshot, before.dump(),
				       after.dump());
	result = json{{"id", newId}, {"source", uniqueName}};
	return true;
}

// Group existing scene items into a new group source (powers "Group"). params:
// {scene?, canvas?, ids:[int...], name?}. Resolves each id to an item, skipping
// any that don't resolve or are themselves groups (libobs forbids nesting), and
// requires at least one groupable item. Returns {id, source} = the new group
// item's id + the group source's uuid. NOT wired into undo: group/ungroup are
// structural multi-item ops the current single-item snapshot undo infra cannot
// faithfully invert; a whole-scene snapshot/restore is a separate follow-up.
bool MethodSceneItemsGroup(const json &params, json &result, std::string &error)
{
	if (!params.is_object() || !params.contains("ids") || !params["ids"].is_array() || params["ids"].empty()) {
		error = "sceneItems.group requires a non-empty 'ids' array";
		return false;
	}
	obs_source_t *sceneSource = ResolveTargetScene(params); // addref'd
	if (!sceneSource) {
		error = "no scene";
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);

	// Resolve ids -> items (borrowed; owned by the scene). Drop unresolved ids and
	// existing groups, both of which obs_scene_insert_group would reject outright.
	// Drop duplicate ids too: a repeated pointer passes libobs's guard, but its
	// per-element detach then corrupts the scene's item list (a null-parent write
	// when the item is the scene's first).
	std::vector<obs_sceneitem_t *> items;
	for (const auto &entry : params["ids"]) {
		if (!entry.is_number_integer()) {
			continue;
		}
		obs_sceneitem_t *item = FindSceneItem(scene, entry.get<int64_t>());
		if (item && !obs_sceneitem_is_group(item) &&
		    std::find(items.begin(), items.end(), item) == items.end()) {
			items.push_back(item);
		}
	}
	if (items.empty()) {
		obs_source_release(sceneSource);
		error = "none of the given ids resolved to a groupable scene item";
		return false;
	}

	std::string base = OptString(params, "name");
	if (base.empty()) {
		base = "Group";
	}
	const std::string groupName = UniqueSourceName(base);

	// Groups the EXISTING items into a new group; the returned group item is owned
	// by the scene (borrowed, like obs_scene_add) -- do NOT release it. The items
	// array is borrowed too (libobs only re-links the items, never releases them).
	obs_sceneitem_t *group = obs_scene_insert_group(scene, groupName.c_str(), items.data(), items.size());
	if (!group) {
		obs_source_release(sceneSource);
		error = "obs_scene_insert_group failed";
		return false;
	}
	const int64_t groupId = obs_sceneitem_get_id(group);
	obs_source_t *groupSrc = obs_sceneitem_get_source(group); // borrowed
	const char *groupUuid = groupSrc ? obs_source_get_uuid(groupSrc) : nullptr;
	const std::string uuid = groupUuid ? groupUuid : "";

	CommitSceneItemChange(params, sceneSource);
	obs_source_release(sceneSource);
	result = json{{"id", groupId}, {"source", uuid}};
	return true;
}

// Create a new EMPTY group in the target scene (powers empty-area "New Group").
// params: {scene?, canvas?, name?}. Unlike sceneItems.group (which wraps existing
// ids and rejects an empty list), this adds a fresh empty group via
// obs_scene_add_group. Returns {id, source} = the new group item's id + the group
// source's uuid. NOT wired into undo, for the same reason as sceneItems.group.
bool MethodSceneItemsCreateGroup(const json &params, json &result, std::string &error)
{
	obs_source_t *sceneSource = ResolveTargetScene(params); // addref'd
	if (!sceneSource) {
		error = "no scene";
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);

	std::string base = OptString(params, "name");
	if (base.empty()) {
		base = "Group";
	}
	const std::string groupName = UniqueSourceName(base);

	// The returned group item is owned by the scene (borrowed, like obs_scene_add) --
	// do NOT release it. obs_scene_add_group delegates to obs_scene_insert_group with
	// no members, so the ref semantics match sceneItems.group exactly.
	obs_sceneitem_t *group = obs_scene_add_group(scene, groupName.c_str());
	if (!group) {
		obs_source_release(sceneSource);
		error = "obs_scene_add_group failed";
		return false;
	}
	const int64_t groupId = obs_sceneitem_get_id(group);
	obs_source_t *groupSrc = obs_sceneitem_get_source(group); // borrowed
	const char *groupUuid = groupSrc ? obs_source_get_uuid(groupSrc) : nullptr;
	const std::string uuid = groupUuid ? groupUuid : "";

	CommitSceneItemChange(params, sceneSource);
	obs_source_release(sceneSource);
	result = json{{"id", groupId}, {"source", uuid}};
	return true;
}

// Dissolve a group, reparenting its children back into the scene (powers
// "Ungroup"). params: {scene?, canvas?, id}. Errors if the id isn't a group.
// NOT wired into undo for the same reason as MethodSceneItemsGroup.
bool MethodSceneItemsUngroup(const json &params, json &result, std::string &error)
{
	int64_t id = 0;
	if (!ItemIdFromParams(params, id, error)) {
		return false;
	}
	obs_source_t *sceneSource = ResolveTargetScene(params); // addref'd
	if (!sceneSource) {
		error = "no scene";
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_sceneitem_t *item = FindSceneItem(scene, id);
	if (!item) {
		obs_source_release(sceneSource);
		error = "no scene item with id " + std::to_string(id);
		return false;
	}
	if (!obs_sceneitem_is_group(item)) {
		obs_source_release(sceneSource);
		error = "scene item " + std::to_string(id) + " is not a group";
		return false;
	}

	// Dissolves the group and reparents its children into the scene; releases the
	// scene's own ref on the group item internally, so `item` is invalid after the
	// call. We hold no extra ref on it (FindSceneItem borrows) -- nothing to free.
	obs_sceneitem_group_ungroup(item);

	CommitSceneItemChange(params, sceneSource);
	obs_source_release(sceneSource);
	result = json{{"ungrouped", true}};
	return true;
}

// Rename a scene item's underlying source by scene-item id (works on both the
// global and additional-canvas paths via ResolveTargetScene). params:
// {canvas?, scene?, id, name}. Rejects a clash with a DIFFERENT existing source.
bool MethodSourcesRename(const json &params, json &result, std::string &error)
{
	int64_t id = 0;
	if (!ItemIdFromParams(params, id, error)) {
		return false;
	}
	std::string name;
	if (!RequireStr(params, "sources.rename", "name", name, error)) {
		return false;
	}
	obs_source_t *sceneSource = ResolveTargetScene(params); // addref'd
	if (!sceneSource) {
		error = "no scene";
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_sceneitem_t *item = FindSceneItem(scene, id);
	if (!item) {
		obs_source_release(sceneSource);
		error = "no scene item with id " + std::to_string(id);
		return false;
	}
	obs_source_t *src = obs_sceneitem_get_source(item); // borrowed, no ref
	const char *curName = src ? obs_source_get_name(src) : nullptr;
	if (curName && name == curName) {
		obs_source_release(sceneSource);
		result = json{{"id", id}, {"source", name}};
		return true;
	}
	// Reject a clash with a DIFFERENT existing source (same source is fine above).
	obs_source_t *clash = obs_get_source_by_name(name.c_str()); // addref'd or null
	if (clash) {
		const bool isSelf = clash == src;
		obs_source_release(clash);
		if (!isSelf) {
			obs_source_release(sceneSource);
			error = "a source named '" + name + "' already exists";
			return false;
		}
	}
	const std::string oldName = curName ? curName : "";
	json before = StateBase(params, src);
	before["name"] = oldName;
	json after = StateBase(params, src);
	after["name"] = name;
	obs_source_set_name(src, name.c_str());
	CommitSceneItemChange(params, sceneSource);
	obs_source_release(sceneSource);
	RecordUndo("Rename", ApplyRename, before, after);
	result = json{{"id", id}, {"source", name}};
	return true;
}

// Rename a source addressed by uuid/name rather than a scene-item id (powers the
// audio mixer's Rename, whose rows are sources without a scene-item locator, and
// whose global audio devices are not scene items at all). params: {uuid?|source?,
// name}. Resolves uuid-first via the shared resolver and applies the SAME
// different-source clash rule as sources.rename. Emits audio.changed so the mixer
// picks up the new name. Not scene-scoped, so it uses neither the scene-item undo
// (ApplyRename resolves by scene item) nor EmitSceneItemsChanged. Returns {source}.
bool MethodSourcesRenameByName(const json &params, json &result, std::string &error)
{
	std::string name;
	if (!RequireStr(params, "sources.renameByName", "name", name, error)) {
		return false;
	}
	OBSSourceAutoRelease src = ResolveAudioSource(params); // addref'd or null; uuid|name
	if (!src) {
		error = "sources.renameByName: no source for the given 'uuid'/'source'";
		return false;
	}
	const char *curName = obs_source_get_name(src);
	if (curName && name == curName) {
		result = json{{"source", name}};
		return true;
	}
	// Reject a clash with a DIFFERENT existing source (same source is fine above) --
	// identical rule to sources.rename.
	OBSSourceAutoRelease clash = obs_get_source_by_name(name.c_str()); // addref'd or null
	if (clash && clash.Get() != src.Get()) {
		error = "a source named '" + name + "' already exists";
		return false;
	}
	obs_source_set_name(src, name.c_str());
	EmitEvent(EventNames::kAudioChanged, json::object());
	result = json{{"source", name}};
	return true;
}

// --- missing files (relink) --------------------------------------------------
// Mirrors the legacy "Missing Files" dialog (frontend_old/dialogs/
// OBSMissingFiles + utility/MissingFilesModel): walk every source AND each
// source's filters, ask each for its obs_missing_files set, and apply a
// replacement by issuing the missing-file's own callback. We reuse
// obs_missing_file_issue_callback -- exactly what the legacy saveFiles() calls --
// rather than the lower-level obs_source_replace_missing_file, because the
// type-specific replace callback the latter needs is baked into each
// missing-file entry and is not exposed to callers here.

using MissingFileVisitor = std::function<void(const char *ownerName, obs_missing_file_t *)>;

// Visit each of `src`'s missing files while it (and its container) is alive.
// obs_source_get_missing_files never returns null (it falls back to an empty set).
void VisitSourceMissingFiles(obs_source_t *src, const MissingFileVisitor &visit)
{
	obs_missing_files_t *files = obs_source_get_missing_files(src);
	const char *name = obs_source_get_name(src);
	const size_t count = obs_missing_files_count(files);
	for (size_t i = 0; i < count; i++) {
		obs_missing_file_t *f = obs_missing_files_get_file(files, (int)i);
		visit(name ? name : "", f);
	}
	obs_missing_files_destroy(files);
}

// Walk every source plus each source's filters, applying `visit` to every
// missing file. Both findMissing and relinkMissing share this so they agree on
// exactly which entries exist (filter-owned ones included).
void ForEachMissingFile(const MissingFileVisitor &visit)
{
	obs_enum_all_sources(
		[](void *param, obs_source_t *source) -> bool {
			const auto &v = *static_cast<const MissingFileVisitor *>(param);
			VisitSourceMissingFiles(source, v);
			obs_source_enum_filters(
				source,
				[](obs_source_t *, obs_source_t *filter, void *p) {
					VisitSourceMissingFiles(filter, *static_cast<const MissingFileVisitor *>(p));
				},
				param);
			return true;
		},
		const_cast<MissingFileVisitor *>(&visit));
}

bool MethodSourcesFindMissing(const json & /*params*/, json &result, std::string & /*error*/)
{
	json arr = json::array();
	ForEachMissingFile([&arr](const char *owner, obs_missing_file_t *f) {
		const char *path = obs_missing_file_get_path(f);
		arr.push_back(json{{"source", owner}, {"originalPath", path ? path : ""}});
	});
	result = std::move(arr);
	return true;
}

bool MethodSourcesRelinkMissing(const json &params, json &result, std::string &error)
{
	const std::string source = OptString(params, "source");
	const std::string originalPath = OptString(params, "originalPath");
	if (source.empty() || originalPath.empty()) {
		error = "sources.relinkMissing requires 'source' and 'originalPath'";
		return false;
	}
	const std::string newPath = OptString(params, "newPath"); // empty string clears the reference

	bool applied = false;
	ForEachMissingFile([&](const char *owner, obs_missing_file_t *f) {
		if (applied) {
			return;
		}
		const char *path = obs_missing_file_get_path(f);
		if (source == owner && path && originalPath == path) {
			obs_missing_file_issue_callback(f, newPath.c_str());
			applied = true;
		}
	});
	if (!applied) {
		error = "sources.relinkMissing: no missing file matched the given 'source'/'originalPath'";
		return false;
	}

	SceneCollection::Save();
	EmitScenesChanged(std::string());
	result = json{{"ok", true}};
	return true;
}

// --- properties (generic obs_properties renderer) ---------------------------

// One editable-object kind: how to resolve a `ref` to an obs object, fetch its
// properties + settings, and apply an update. Adding "encoder"/"service"/
// "output" later is one row here, not a new method. The resolver returns an
// addref'd handle (caller releases via `release`); `get_props`/`get_settings`/
// `update` operate on the void* handle.
struct PropertyKind {
	const char *name;
	void *(*resolve)(const std::string &ref);        // addref'd; null if not found
	obs_properties_t *(*get_props)(void *obj);       // caller frees with obs_properties_destroy
	obs_data_t *(*get_settings)(void *obj);          // addref'd; caller releases
	void (*update)(void *obj, obs_data_t *settings); // apply settings
	void (*release)(void *obj);                      // drop the resolve ref
};

// A canvas encoder is a *stored* (non-instantiated) id + obs_data settings living
// in a CanvasDefinition, not a live obs object. The "encoder" PropertyKind below
// resolves a ref of the form "<canvasUuid>:video" | "<canvasUuid>:audio" to this
// context: properties come from the encoder *type* (obs_get_encoder_properties),
// settings are the definition's own obs_data, and updates apply back into it +
// Save + emit canvas.changed. Heap-allocated by resolve, freed by release.
struct EncoderRefCtx {
	std::string canvasUuid;
	bool isVideo = true;
	std::string encoderId;       // e.g. "obs_x264" / "ffmpeg_aac"
	OBSDataAutoRelease settings; // the CanvasDefinition's encoder settings (same obs_data instance)
};

// Parse "<uuid>:video" / "<uuid>:audio". Returns false on malformed refs.
bool ParseEncoderRef(const std::string &ref, std::string &uuid, bool &isVideo)
{
	const size_t colon = ref.find_last_of(':');
	if (colon == std::string::npos || colon == 0 || colon + 1 >= ref.size()) {
		return false;
	}
	uuid = ref.substr(0, colon);
	const std::string which = ref.substr(colon + 1);
	if (which == "video") {
		isVideo = true;
	} else if (which == "audio") {
		isVideo = false;
	} else {
		return false;
	}
	return true;
}

void *ResolveEncoderRef(const std::string &ref)
{
	std::string uuid;
	bool isVideo = true;
	if (!ParseEncoderRef(ref, uuid, isVideo)) {
		return nullptr;
	}
	CanvasDefinition *def = ObsBootstrap::Canvases().Find(uuid);
	if (!def) {
		return nullptr;
	}
	CanvasEncoderDef &enc = isVideo ? def->video : def->audio;
	if (enc.id.empty()) {
		return nullptr; // no encoder chosen for this canvas yet
	}
	// Ensure the settings obs_data exists so props bind + updates persist; seed
	// from the encoder type's defaults the first time.
	if (!enc.settings) {
		enc.settings = obs_encoder_defaults(enc.id.c_str());
		if (!enc.settings) {
			enc.settings = obs_data_create();
		}
	}

	auto *ctx = new EncoderRefCtx();
	ctx->canvasUuid = uuid;
	ctx->isVideo = isVideo;
	ctx->encoderId = enc.id;
	// Share the SAME obs_data instance the definition holds (so applied settings
	// land in the model). OBSDataAutoRelease::operator=(T) takes ownership without
	// addref, so addref explicitly to keep both refs valid.
	obs_data_addref(enc.settings.Get());
	ctx->settings = enc.settings.Get();
	return ctx;
}

// A stream-profile service is a *stored* service id + obs_data settings living in
// a StreamProfile, not a live obs object. The "service" PropertyKind resolves a
// ref of the form "<profileUuid>" to this context: a transient obs_service is
// created (bound to the profile's stored settings) so obs_service_properties and
// its modified-callbacks work; properties come from that instance, settings are
// the profile's own obs_data, and updates apply back into it + Save + emit
// streamProfile.changed. Heap-allocated by resolve, freed by release.
struct ServiceRefCtx {
	std::string profileUuid;
	OBSServiceAutoRelease service; // transient instance bound to the stored settings
	OBSDataAutoRelease settings;   // the StreamProfile's settings (same obs_data instance)
};

void *ResolveServiceRef(const std::string &ref)
{
	StreamProfile *p = ObsBootstrap::StreamProfiles().Find(ref);
	if (!p) {
		return nullptr;
	}
	// Ensure the settings obs_data exists so props bind + updates persist; seed
	// from the service type's defaults the first time.
	if (!p->settings) {
		p->settings = obs_service_defaults(p->serviceId.c_str());
		if (!p->settings) {
			p->settings = obs_data_create();
		}
	}

	// A live obs_service is required for obs_service_properties (and so its
	// modified-callbacks see the stored values). Create one private instance bound
	// to a COPY of the settings -- the service may mutate its own copy, but we apply
	// edits back into the profile's obs_data explicitly on update.
	OBSDataAutoRelease seed = obs_data_create();
	obs_data_apply(seed, p->settings);
	OBSServiceAutoRelease svc = obs_service_create_private(p->serviceId.c_str(), "bridge-service-props", seed);
	if (!svc) {
		return nullptr;
	}

	auto *ctx = new ServiceRefCtx();
	ctx->profileUuid = ref;
	ctx->service = std::move(svc);
	// Share the SAME obs_data instance the profile holds so applied settings land
	// in the model. operator=(T) takes ownership without addref, so addref first.
	obs_data_addref(p->settings.Get());
	ctx->settings = p->settings.Get();
	return ctx;
}

const PropertyKind kPropertyKinds[] = {
	{
		"source",
		[](const std::string &ref) -> void * { return obs_get_source_by_name(ref.c_str()); },
		[](void *obj) -> obs_properties_t * { return obs_source_properties(static_cast<obs_source_t *>(obj)); },
		[](void *obj) -> obs_data_t * { return obs_source_get_settings(static_cast<obs_source_t *>(obj)); },
		[](void *obj, obs_data_t *settings) { obs_source_update(static_cast<obs_source_t *>(obj), settings); },
		[](void *obj) { obs_source_release(static_cast<obs_source_t *>(obj)); },
	},
	{
		// The single active program transition (scene/transitions.cpp) is an
		// obs_source_t, so reuse the source property/update ops. `ref` is ignored --
		// there is exactly one instance -- and resolve hands out an addref'd source
		// the release lambda balances; null (no active transition) surfaces a clean
		// error via ResolvePropertyTarget's not-found path.
		"transition",
		[](const std::string &) -> void * { return Transitions::GetActiveTransition(); },
		[](void *obj) -> obs_properties_t * { return obs_source_properties(static_cast<obs_source_t *>(obj)); },
		[](void *obj) -> obs_data_t * { return obs_source_get_settings(static_cast<obs_source_t *>(obj)); },
		[](void *obj, obs_data_t *settings) {
			obs_source_update(static_cast<obs_source_t *>(obj), settings);
			// Persist the edit (session cache + disk) so it survives a type round-trip
			// and a restart. properties.defaults also routes through this lambda, so a
			// Restore Defaults on the transition persists the cleared settings too.
			Transitions::SaveActiveSettings();
		},
		[](void *obj) { obs_source_release(static_cast<obs_source_t *>(obj)); },
	},
	{
		// A filter is an obs_source_t (type FILTER) in the global uuid registry, so
		// resolve it by uuid and reuse the source property/update ops.
		"filter",
		[](const std::string &ref) -> void * { return obs_get_source_by_uuid(ref.c_str()); },
		[](void *obj) -> obs_properties_t * { return obs_source_properties(static_cast<obs_source_t *>(obj)); },
		[](void *obj) -> obs_data_t * { return obs_source_get_settings(static_cast<obs_source_t *>(obj)); },
		[](void *obj, obs_data_t *settings) { obs_source_update(static_cast<obs_source_t *>(obj), settings); },
		[](void *obj) { obs_source_release(static_cast<obs_source_t *>(obj)); },
	},
	{
		"encoder",
		ResolveEncoderRef,
		[](void *obj) -> obs_properties_t * {
			return obs_get_encoder_properties(static_cast<EncoderRefCtx *>(obj)->encoderId.c_str());
		},
		[](void *obj) -> obs_data_t * {
			obs_data_t *s = static_cast<EncoderRefCtx *>(obj)->settings;
			if (s) {
				obs_data_addref(s);
			}
			return s;
		},
		[](void *obj, obs_data_t *settings) {
			auto *ctx = static_cast<EncoderRefCtx *>(obj);
			// Merge the incoming settings into the definition's stored obs_data
			// (same instance the def holds), then persist + announce.
			obs_data_apply(ctx->settings, settings);
			ObsBootstrap::Canvases().Save();
			EmitEvent(EventNames::kCanvasChanged, json::object());
		},
		[](void *obj) { delete static_cast<EncoderRefCtx *>(obj); },
	},
	{
		"service",
		ResolveServiceRef,
		[](void *obj) -> obs_properties_t * {
			return obs_service_properties(static_cast<ServiceRefCtx *>(obj)->service.Get());
		},
		[](void *obj) -> obs_data_t * {
			obs_data_t *s = static_cast<ServiceRefCtx *>(obj)->settings.Get();
			if (s) {
				obs_data_addref(s);
			}
			return s;
		},
		[](void *obj, obs_data_t *settings) {
			auto *ctx = static_cast<ServiceRefCtx *>(obj);
			// Merge incoming settings into the profile's stored obs_data (same
			// instance the profile holds) AND into the transient service so its
			// modified-callbacks re-evaluate on the next get. Then persist + announce.
			obs_data_apply(ctx->settings, settings);
			obs_service_update(ctx->service.Get(), settings);
			ObsBootstrap::StreamProfiles().Save();
			EmitEvent(EventNames::kStreamProfileChanged, json::object());
		},
		[](void *obj) { delete static_cast<ServiceRefCtx *>(obj); },
	},
};

const PropertyKind *FindPropertyKind(const std::string &kind)
{
	for (const auto &k : kPropertyKinds) {
		if (kind == k.name) {
			return &k;
		}
	}
	return nullptr;
}

// Build {props, values} for an already-resolved object. `props` is the
// serialized descriptor array; `values` is the object's current settings as a
// JSON object (so the form binds the same data the object reads). Frees the
// fetched properties; does NOT release `obj`.
// Flatten a serialized property tree into a flat name->value map. The per-property
// serializer reads each value via the default-falling-back obs_data_get_*, so this
// map carries a source type's defaults (e.g. Display Capture's method/monitor) that
// obs_data_get_json would omit -- it only emits explicitly-set values, leaving a
// freshly created source's defaulted fields blank in the properties form.
static void CollectPropertyValues(const json &descriptors, json &values)
{
	if (!descriptors.is_array()) {
		return;
	}
	for (const auto &d : descriptors) {
		const auto nameIt = d.find("name");
		const auto valIt = d.find("value");
		if (nameIt != d.end() && nameIt->is_string() && valIt != d.end()) {
			values[nameIt->get<std::string>()] = *valIt;
		}
		const auto propsIt = d.find("props"); // checkable/normal group children
		if (propsIt != d.end()) {
			CollectPropertyValues(*propsIt, values);
		}
	}
}

bool BuildPropertiesResult(const PropertyKind *kind, void *obj, json &result, std::string &error)
{
	obs_properties_t *props = kind->get_props(obj);
	obs_data_t *settings = kind->get_settings(obj); // addref'd

	// Run each property's modified-callback against the current settings before
	// serializing, mirroring the legacy Qt properties view. This resolves dynamic
	// state (e.g. NVENC hides bitrate/cqp fields per rate_control, services
	// populate server lists) so the form reflects the encoder's real field set
	// rather than every field shown unconditionally. Idempotent and side-effect
	// free for display -- callbacks only re-evaluate visibility/enabled/items.
	if (props && settings) {
		obs_properties_apply_settings(props, settings);
	}

	json descriptors = PropertiesSerializer::SerializeProperties(props, settings);

	// Build the value map from the serialized descriptors (default-aware) rather
	// than obs_data_get_json (set-values only), so a source type's get_defaults
	// values populate the form instead of showing blank until manually touched.
	json values = json::object();
	CollectPropertyValues(descriptors, values);

	if (props) {
		obs_properties_destroy(props);
	}
	if (settings) {
		obs_data_release(settings);
	}

	(void)error;
	result = json{{"props", std::move(descriptors)}, {"values", std::move(values)}};
	return true;
}

// --- native file dialog -------------------------------------------------------

// UTF-8 -> UTF-16. Empty string maps to empty wstring.
std::wstring Utf8ToWide(const std::string &s)
{
	if (s.empty()) {
		return std::wstring();
	}
	const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring out(len > 0 ? len : 0, L'\0');
	if (len > 0) {
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
	}
	return out;
}

// UTF-16 -> UTF-8. Empty input maps to empty string.
std::string WideToUtf8(const wchar_t *s)
{
	if (!s || !*s) {
		return std::string();
	}
	const int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
	std::string out(len > 0 ? len - 1 : 0, '\0');
	if (len > 0) {
		WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), len, nullptr, nullptr);
	}
	return out;
}

// One parsed filter spec; owns its strings so the COMDLG_FILTERSPEC views stay
// valid for the dialog's lifetime.
struct DialogFilterSpec {
	std::wstring name;
	std::wstring pattern;
};

// Parse the OBS filter-string format ("Desc (*.a *.b);;Desc2 (*.c)") into spec
// pairs of {label, "*.a;*.b"} (the Win32 dialog wants patterns ';'-joined).
std::vector<DialogFilterSpec> ParseDialogFilter(const std::string &filter)
{
	std::vector<DialogFilterSpec> specs;
	size_t pos = 0;
	while (pos < filter.size()) {
		size_t end = filter.find(";;", pos);
		const std::string entry = filter.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
		pos = end == std::string::npos ? filter.size() : end + 2;
		if (entry.empty()) {
			continue;
		}

		const size_t paren = entry.find('(');
		std::string label = paren == std::string::npos ? entry : entry.substr(0, paren);
		// Trim trailing whitespace off the label.
		while (!label.empty() && (label.back() == ' ' || label.back() == '\t')) {
			label.pop_back();
		}

		std::string patterns;
		if (paren != std::string::npos) {
			const size_t close = entry.find(')', paren);
			patterns = entry.substr(paren + 1,
						close == std::string::npos ? std::string::npos : close - paren - 1);
		}
		// Patterns are space-separated in OBS form; Win32 wants ';'-separated.
		std::wstring widePatterns;
		size_t pstart = 0;
		while (pstart < patterns.size()) {
			size_t pend = patterns.find(' ', pstart);
			const std::string token =
				patterns.substr(pstart, pend == std::string::npos ? std::string::npos : pend - pstart);
			pstart = pend == std::string::npos ? patterns.size() : pend + 1;
			if (token.empty()) {
				continue;
			}
			if (!widePatterns.empty()) {
				widePatterns += L";";
			}
			widePatterns += Utf8ToWide(token);
		}
		if (widePatterns.empty()) {
			widePatterns = L"*.*";
		}

		specs.push_back({Utf8ToWide(label.empty() ? "Files" : label), std::move(widePatterns)});
	}
	return specs;
}

// Native file open/save/directory picker via the modern IFileDialog (COM).
// params: {mode: "open"|"save"|"directory", filter?, defaultPath?, defaultName?}.
// Returns {path: string|null} -- null on cancel. Runs on the CEF UI thread.
bool MethodDialogOpenFile(const json &params, json &result, std::string &error)
{
	const std::string mode = [&]() {
		const std::string m = OptString(params, "mode");
		return m.empty() ? std::string("open") : m;
	}();
	const std::string filter = OptString(params, "filter");
	const std::string defaultPath = OptString(params, "defaultPath");
	const std::string defaultName = OptString(params, "defaultName");
	const bool isSave = mode == "save";
	const bool isDirectory = mode == "directory";

	// The bridge runs on the CEF UI thread; COM may or may not be initialized
	// there. Try the call first; only initialize (and uninitialize) if needed.
	bool comInitialized = false;
	IFileDialog *dialog = nullptr;
	const CLSID clsid = isSave ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
	HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
	if (hr == CO_E_NOTINITIALIZED) {
		if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
			comInitialized = true;
			hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
		}
	}
	if (FAILED(hr) || !dialog) {
		if (comInitialized) {
			CoUninitialize();
		}
		error = "failed to create file dialog";
		return false;
	}

	if (isDirectory) {
		DWORD opts = 0;
		if (SUCCEEDED(dialog->GetOptions(&opts))) {
			dialog->SetOptions(opts | FOS_PICKFOLDERS);
		}
	} else {
		std::vector<DialogFilterSpec> specs = ParseDialogFilter(filter);
		if (specs.empty()) {
			specs.push_back({L"All Files", L"*.*"});
		}
		std::vector<COMDLG_FILTERSPEC> winSpecs;
		winSpecs.reserve(specs.size());
		for (const DialogFilterSpec &s : specs) {
			winSpecs.push_back({s.name.c_str(), s.pattern.c_str()});
		}
		dialog->SetFileTypes(static_cast<UINT>(winSpecs.size()), winSpecs.data());
	}

	if (!defaultPath.empty()) {
		IShellItem *folder = nullptr;
		if (SUCCEEDED(SHCreateItemFromParsingName(Utf8ToWide(defaultPath).c_str(), nullptr,
							  IID_PPV_ARGS(&folder)))) {
			dialog->SetFolder(folder);
			folder->Release();
		}
	}
	if (!defaultName.empty() && !isDirectory) {
		dialog->SetFileName(Utf8ToWide(defaultName).c_str());
	}

	HWND parent = nullptr;
	if (PreviewManager *pm = Preview::Instance()) {
		parent = pm->MainHostHwnd();
	}

	std::string chosen;
	bool cancelled = true;
	bool failed = false;
	const HRESULT showHr = dialog->Show(parent);
	if (SUCCEEDED(showHr)) {
		IShellItem *item = nullptr;
		if (SUCCEEDED(dialog->GetResult(&item)) && item) {
			PWSTR pathW = nullptr;
			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &pathW)) && pathW) {
				chosen = WideToUtf8(pathW);
				cancelled = false;
				CoTaskMemFree(pathW);
			}
			item->Release();
		}
	} else if (showHr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
		// User cancellation is reported as {path:null}; any other failure is a
		// genuine error so the caller can distinguish them.
		failed = true;
	}

	dialog->Release();
	if (comInitialized) {
		CoUninitialize();
	}

	if (failed) {
		error = "file dialog failed";
		return false;
	}
	result = json{{"path", cancelled ? json(nullptr) : json(chosen)}};
	// Include the byte size of a chosen file so callers can validate against a limit
	// (e.g. the Go Live thumbnail picker refusing >2 MB) before storing the path.
	if (!cancelled && !isDirectory) {
		std::error_code ec;
		const uintmax_t sz = std::filesystem::file_size(std::filesystem::u8path(chosen), ec);
		if (!ec) {
			result["size"] = static_cast<std::uint64_t>(sz);
		}
	}
	return true;
}

// Reveal a wide path in the system file manager: a directory opens directly, a
// file is highlighted inside its containing folder. Shared by shell.revealPath and
// diagnostics.openLogFolder so the ShellExecute reveal lives in one place.
bool RevealInFileManager(const std::wstring &widePath, std::string &error)
{
	const DWORD attrs = GetFileAttributesW(widePath.c_str());
	if (attrs == INVALID_FILE_ATTRIBUTES) {
		error = "path does not exist";
		return false;
	}

	HINSTANCE rc;
	if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
		rc = ShellExecuteW(nullptr, L"open", widePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	} else {
		const std::wstring args = L"/select,\"" + widePath + L"\"";
		rc = ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
	}
	if (reinterpret_cast<INT_PTR>(rc) <= 32) {
		error = "failed to open file manager";
		return false;
	}
	return true;
}

// Reveal a path in the system file manager. A file is highlighted inside its
// containing folder; a directory is opened directly.
bool MethodShellRevealPath(const json &params, json &result, std::string &error)
{
	std::string path;
	if (!RequireStr(params, "shell.revealPath", "path", path, error)) {
		return false;
	}
	if (!RevealInFileManager(Utf8ToWide(path), error)) {
		return false;
	}
	result = json{{"ok", true}};
	return true;
}

// Standard-base64 encoder (RFC 4648) used to inline a small local file as a data
// URI. No standard-alphabet encoder existed in this TU (DecodeBase64 is decode
// only; the OAuth helper is base64url).
std::string EncodeBase64(const std::vector<unsigned char> &in)
{
	static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((in.size() + 2) / 3 * 4);
	size_t i = 0;
	for (; i + 2 < in.size(); i += 3) {
		const uint32_t n = (static_cast<uint32_t>(in[i]) << 16) | (static_cast<uint32_t>(in[i + 1]) << 8) |
				   static_cast<uint32_t>(in[i + 2]);
		out.push_back(tbl[(n >> 18) & 0x3F]);
		out.push_back(tbl[(n >> 12) & 0x3F]);
		out.push_back(tbl[(n >> 6) & 0x3F]);
		out.push_back(tbl[n & 0x3F]);
	}
	const size_t rem = in.size() - i;
	if (rem == 1) {
		const uint32_t n = static_cast<uint32_t>(in[i]) << 16;
		out.push_back(tbl[(n >> 18) & 0x3F]);
		out.push_back(tbl[(n >> 12) & 0x3F]);
		out.push_back('=');
		out.push_back('=');
	} else if (rem == 2) {
		const uint32_t n = (static_cast<uint32_t>(in[i]) << 16) | (static_cast<uint32_t>(in[i + 1]) << 8);
		out.push_back(tbl[(n >> 18) & 0x3F]);
		out.push_back(tbl[(n >> 12) & 0x3F]);
		out.push_back(tbl[(n >> 6) & 0x3F]);
		out.push_back('=');
	}
	return out;
}

// file.readDataUri {path} -> {dataUri:"data:<mime>;base64,..."}. CEF serves the
// app from a custom app:// scheme and refuses to load file:// local resources
// from that origin, so an image preview (e.g. a Go Live thumbnail) must be
// inlined as a data URI. Reads binary, caps the size so the bridge payload stays
// bounded, and infers the MIME from the file extension.
bool MethodFileReadDataUri(const json &params, json &result, std::string &error)
{
	std::string path;
	if (!RequireStr(params, "file.readDataUri", "path", path, error)) {
		return false;
	}

	const std::filesystem::path fsPath = std::filesystem::u8path(path);
	std::error_code ec;
	const uintmax_t size = std::filesystem::file_size(fsPath, ec);
	if (ec) {
		error = "cannot stat file: " + path;
		return false;
	}
	constexpr uintmax_t kMaxBytes = 10u * 1024 * 1024;
	if (size > kMaxBytes) {
		error = "file exceeds 10 MB";
		return false;
	}

	std::ifstream file(fsPath, std::ios::in | std::ios::binary);
	if (!file) {
		error = "cannot open file: " + path;
		return false;
	}
	std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	if (file.bad()) {
		error = "failed to read file: " + path;
		return false;
	}

	std::string ext = fsPath.extension().string();
	for (char &c : ext) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	static const std::unordered_map<std::string, std::string> kMimeByExt = {
		{".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".png", "image/png"},
		{".gif", "image/gif"},  {".webp", "image/webp"}, {".bmp", "image/bmp"},
	};
	auto it = kMimeByExt.find(ext);
	const std::string mime = it != kMimeByExt.end() ? it->second : "application/octet-stream";

	result = json{{"dataUri", "data:" + mime + ";base64," + EncodeBase64(bytes)},
		      {"size", static_cast<std::uint64_t>(size)}};
	return true;
}

// Read+validate {kind, ref} -> resolved object + its kind. Caller releases the
// object via kind->release on success.
bool ResolvePropertyTarget(const json &params, const PropertyKind *&kind, void *&obj, std::string &error)
{
	const std::string kindName = OptString(params, "kind");
	const std::string ref = OptString(params, "ref");
	if (kindName.empty()) {
		error = "properties requires a non-empty 'kind'";
		return false;
	}
	kind = FindPropertyKind(kindName);
	if (!kind) {
		error = "unsupported properties kind: " + kindName;
		return false;
	}
	// An empty `ref` is legal for single-instance kinds (transition); name/uuid
	// kinds resolve it to null below and get the clean not-found error.
	obj = kind->resolve(ref);
	if (!obj) {
		error = "no " + kindName + " named '" + ref + "'";
		return false;
	}
	return true;
}

bool MethodPropertiesGet(const json &params, json &result, std::string &error)
{
	const PropertyKind *kind = nullptr;
	void *obj = nullptr;
	if (!ResolvePropertyTarget(params, kind, obj, error)) {
		return false;
	}
	const bool ok = BuildPropertiesResult(kind, obj, result, error);
	kind->release(obj);
	return ok;
}

bool MethodPropertiesSet(const json &params, json &result, std::string &error)
{
	const PropertyKind *kind = nullptr;
	void *obj = nullptr;
	if (!ResolvePropertyTarget(params, kind, obj, error)) {
		return false;
	}

	// Apply the supplied settings (partial: obs_*_update merges over existing).
	if (params.is_object() && params.contains("settings") && params["settings"].is_object()) {
		const std::string dump = params["settings"].dump();
		obs_data_t *settings = obs_data_create_from_json(dump.c_str());
		if (settings) {
			kind->update(obj, settings);
			obs_data_release(settings);
			// Persist source-setting edits (encoder/service kinds persist their own
			// stores in their update closures). Filters serialize with their parent
			// source, so persist them the same way; PersistSourceState resolves
			// whether that's the scene collection or (for a global audio channel's
			// filter) GlobalAudioChannels.
			const std::string kindName = kind->name;
			if (kindName == "source" || kindName == "filter") {
				PersistSourceState(static_cast<obs_source_t *>(obj));
			}
		}
	}

	// Re-fetch so modified-callbacks' visibility/enabled/option changes reflect
	// in the returned schema + values. obs re-runs them during update + get.
	const bool ok = BuildPropertiesResult(kind, obj, result, error);
	kind->release(obj);
	return ok;
}

bool MethodPropertiesDefaults(const json &params, json &result, std::string &error)
{
	const PropertyKind *kind = nullptr;
	void *obj = nullptr;
	if (!ResolvePropertyTarget(params, kind, obj, error)) {
		return false;
	}
	// Restore Defaults == Qt's obs_data_clear(settings) + update: drop every explicit
	// value so the type's registered defaults take over again, then re-apply so the
	// object's runtime + persisted state reflect it.
	obs_data_t *settings = kind->get_settings(obj); // addref'd
	if (settings) {
		obs_data_clear(settings);
		kind->update(obj, settings);
		obs_data_release(settings);
		const std::string kindName = kind->name;
		if (kindName == "source" || kindName == "filter") {
			PersistSourceState(static_cast<obs_source_t *>(obj));
		}
	}
	const bool ok = BuildPropertiesResult(kind, obj, result, error);
	kind->release(obj);
	return ok;
}

bool MethodPropertiesButton(const json &params, json &result, std::string &error)
{
	const PropertyKind *kind = nullptr;
	void *obj = nullptr;
	if (!ResolvePropertyTarget(params, kind, obj, error)) {
		return false;
	}
	const std::string propName = OptString(params, "prop");
	if (propName.empty()) {
		kind->release(obj);
		error = "properties.button requires a 'prop' name";
		return false;
	}

	obs_properties_t *props = kind->get_props(obj);
	obs_property_t *prop = props ? obs_properties_get(props, propName.c_str()) : nullptr;
	if (!prop) {
		if (props) {
			obs_properties_destroy(props);
		}
		kind->release(obj);
		error = "no property named '" + propName + "'";
		return false;
	}

	// Invoke the button's click callback. It may mutate the property layout; we
	// re-fetch fresh props+values afterward regardless.
	obs_property_button_clicked(prop, obj);
	obs_properties_destroy(props);

	const bool ok = BuildPropertiesResult(kind, obj, result, error);
	kind->release(obj);
	return ok;
}

// --- source filters ----------------------------------------------------------

// Resolve the parent source a filter operates on. params: {source}. Addref'd
// (caller releases). Fills `error` and returns null when the name is empty or
// unknown.
obs_source_t *ResolveFilterParent(const json &params, std::string &error)
{
	std::string name;
	if (!RequireStr(params, "filters", "source", name, error)) {
		return nullptr;
	}
	obs_source_t *parent = obs_get_source_by_name(name.c_str());
	if (!parent) {
		error = "no source named '" + name + "'";
		return nullptr;
	}
	return parent;
}

// List CREATABLE filter types: id, display name, coarse capability flags. Skips
// deprecated/disabled types. params: {kind?: "video"|"audio"|"all"} narrows the
// list to filters capable of that media kind ("all" = no filter). Sorted by name.
bool MethodFilterTypesList(const json &params, json &result, std::string & /*error*/)
{
	const std::string kind = OptString(params, "kind");
	const bool wantVideo = kind == "video";
	const bool wantAudio = kind == "audio";

	json types = json::array();
	const char *id = nullptr;
	for (size_t idx = 0; obs_enum_filter_types(idx, &id); ++idx) {
		if (!id) {
			continue;
		}
		const uint32_t flags = obs_get_source_output_flags(id);
		if (flags & (OBS_SOURCE_DEPRECATED | OBS_SOURCE_CAP_DISABLED)) {
			continue;
		}
		const bool video = (flags & OBS_SOURCE_VIDEO) != 0;
		const bool audio = (flags & OBS_SOURCE_AUDIO) != 0;
		if (wantVideo && !video) {
			continue;
		}
		if (wantAudio && !audio) {
			continue;
		}
		const char *display = obs_source_get_display_name(id);
		types.push_back(json{
			{"id", id},
			{"name", display ? json(display) : json(id)},
			{"video", video},
			{"audio", audio},
		});
	}

	std::sort(types.begin(), types.end(), [](const json &a, const json &b) {
		return a.value("name", std::string()) < b.value("name", std::string());
	});

	result = std::move(types);
	return true;
}

// Context threaded into obs_source_enum_filters to collect the chain in order.
struct FilterEnumCtx {
	json *filters;
};

void CollectFilter(obs_source_t * /*parent*/, obs_source_t *child, void *param)
{
	auto *ctx = static_cast<FilterEnumCtx *>(param);
	const char *name = obs_source_get_name(child);
	const char *id = obs_source_get_id(child);
	const char *uuid = obs_source_get_uuid(child);
	ctx->filters->push_back(json{
		{"name", name ? json(name) : json()},
		{"id", id ? json(id) : json()},
		{"uuid", uuid ? json(uuid) : json()},
		{"enabled", obs_source_enabled(child)},
	});
}

// List the filters on a source in chain order. params: {source}.
bool MethodFiltersList(const json &params, json &result, std::string &error)
{
	obs_source_t *parent = ResolveFilterParent(params, error);
	if (!parent) {
		return false;
	}
	json filters = json::array();
	FilterEnumCtx ctx{&filters};
	obs_source_enum_filters(parent, CollectFilter, &ctx);
	obs_source_release(parent);
	result = std::move(filters);
	return true;
}

// Add a new filter of `type` to a source. params: {source, type, name?}. `name`
// defaults to the type's display name; rejects a name already used on the parent.
// Returns {name, uuid}.
bool MethodFiltersAdd(const json &params, json &result, std::string &error)
{
	obs_source_t *parent = ResolveFilterParent(params, error);
	if (!parent) {
		return false;
	}
	const std::string type = OptString(params, "type");
	if (type.empty()) {
		obs_source_release(parent);
		error = "filters.add requires a non-empty 'type'";
		return false;
	}
	std::string name = OptString(params, "name");
	const bool explicitName = !name.empty();
	if (!explicitName) {
		const char *display = obs_source_get_display_name(type.c_str());
		name = display ? std::string(display) : type;
	}

	OBSSourceAutoRelease existing = obs_source_get_filter_by_name(parent, name.c_str());
	if (existing) {
		if (explicitName) {
			obs_source_release(parent);
			error = "a filter named '" + name + "' already exists on this source";
			return false;
		}
		// Auto-suffix the default name ("Color Correction", "Color Correction 2", ...)
		// until a free name is found, matching native OBS behavior.
		const std::string base = name;
		for (int n = 2;; ++n) {
			std::string candidate = base + " " + std::to_string(n);
			OBSSourceAutoRelease taken = obs_source_get_filter_by_name(parent, candidate.c_str());
			if (taken) {
				continue;
			}
			name = std::move(candidate);
			break;
		}
	}

	obs_source_t *f = obs_source_create(type.c_str(), name.c_str(), nullptr, nullptr);
	if (!f) {
		obs_source_release(parent);
		error = "failed to create filter of type '" + type + "'";
		return false;
	}
	obs_source_filter_add(parent, f);
	const char *uuid = obs_source_get_uuid(f);
	std::string uuidStr = uuid ? std::string(uuid) : std::string();
	obs_source_release(f); // parent now holds the reference
	obs_source_release(parent);

	SceneCollection::Save();
	result = json{{"name", name}, {"uuid", uuidStr}};
	return true;
}

// Remove a filter by name. params: {source, name}. Returns {removed: name}.
bool MethodFiltersRemove(const json &params, json &result, std::string &error)
{
	obs_source_t *parent = ResolveFilterParent(params, error);
	if (!parent) {
		return false;
	}
	const std::string name = OptString(params, "name");
	OBSSourceAutoRelease f = obs_source_get_filter_by_name(parent, name.c_str());
	if (!f) {
		obs_source_release(parent);
		error = "no filter named '" + name + "' on this source";
		return false;
	}
	obs_source_filter_remove(parent, f);
	obs_source_release(parent);

	SceneCollection::Save();
	result = json{{"removed", name}};
	return true;
}

// Enable/disable a filter by name. params: {source, name, enabled}. Returns
// {name, enabled}.
bool MethodFiltersSetEnabled(const json &params, json &result, std::string &error)
{
	obs_source_t *parent = ResolveFilterParent(params, error);
	if (!parent) {
		return false;
	}
	const std::string name = OptString(params, "name");
	OBSSourceAutoRelease f = obs_source_get_filter_by_name(parent, name.c_str());
	if (!f) {
		obs_source_release(parent);
		error = "no filter named '" + name + "' on this source";
		return false;
	}
	const bool enabled = params.is_object() && params.value("enabled", false);
	obs_source_set_enabled(f, enabled);
	obs_source_release(parent);

	SceneCollection::Save();
	result = json{{"name", name}, {"enabled", enabled}};
	return true;
}

// Move a filter within the chain. params: {source, name, direction}, direction
// one of up|down|top|bottom. Returns {name, direction}.
bool MethodFiltersReorder(const json &params, json &result, std::string &error)
{
	const std::string direction = OptString(params, "direction");
	struct Move {
		const char *name;
		obs_order_movement movement;
	};
	static const Move kMoves[] = {
		{"up", OBS_ORDER_MOVE_UP},
		{"down", OBS_ORDER_MOVE_DOWN},
		{"top", OBS_ORDER_MOVE_TOP},
		{"bottom", OBS_ORDER_MOVE_BOTTOM},
	};
	const obs_order_movement *movement = nullptr;
	for (const auto &m : kMoves) {
		if (direction == m.name) {
			movement = &m.movement;
			break;
		}
	}
	if (!movement) {
		error = "reorder 'direction' must be one of up|down|top|bottom";
		return false;
	}

	obs_source_t *parent = ResolveFilterParent(params, error);
	if (!parent) {
		return false;
	}
	const std::string name = OptString(params, "name");
	OBSSourceAutoRelease f = obs_source_get_filter_by_name(parent, name.c_str());
	if (!f) {
		obs_source_release(parent);
		error = "no filter named '" + name + "' on this source";
		return false;
	}
	obs_source_filter_set_order(parent, f, *movement);
	obs_source_release(parent);

	SceneCollection::Save();
	result = json{{"name", name}, {"direction", direction}};
	return true;
}

// Rename a filter. params: {source, name, newName}. Rejects an empty newName or
// a collision with a different filter on the parent. Returns {name: newName}.
bool MethodFiltersRename(const json &params, json &result, std::string &error)
{
	obs_source_t *parent = ResolveFilterParent(params, error);
	if (!parent) {
		return false;
	}
	const std::string name = OptString(params, "name");
	const std::string newName = OptString(params, "newName");
	if (newName.empty()) {
		obs_source_release(parent);
		error = "filters.rename requires a non-empty 'newName'";
		return false;
	}
	OBSSourceAutoRelease f = obs_source_get_filter_by_name(parent, name.c_str());
	if (!f) {
		obs_source_release(parent);
		error = "no filter named '" + name + "' on this source";
		return false;
	}
	OBSSourceAutoRelease clash = obs_source_get_filter_by_name(parent, newName.c_str());
	if (clash && clash.Get() != f.Get()) {
		obs_source_release(parent);
		error = "a filter named '" + newName + "' already exists on this source";
		return false;
	}
	obs_source_set_name(f, newName.c_str());
	obs_source_release(parent);

	SceneCollection::Save();
	result = json{{"name", newName}};
	return true;
}

// Compute a filter name not already used on `parent`: `base`, then "base 2",
// "base 3", ... Mirrors the auto-suffix loop in MethodFiltersAdd.
std::string UniqueFilterName(obs_source_t *parent, const std::string &base)
{
	OBSSourceAutoRelease existing = obs_source_get_filter_by_name(parent, base.c_str());
	if (!existing) {
		return base;
	}
	for (int n = 2;; ++n) {
		std::string candidate = base + " " + std::to_string(n);
		OBSSourceAutoRelease taken = obs_source_get_filter_by_name(parent, candidate.c_str());
		if (!taken) {
			return candidate;
		}
	}
}

// Duplicate a single filter in place. params: {source, name}. Copies the named
// filter's id + settings under a clash-free name (UniqueFilterName, matching
// filters.add) and appends it to the same source. obs_source_duplicate copies
// settings but not the enabled flag, so carry that over explicitly. Returns
// {name, uuid}.
bool MethodFiltersDuplicate(const json &params, json &result, std::string &error)
{
	obs_source_t *parent = ResolveFilterParent(params, error);
	if (!parent) {
		return false;
	}
	const std::string name = OptString(params, "name");
	OBSSourceAutoRelease existing = obs_source_get_filter_by_name(parent, name.c_str());
	if (!existing) {
		obs_source_release(parent);
		error = "no filter named '" + name + "' on this source";
		return false;
	}

	const std::string dupName = UniqueFilterName(parent, name);
	obs_source_t *dup = obs_source_duplicate(existing, dupName.c_str(), false);
	if (!dup) {
		obs_source_release(parent);
		error = "failed to duplicate filter '" + name + "'";
		return false;
	}
	obs_source_set_enabled(dup, obs_source_enabled(existing));
	obs_source_filter_add(parent, dup);
	const char *uuid = obs_source_get_uuid(dup);
	std::string uuidStr = uuid ? std::string(uuid) : std::string();
	obs_source_release(dup); // parent now holds the reference
	obs_source_release(parent);

	SceneCollection::Save();
	result = json{{"name", dupName}, {"uuid", uuidStr}};
	return true;
}

// Serialize a source's entire filter chain to a json array, each element
// {id, name, settings, enabled}, in obs enumeration order (bottom-to-top).
// Read-only; no mutation, no undo. params: {source}. Returns {filters: [...]}.
bool MethodFiltersCopyChain(const json &params, json &result, std::string &error)
{
	obs_source_t *parent = ResolveFilterParent(params, error);
	if (!parent) {
		return false;
	}
	json filters = json::array();
	obs_source_enum_filters(
		parent,
		[](obs_source_t *, obs_source_t *child, void *param) {
			auto *arr = static_cast<json *>(param);
			const char *id = obs_source_get_id(child);
			const char *name = obs_source_get_name(child);
			OBSDataAutoRelease settings = obs_source_get_settings(child); // addref'd
			const char *jsonStr = settings ? obs_data_get_json(settings) : nullptr;
			json parsed = json::object();
			if (jsonStr) {
				json p = json::parse(jsonStr, nullptr, false);
				if (!p.is_discarded()) {
					parsed = std::move(p);
				}
			}
			arr->push_back(json{
				{"id", id ? json(id) : json()},
				{"name", name ? json(name) : json()},
				{"settings", std::move(parsed)},
				{"enabled", obs_source_enabled(child)},
			});
		},
		&filters);
	obs_source_release(parent);
	result = json{{"filters", std::move(filters)}};
	return true;
}

// Paste a serialized filter chain onto a target source. params: {source,
// filters:[{id,name,settings,enabled}]}. Creates each filter (a private source)
// in array order, uniquifying its name against the target; entries whose `id` is
// not a loadable filter type are skipped. NOT undoable -- the undo system covers
// scene-item structure, not filter-chain ops (follow-up). Returns {pasted: count}.
bool MethodFiltersPasteChain(const json &params, json &result, std::string &error)
{
	obs_source_t *parent = ResolveFilterParent(params, error);
	if (!parent) {
		return false;
	}
	const json *filters = nullptr;
	if (params.is_object()) {
		auto it = params.find("filters");
		if (it != params.end() && it->is_array()) {
			filters = &*it;
		}
	}
	if (!filters) {
		obs_source_release(parent);
		error = "filters.pasteChain requires a 'filters' array";
		return false;
	}

	int pasted = 0;
	for (const json &entry : *filters) {
		if (!entry.is_object()) {
			continue;
		}
		const std::string id = OptString(entry, "id");
		if (id.empty()) {
			continue;
		}
		std::string name = OptString(entry, "name");
		if (name.empty()) {
			const char *display = obs_source_get_display_name(id.c_str());
			name = display ? std::string(display) : id;
		}
		name = UniqueFilterName(parent, name);

		OBSDataAutoRelease settings;
		if (auto sit = entry.find("settings"); sit != entry.end() && sit->is_object()) {
			settings = obs_data_create_from_json(sit->dump().c_str());
		}
		// Filters are private sources; null result == unknown/unloadable type -> skip.
		OBSSourceAutoRelease f = obs_source_create_private(id.c_str(), name.c_str(), settings);
		if (!f) {
			continue;
		}
		obs_source_filter_add(parent, f);
		obs_source_set_enabled(f, entry.value("enabled", true));
		++pasted;
	}
	obs_source_release(parent);

	// Mirror the filters.* family: persist via SceneCollection::Save(), no event.
	if (pasted > 0) {
		SceneCollection::Save();
	}
	result = json{{"pasted", pasted}};
	return true;
}

// --- canvases (native multistream encode targets, 4.4.1) --------------------

// Whether a canvas is currently encoding/streaming. Structural edits
// (resolution/fps/encoder) are refused while live, since the canvas's encoders
// are bound to its video mix and resetting it would free the mix out from under
// them (UAF).
bool CanvasIsLive(const std::string &uuid)
{
	return ObsBootstrap::Multistream().IsCanvasLive(uuid);
}

// Map one CanvasDefinition to the bridge's canvas shape. output* is the scaled
// encode size; a stored 0 means "follow base", so it is reported as the base
// dimension. scaleType is the downscale filter applied when output != base.
json CanvasToJson(const CanvasDefinition &def)
{
	return json{
		{"uuid", def.uuid},
		{"name", def.name},
		{"isDefault", def.isDefault},
		{"baseWidth", def.width},
		{"baseHeight", def.height},
		{"outputWidth", def.outputWidth ? def.outputWidth : def.width},
		{"outputHeight", def.outputHeight ? def.outputHeight : def.height},
		{"scaleType", def.scaleType},
		{"fpsNum", def.fpsNum},
		{"fpsDen", def.fpsDen},
		// Per-section inheritance from the Default canvas: resolution/fps (via
		// ToVideoInfo) and each encoder (via the engine's EnsureCanvasEncoders). Flat
		// to mirror the flat videoEncoder/audioEncoder ids; color inheritance stays
		// nested under `color` alongside the color fields it governs.
		{"useDefaultResolution", def.useDefaultResolution},
		{"videoEncoder", def.video.id},
		{"audioEncoder", def.audio.id},
		{"videoUseDefault", def.video.useDefault},
		{"audioUseDefault", def.audio.useDefault},
		// Per-canvas color: format/space/range tokens map 1:1 onto libobs video
		// format/colorspace/range (applied via CanvasDefinition::ToVideoInfo). The
		// sdr/hdr nit levels are persisted but not yet applied to the pipeline.
		{"color",
		 json{
			 {"format", def.color.format},
			 {"space", def.color.space},
			 {"range", def.color.range},
			 {"sdrWhiteLevel", def.color.sdrWhiteLevel},
			 {"hdrNominalPeakLevel", def.color.hdrNominalPeakLevel},
			 {"useDefault", def.color.useDefault},
		 }},
		// Output-gated: a canvas panel is shown only when >=1 enabled output binds
		// it (the Default canvas included -- its panel hides when disabled, which
		// the Qt central-widget preview could never do). Re-evaluated by the UI on
		// canvas.changed / outputBinding.changed.
		{"enabled", ObsBootstrap::OutputBindings().Bindings().AnyEnabledForCanvas(def.uuid)},
	};
}

bool MethodCanvasList(const json & /*params*/, json &result, std::string & /*error*/)
{
	json arr = json::array();
	for (const CanvasDefinition &def : ObsBootstrap::Canvases().Definitions()) {
		arr.push_back(CanvasToJson(def));
	}
	result = std::move(arr);
	return true;
}

// Read an optional positive dimension (caps at 16384, the D3D11 texture max).
// Absent -> keeps `current`. Zero/negative/oversized -> error.
bool ReadCanvasDim(const json &params, const char *key, uint32_t current, uint32_t &out, std::string &error)
{
	auto it = params.find(key);
	if (it == params.end() || it->is_null()) {
		out = current;
		return true;
	}
	if (!it->is_number_integer() && !it->is_number_unsigned()) {
		error = std::string("'") + key + "' must be an integer";
		return false;
	}
	const int64_t v = it->get<int64_t>();
	if (v <= 0 || v > 16384) {
		error = std::string("'") + key + "' must be in 1..16384";
		return false;
	}
	out = uint32_t(v);
	return true;
}

bool ReadCanvasFps(const json &params, const char *key, uint32_t current, uint32_t &out, std::string &error)
{
	auto it = params.find(key);
	if (it == params.end() || it->is_null()) {
		out = current;
		return true;
	}
	if (!it->is_number_integer() && !it->is_number_unsigned()) {
		error = std::string("'") + key + "' must be an integer";
		return false;
	}
	const int64_t v = it->get<int64_t>();
	// Wide enough for fractional rates: den 1001 (29.97/59.94/23.976) and high
	// nums (e.g. 60000). 144000 caps both num and den sanely.
	if (v <= 0 || v > 144000) {
		error = std::string("'") + key + "' must be in 1..144000";
		return false;
	}
	out = uint32_t(v);
	return true;
}

// Optional scaled-output dimension: absent OR explicit 0 -> "follow base" (0). A
// positive value is the real scaled size; oversized/negative -> error.
bool ReadCanvasOutputDim(const json &params, const char *key, uint32_t current, uint32_t &out, std::string &error)
{
	auto it = params.find(key);
	if (it == params.end() || it->is_null()) {
		out = current;
		return true;
	}
	if (!it->is_number_integer() && !it->is_number_unsigned()) {
		error = std::string("'") + key + "' must be an integer";
		return false;
	}
	const int64_t v = it->get<int64_t>();
	if (v < 0 || v > 16384) {
		error = std::string("'") + key + "' must be in 0..16384 (0 = follow base)";
		return false;
	}
	out = uint32_t(v);
	return true;
}

// Optional downscale-filter token. Absent/empty -> keep `current`. Only the
// resampling filters are valid for a canvas; "disable"/"point" are rejected.
bool ReadCanvasScale(const json &params, const char *key, const std::string &current, std::string &out,
		     std::string &error)
{
	const std::string tok = OptString(params, key);
	if (tok.empty()) {
		out = current;
		return true;
	}
	obs_scale_type type;
	if (!ScaleFilterFromToken(tok, type) || type == OBS_SCALE_DISABLE || type == OBS_SCALE_POINT) {
		error = std::string("'") + key + "' must be one of bilinear, bicubic, lanczos, area";
		return false;
	}
	out = tok;
	return true;
}

// Valid per-canvas color tokens -- must stay in sync with CanvasDefinition.cpp's
// VideoFormatFromName / VideoColorSpaceFromName / VideoRangeFromName maps. "RGB"
// is the editor token for the BGRA packed format. Unknown tokens are rejected
// rather than silently coerced to a fallback.
const std::unordered_set<std::string> kColorFormats = {"NV12", "I420", "I444", "I010", "P010", "P216", "P416", "RGB"};
const std::unordered_set<std::string> kColorSpaces = {"601", "709", "2100PQ", "2100HLG", "sRGB"};
const std::unordered_set<std::string> kColorRanges = {"Partial", "Full"};

// Read an optional nested `color` object. Absent/null -> out = current (no change).
// Any present subset of {format,space,range,sdrWhiteLevel,hdrNominalPeakLevel,
// useDefault} overrides the corresponding field; unknown format/space/range tokens
// are rejected with a clear error.
bool ReadCanvasColor(const json &params, const CanvasColorDef &current, CanvasColorDef &out, std::string &error)
{
	out = current;
	auto it = params.find("color");
	if (it == params.end() || it->is_null()) {
		return true;
	}
	if (!it->is_object()) {
		error = "'color' must be an object";
		return false;
	}
	const json &c = *it;
	auto readTok = [&](const char *key, const std::unordered_set<std::string> &valid, std::string &field) -> bool {
		auto f = c.find(key);
		if (f == c.end() || f->is_null()) {
			return true;
		}
		if (!f->is_string()) {
			error = std::string("color.") + key + " must be a string";
			return false;
		}
		const std::string v = f->get<std::string>();
		if (valid.find(v) == valid.end()) {
			error = std::string("color.") + key + " '" + v + "' is not a recognized token";
			return false;
		}
		field = v;
		return true;
	};
	if (!readTok("format", kColorFormats, out.format) || !readTok("space", kColorSpaces, out.space) ||
	    !readTok("range", kColorRanges, out.range)) {
		return false;
	}
	auto readNit = [&](const char *key, uint32_t &field) -> bool {
		auto f = c.find(key);
		if (f == c.end() || f->is_null()) {
			return true;
		}
		if (!f->is_number()) {
			error = std::string("color.") + key + " must be a number";
			return false;
		}
		int64_t v = f->get<int64_t>();
		if (v < 0) {
			v = 0;
		}
		field = (uint32_t)v;
		return true;
	};
	if (!readNit("sdrWhiteLevel", out.sdrWhiteLevel) || !readNit("hdrNominalPeakLevel", out.hdrNominalPeakLevel)) {
		return false;
	}
	auto ud = c.find("useDefault");
	if (ud != c.end() && ud->is_boolean()) {
		out.useDefault = ud->get<bool>();
	}
	return true;
}

// Read an optional boolean; absent/null/non-bool -> keeps `current`.
bool ReadOptBool(const json &params, const char *key, bool current)
{
	auto it = params.find(key);
	if (it == params.end() || !it->is_boolean()) {
		return current;
	}
	return it->get<bool>();
}

bool MethodCanvasCreate(const json &params, json &result, std::string &error)
{
	if (!RequireObject(params, "canvas.create", error)) {
		return false;
	}
	std::string name;
	if (!RequireStr(params, "canvas.create", "name", name, error)) {
		return false;
	}

	CanvasDefinition def;
	def.name = name;
	def.isDefault = false;
	// base* is the edit surface; output*/scaleType are the scaled encode size and
	// downscale filter (output 0 = follow base). Absent output -> stays 0.
	if (!ReadCanvasDim(params, "baseWidth", 1920, def.width, error) ||
	    !ReadCanvasDim(params, "baseHeight", 1080, def.height, error) ||
	    !ReadCanvasOutputDim(params, "outputWidth", 0, def.outputWidth, error) ||
	    !ReadCanvasOutputDim(params, "outputHeight", 0, def.outputHeight, error) ||
	    !ReadCanvasScale(params, "scaleType", def.scaleType, def.scaleType, error) ||
	    !ReadCanvasFps(params, "fpsNum", 60, def.fpsNum, error) ||
	    !ReadCanvasFps(params, "fpsDen", 1, def.fpsDen, error) ||
	    !ReadCanvasColor(params, def.color, def.color, error)) {
		return false;
	}

	// Encoder ids: explicit or the same defaults the legacy / EnsureDefaultEncoders
	// use. Seed their settings blobs from the encoder type defaults.
	const std::string venc = OptString(params, "videoEncoder");
	const std::string aenc = OptString(params, "audioEncoder");
	def.video.id = venc.empty() ? "obs_x264" : venc;
	def.audio.id = aenc.empty() ? "ffmpeg_aac" : aenc;
	def.video.settings = obs_encoder_defaults(def.video.id.c_str());
	def.audio.settings = obs_encoder_defaults(def.audio.id.c_str());

	// Per-section inheritance from the Default canvas (resolution/fps + each encoder);
	// applied via ToVideoInfo / the engine's EnsureCanvasEncoders. color.useDefault is
	// parsed inside ReadCanvasColor above.
	def.useDefaultResolution = ReadOptBool(params, "useDefaultResolution", false);
	def.video.useDefault = ReadOptBool(params, "videoUseDefault", false);
	def.audio.useDefault = ReadOptBool(params, "audioUseDefault", false);

	CanvasStore &store = ObsBootstrap::Canvases();
	const CanvasDefinition &added = store.Add(std::move(def));
	const std::string uuid = added.uuid;
	const bool saved = store.Save();

	// Bring up the live obs_canvas_t mix so the new canvas can encode immediately.
	ObsBootstrap::CanvasRuntime().EnsureCanvas(added);
	ObsBootstrap::CanvasRuntime()
		.EnsureScenes(); // seed the new canvas with a default scene (Task-1 decoupled seeding from EnsureCanvas)

	EmitEvent(EventNames::kCanvasChanged, json::object());
	result = json{{"uuid", uuid}};
	return PersistOrFail(saved, error);
}

bool MethodCanvasUpdate(const json &params, json &result, std::string &error)
{
	if (!RequireObject(params, "canvas.update", error)) {
		return false;
	}
	std::string uuid;
	if (!RequireStr(params, "canvas.update", "uuid", uuid, error)) {
		return false;
	}
	// Read the current def only to fold absent JSON keys to their existing values (the
	// parse helpers seed `out` from these). The domain apply -- diff, live-refusal
	// gate, Default->global-video coupling, commit, reset ordering, encoder
	// invalidation -- lives in CanvasService; this handler is a pure JSON adapter.
	CanvasStore &store = ObsBootstrap::Canvases();
	const CanvasDefinition *def = store.Find(uuid);
	if (!def) {
		error = "no canvas with uuid '" + uuid + "'";
		return false;
	}

	CanvasUpdateRequest req;
	req.uuid = uuid;
	req.name = OptString(params, "name"); // empty -> no change (applied by the service)
	if (!ReadCanvasDim(params, "baseWidth", def->width, req.width, error) ||
	    !ReadCanvasDim(params, "baseHeight", def->height, req.height, error) ||
	    !ReadCanvasOutputDim(params, "outputWidth", def->outputWidth, req.outputWidth, error) ||
	    !ReadCanvasOutputDim(params, "outputHeight", def->outputHeight, req.outputHeight, error) ||
	    !ReadCanvasScale(params, "scaleType", def->scaleType, req.scaleType, error) ||
	    !ReadCanvasFps(params, "fpsNum", def->fpsNum, req.fpsNum, error) ||
	    !ReadCanvasFps(params, "fpsDen", def->fpsDen, req.fpsDen, error)) {
		return false;
	}
	if (!ReadCanvasColor(params, def->color, req.color, error)) {
		return false;
	}
	req.videoEncoderId = OptString(params, "videoEncoder");
	req.audioEncoderId = OptString(params, "audioEncoder");
	req.useDefaultResolution = ReadOptBool(params, "useDefaultResolution", def->useDefaultResolution);
	req.videoUseDefault = ReadOptBool(params, "videoUseDefault", def->video.useDefault);
	req.audioUseDefault = ReadOptBool(params, "audioUseDefault", def->audio.useDefault);

	CanvasUpdateResult r = ObsBootstrap::CanvasService().Update(req);
	if (!r.ok) {
		error = r.error;
		return false;
	}

	EmitEvent(EventNames::kCanvasChanged, json::object());
	result = CanvasToJson(*r.def);
	return true;
}

// Tear down every consumer of a canvas's mix, then the mix itself. Ordering is
// the whole point, since the mix must not be freed while any obs_display draw
// callback can still reach it from the render thread:
//   1. projectors first -- a Canvas/Multiview projector BORROWS the mix without
//      a CanvasRuntime preview ref, so it must be gone before step 2, where the
//      last RemovePreview can already drop the mix via Reconcile;
//   2. preview surfaces on EVERY window (detached windows mint their own
//      windowId, so a per-window destroy would miss them);
//   3. the engine's cached encoder pair (bound to the dying mix), then the mix.
// Destroying an obs_display synchronizes with the render thread, so after 1+2
// no draw callback is in flight. Callers refuse a live canvas before this runs.
void RemoveCanvasMixAndConsumers(const std::string &uuid)
{
	if (ProjectorManager *pj = Projector::Instance()) {
		for (int id : pj->CloseForCanvas(uuid)) {
			EmitEvent(EventNames::kProjectorChanged, json{{"closed", id}});
		}
	}
	if (PreviewManager *pm = Preview::Instance()) {
		pm->DestroyForCanvas(uuid);
	}
	ObsBootstrap::Multistream().InvalidateCanvasEncoders(uuid);
	ObsBootstrap::CanvasRuntime().RemoveCanvas(uuid);
}

bool MethodCanvasRemove(const json &params, json &result, std::string &error)
{
	std::string uuid;
	if (!RequireStr(params, "canvas.remove", "uuid", uuid, error)) {
		return false;
	}
	CanvasStore &store = ObsBootstrap::Canvases();
	const CanvasDefinition *def = store.Find(uuid);
	if (!def) {
		error = "no canvas with uuid '" + uuid + "'";
		return false;
	}
	if (def->isDefault) {
		error = "the default canvas cannot be removed";
		return false;
	}
	// Removing destroys the canvas mix; a live output's encoder is still bound to
	// it, so refuse while live (mirrors the canvas.update guard) rather than free
	// the mix under a running encoder.
	if (CanvasIsLive(uuid)) {
		error = "cannot remove a canvas while it is live";
		return false;
	}
	// A live output is refused above; an idle-but-projected/previewed canvas is
	// not, which is exactly what the consumer reap guards (see the helper).
	RemoveCanvasMixAndConsumers(uuid);

	store.Remove(uuid);
	const bool saved = store.Save();
	ObsBootstrap::PruneSceneLinksForCanvas(uuid);
	EmitEvent(EventNames::kCanvasChanged, json::object());
	result = json{{"removed", uuid}};
	return PersistOrFail(saved, error);
}

bool MethodCanvasReorder(const json &params, json &result, std::string &error)
{
	if (!RequireObject(params, "canvas.reorder", error)) {
		return false;
	}
	auto it = params.find("order");
	if (it == params.end() || !it->is_array()) {
		error = "canvas.reorder 'order' must be an array of uuids";
		return false;
	}
	std::vector<std::string> order;
	for (const json &el : *it) {
		if (el.is_string()) {
			order.push_back(el.get<std::string>());
		}
	}

	CanvasStore &store = ObsBootstrap::Canvases();
	store.Reorder(order);
	const bool saved = store.Save();
	EmitEvent(EventNames::kCanvasChanged, json::object());

	json arr = json::array();
	for (const CanvasDefinition &def : store.Definitions()) {
		arr.push_back(def.uuid);
	}
	result = json{{"order", arr}};
	return PersistOrFail(saved, error);
}

// List registered encoder types of a kind ("video"|"audio") as {id, name}.
// Filters obs_enum_encoder_types by obs_get_encoder_type. Sorted by display name.
bool MethodEncoderTypesList(const json &params, json &result, std::string &error)
{
	const std::string kind = OptString(params, "kind");
	obs_encoder_type want;
	if (kind == "video") {
		want = OBS_ENCODER_VIDEO;
	} else if (kind == "audio") {
		want = OBS_ENCODER_AUDIO;
	} else {
		error = "encoderTypes.list requires 'kind' of 'video' or 'audio'";
		return false;
	}

	json arr = json::array();
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); ++i) {
		if (!id || obs_get_encoder_type(id) != want) {
			continue;
		}
		// Skip internal (texture-based) and deprecated compat variants whose display
		// names duplicate the user-facing encoders, matching upstream Settings.
		if (obs_get_encoder_caps(id) & (OBS_ENCODER_CAP_INTERNAL | OBS_ENCODER_CAP_DEPRECATED)) {
			continue;
		}
		const char *display = obs_encoder_get_display_name(id);
		arr.push_back(json{{"id", id}, {"name", display ? json(display) : json(id)}});
	}
	std::sort(arr.begin(), arr.end(), [](const json &a, const json &b) {
		return a.value("name", std::string()) < b.value("name", std::string());
	});
	result = std::move(arr);
	return true;
}

// --- stream profiles (reusable destination credentials, 4.4.2) --------------

// The full selected service string for display, e.g. "YouTube - RTMPS". For
// rtmp_common this is the profile's stored "service" setting verbatim (unlike
// PlatformName, which strips the " - RTMPS" suffix); WHIP/custom have no such
// string, so fall back to a never-empty label (the server URL when set).
std::string StreamProfileServiceLabel(const StreamProfile &p)
{
	if (p.serviceId == "whip_custom") {
		return "WHIP";
	}
	if (p.serviceId == "rtmp_custom") {
		const char *server = p.settings ? obs_data_get_string(p.settings, "server") : "";
		return (server && *server) ? std::string(server) : std::string("Custom");
	}
	const char *svc = p.settings ? obs_data_get_string(p.settings, "service") : "";
	return (svc && *svc) ? std::string(svc) : p.PlatformName();
}

// Map one StreamProfile to the bridge's profile shape. `platform` is the display
// prefix (e.g. "YouTube"); `service` is the raw service id (rtmp_common etc.);
// `serviceLabel` is the full selected service (e.g. "YouTube - RTMPS").
json StreamProfileToJson(const StreamProfile &p)
{
	return json{
		{"uuid", p.uuid},
		{"label", p.label},
		{"isPrimary", p.isPrimary},
		{"service", p.serviceId},
		{"platform", p.PlatformName()},
		{"serviceLabel", StreamProfileServiceLabel(p)},
		{"accountId", p.accountId},
	};
}

// Ported duplicate guard (legacy OBSBasicSettings::CheckStreamProfileConflicts):
// reject when another profile (not `selfUuid`) shares this one's non-empty stream
// key, OR a case-insensitive identical "Platform - Name" display label. `candidate`
// is the prospective post-edit profile. Fills `error` and returns true on conflict.
bool StreamProfileConflicts(const StreamProfile &candidate, const std::string &selfUuid, std::string &error)
{
	const std::string candKey = candidate.Key();
	const std::string candName = candidate.DisplayName();

	for (const StreamProfile &other : ObsBootstrap::StreamProfiles().Profiles()) {
		if (other.uuid == selfUuid) {
			continue;
		}
		// Only non-empty keys collide; two unset credentials aren't a clash.
		if (!candKey.empty() && candKey == other.Key()) {
			error = "stream key already in use by '" + other.DisplayName() + "'";
			return true;
		}
		if (astrcmpi(candName.c_str(), other.DisplayName().c_str()) == 0) {
			error = "a profile named '" + other.DisplayName() + "' already exists";
			return true;
		}
	}
	return false;
}

// Build an OBSDataAutoRelease from a JSON `settings` sub-object, or null when
// absent/not an object.
OBSDataAutoRelease SettingsFromParams(const json &params)
{
	if (params.is_object() && params.contains("settings") && params["settings"].is_object()) {
		const std::string dump = params["settings"].dump();
		return OBSDataAutoRelease(obs_data_create_from_json(dump.c_str()));
	}
	return OBSDataAutoRelease(nullptr);
}

bool MethodStreamProfileList(const json & /*params*/, json &result, std::string & /*error*/)
{
	json arr = json::array();
	for (const StreamProfile &p : ObsBootstrap::StreamProfiles().Profiles()) {
		arr.push_back(StreamProfileToJson(p));
	}
	result = std::move(arr);
	return true;
}

bool MethodStreamProfileCreate(const json &params, json &result, std::string &error)
{
	if (!RequireObject(params, "streamProfile.create", error)) {
		return false;
	}
	std::string label;
	if (!RequireStr(params, "streamProfile.create", "label", label, error)) {
		return false;
	}

	StreamProfile p;
	p.label = label;
	const std::string service = OptString(params, "service");
	if (!service.empty()) {
		p.serviceId = service;
	}
	p.settings = SettingsFromParams(params);

	// Validate against every existing profile before committing.
	if (StreamProfileConflicts(p, std::string(), error)) {
		return false;
	}

	StreamProfileStore &store = ObsBootstrap::StreamProfiles();
	const std::string uuid = store.Add(std::move(p)).uuid;
	const bool saved = store.Save();

	EmitEvent(EventNames::kStreamProfileChanged, json::object());
	result = json{{"uuid", uuid}};
	return PersistOrFail(saved, error);
}

bool MethodStreamProfileUpdate(const json &params, json &result, std::string &error)
{
	if (!RequireObject(params, "streamProfile.update", error)) {
		return false;
	}
	std::string uuid;
	if (!RequireStr(params, "streamProfile.update", "uuid", uuid, error)) {
		return false;
	}
	StreamProfileStore &store = ObsBootstrap::StreamProfiles();
	StreamProfile *p = store.Find(uuid);
	if (!p) {
		error = "no stream profile with uuid '" + uuid + "'";
		return false;
	}

	// Build the prospective post-edit profile (StreamProfile is move-only, so
	// assemble its fields rather than copying), validate, then commit on success.
	StreamProfile candidate;
	candidate.uuid = uuid;
	candidate.isPrimary = p->isPrimary;

	const std::string label = OptString(params, "label");
	candidate.label = label.empty() ? p->label : label;

	const std::string service = OptString(params, "service");
	const bool serviceChanged = !service.empty() && service != p->serviceId;
	candidate.serviceId = serviceChanged ? service : p->serviceId;

	OBSDataAutoRelease newSettings = SettingsFromParams(params);
	if (newSettings) {
		candidate.settings = std::move(newSettings);
	} else if (serviceChanged) {
		// Switching service id without supplying settings: reset to that service's
		// defaults (the prior blob belongs to a different schema).
		candidate.settings = obs_service_defaults(candidate.serviceId.c_str());
	} else if (p->settings) {
		// Reuse the existing settings: deep-copy so the candidate (and the
		// duplicate-guard Key()/DisplayName()) read the same values without
		// aliasing the live profile's obs_data.
		candidate.settings = obs_data_create();
		obs_data_apply(candidate.settings, p->settings);
	}

	if (StreamProfileConflicts(candidate, uuid, error)) {
		return false;
	}

	p->label = candidate.label;
	p->serviceId = candidate.serviceId;
	p->settings = std::move(candidate.settings);
	const bool saved = store.Save();

	EmitEvent(EventNames::kStreamProfileChanged, json::object());
	result = StreamProfileToJson(*p);
	return PersistOrFail(saved, error);
}

bool MethodStreamProfileRemove(const json &params, json &result, std::string &error)
{
	std::string uuid;
	if (!RequireStr(params, "streamProfile.remove", "uuid", uuid, error)) {
		return false;
	}
	StreamProfileStore &store = ObsBootstrap::StreamProfiles();
	StreamProfile *victim = store.Find(uuid);
	if (!victim) {
		error = "no stream profile with uuid '" + uuid + "'";
		return false;
	}
	// Capture the linked account before the profile is gone; if this was the last
	// profile referencing it, the account is orphaned and gets reclaimed below.
	const std::string linkedAccount = victim->accountId;

	// Remove re-points the primary internally when the primary was removed.
	store.Remove(uuid);
	const bool saved = store.Save();

	// Cascade: drop every output binding that routed this profile so no dangling
	// (deleted) edge lingers for the user to unbind by hand. Only the active
	// collection is pruned in memory; other collections keep the "(deleted)" label
	// until they load (see PruneOutputBindingsForProfile).
	if (ObsBootstrap::PruneOutputBindingsForProfile(uuid) > 0) {
		EmitEvent(EventNames::kOutputBindingChanged, json::object());
	}

	EmitEvent(EventNames::kStreamProfileChanged, json::object());

	// Account lifecycle is profile-owned: an OAuth account is only ever created by a
	// profile's connect flow, so an account no remaining profile references is unowned.
	// Reclaim it through the shared teardown (store + flight lock + chat/events + status)
	// so a deleted profile can't strand an account that keeps surfacing in chat/events.
	if (!store.ReferencesAccount(linkedAccount) && OAuth::Accounts().Get(linkedAccount)) {
		TeardownAccount(linkedAccount);
	}

	result = json{{"removed", uuid}};
	return PersistOrFail(saved, error);
}

bool MethodStreamProfileSetPrimary(const json &params, json &result, std::string &error)
{
	std::string uuid;
	if (!RequireStr(params, "streamProfile.setPrimary", "uuid", uuid, error)) {
		return false;
	}
	StreamProfileStore &store = ObsBootstrap::StreamProfiles();
	if (!store.SetPrimary(uuid)) {
		error = "no stream profile with uuid '" + uuid + "'";
		return false;
	}
	const bool saved = store.Save();

	EmitEvent(EventNames::kStreamProfileChanged, json::object());
	result = json{{"uuid", uuid}, {"isPrimary", true}};
	return PersistOrFail(saved, error);
}

bool MethodStreamProfileReorder(const json &params, json &result, std::string &error)
{
	if (!RequireObject(params, "streamProfile.reorder", error)) {
		return false;
	}
	auto it = params.find("order");
	if (it == params.end() || !it->is_array()) {
		error = "streamProfile.reorder 'order' must be an array of uuids";
		return false;
	}
	std::vector<std::string> order;
	for (const json &el : *it) {
		if (el.is_string()) {
			order.push_back(el.get<std::string>());
		}
	}

	StreamProfileStore &store = ObsBootstrap::StreamProfiles();
	store.Reorder(order);
	const bool saved = store.Save();
	EmitEvent(EventNames::kStreamProfileChanged, json::object());

	json arr = json::array();
	for (const StreamProfile &p : store.Profiles()) {
		arr.push_back(p.uuid);
	}
	result = json{{"order", arr}};
	return PersistOrFail(saved, error);
}

// List registered service types as {id, name} (e.g. rtmp_common, rtmp_custom,
// whip_custom). Sorted by display name.
bool MethodServiceTypesList(const json & /*params*/, json &result, std::string & /*error*/)
{
	json arr = json::array();
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_service_types(i, &id); ++i) {
		if (!id) {
			continue;
		}
		const char *display = obs_service_get_display_name(id);
		arr.push_back(json{{"id", id}, {"name", display ? json(display) : json(id)}});
	}
	std::sort(arr.begin(), arr.end(), [](const json &a, const json &b) {
		return a.value("name", std::string()) < b.value("name", std::string());
	});
	result = std::move(arr);
	return true;
}

// --- output bindings (profile x canvas routing edges, 4.4.3) ----------------

// Resolve a profile uuid to a display label for the join in outputBinding.list.
// Empty uuid -> "(unset)"; a uuid with no live profile -> "(deleted)"; otherwise
// the profile's DisplayName(). This mirrors the legacy "deleted vs unset"
// distinction so a dangling reference reads differently from an empty slot.
std::string ProfileLabelFor(const std::string &profileUuid)
{
	if (profileUuid.empty()) {
		return "(unset)";
	}
	StreamProfile *p = ObsBootstrap::StreamProfiles().Find(profileUuid);
	return p ? p->DisplayName() : "(deleted)";
}

// Resolve a canvas uuid to a display name. Empty uuid -> "(unset)"; a uuid with
// no live canvas -> "(deleted)"; otherwise the canvas name.
std::string CanvasNameFor(const std::string &canvasUuid)
{
	if (canvasUuid.empty()) {
		return "(unset)";
	}
	CanvasDefinition *def = ObsBootstrap::Canvases().Find(canvasUuid);
	return def ? def->name : "(deleted)";
}

// Map one OutputBinding to the bridge's shape, joining its profile/canvas uuids
// to display names through the live stores.
json OutputBindingToJson(const OutputBinding &b)
{
	return json{
		{"uuid", b.uuid},
		{"profileUuid", b.profileUuid},
		{"profileLabel", ProfileLabelFor(b.profileUuid)},
		{"canvasUuid", b.canvasUuid},
		{"canvasName", CanvasNameFor(b.canvasUuid)},
		{"enabled", b.enabled},
	};
}

bool MethodOutputBindingList(const json & /*params*/, json &result, std::string & /*error*/)
{
	json arr = json::array();
	for (const OutputBinding &b : ObsBootstrap::OutputBindings().Bindings().bindings) {
		arr.push_back(OutputBindingToJson(b));
	}
	result = std::move(arr);
	return true;
}

bool MethodOutputBindingCreate(const json &params, json &result, std::string &error)
{
	if (!RequireObject(params, "outputBinding.create", error)) {
		return false;
	}
	std::string canvasUuid;
	if (!RequireStr(params, "outputBinding.create", "canvasUuid", canvasUuid, error)) {
		return false;
	}
	if (!ObsBootstrap::Canvases().Find(canvasUuid)) {
		error = "no canvas with uuid '" + canvasUuid + "'";
		return false;
	}
	const std::string profileUuid = OptString(params, "profileUuid");
	if (!profileUuid.empty() && !ObsBootstrap::StreamProfiles().Find(profileUuid)) {
		error = "no stream profile with uuid '" + profileUuid + "'";
		return false;
	}

	OutputBindings &bindings = ObsBootstrap::OutputBindings().Bindings();

	// Reject an exact duplicate: the same (profile x canvas) pair already bound.
	if (bindings.HasPair(profileUuid, canvasUuid)) {
		error = "that profile is already bound to this canvas";
		return false;
	}

	// A new binding is created enabled, so it must obey the single-live-stream rule:
	// refuse if this profile is already enabled on another canvas (one RTMP key = one
	// live stream). Mirrors the engine-layer ProfileLiveElsewhere guard.
	if (!profileUuid.empty() && bindings.ProfileEnabledElsewhere(std::string(), profileUuid)) {
		error = "that stream profile is already enabled on another canvas";
		return false;
	}

	OutputBinding &created = bindings.Add(canvasUuid);
	created.profileUuid = profileUuid;
	created.enabled = true;
	const std::string uuid = created.uuid;
	const bool saved = ObsBootstrap::OutputBindings().Save();
	ObsBootstrap::CanvasRuntime().ReconcileAll(); // new enabled binding -> its canvas activates

	EmitEvent(EventNames::kOutputBindingChanged, json::object());
	result = json{{"uuid", uuid}};
	return PersistOrFail(saved, error);
}

bool MethodOutputBindingUpdate(const json &params, json &result, std::string &error)
{
	if (!RequireObject(params, "outputBinding.update", error)) {
		return false;
	}
	std::string uuid;
	if (!RequireStr(params, "outputBinding.update", "uuid", uuid, error)) {
		return false;
	}
	OutputBindings &bindings = ObsBootstrap::OutputBindings().Bindings();
	OutputBinding *b = bindings.Find(uuid);
	if (!b) {
		error = "no output binding with uuid '" + uuid + "'";
		return false;
	}

	// Resolve the prospective post-edit pair (absent fields keep current values),
	// validate refs + the duplicate guard, then commit.
	std::string newProfile = b->profileUuid;
	std::string newCanvas = b->canvasUuid;
	if (params.contains("profileUuid")) {
		newProfile = OptString(params, "profileUuid");
		if (!newProfile.empty() && !ObsBootstrap::StreamProfiles().Find(newProfile)) {
			error = "no stream profile with uuid '" + newProfile + "'";
			return false;
		}
	}
	if (params.contains("canvasUuid")) {
		newCanvas = OptString(params, "canvasUuid");
		if (newCanvas.empty()) {
			error = "'canvasUuid' must be non-empty";
			return false;
		}
		if (!ObsBootstrap::Canvases().Find(newCanvas)) {
			error = "no canvas with uuid '" + newCanvas + "'";
			return false;
		}
	}

	if (bindings.HasPair(newProfile, newCanvas, uuid)) {
		error = "that profile is already bound to this canvas";
		return false;
	}

	// Retargeting an ENABLED binding onto a profile already enabled elsewhere would
	// violate the single-live-stream rule; refuse before touching config or the engine.
	if (b->enabled && !newProfile.empty() && bindings.ProfileEnabledElsewhere(uuid, newProfile)) {
		error = "that stream profile is already enabled on another canvas";
		return false;
	}

	// Keep config and the live output coherent: if this binding is live and the edit
	// changes WHAT/WHERE it streams (its profile or canvas), the running output still
	// carries the old pair, so stop it, commit the new pair, then restart on the new
	// pair (if still enabled). A pure no-op keeps streaming untouched. Engine
	// start/stop manage their own locking; we hold none across them.
	MultistreamEngine &engine = ObsBootstrap::Multistream();
	const bool targetChanged = newProfile != b->profileUuid || newCanvas != b->canvasUuid;
	const bool wasLive = targetChanged && engine.IsLive(uuid);
	if (wasLive) {
		engine.StopOutput(uuid);
	}

	b->profileUuid = newProfile;
	b->canvasUuid = newCanvas;
	const bool saved = ObsBootstrap::OutputBindings().Save();

	// Build/drop the old+new canvas mixes BEFORE (re)starting the output: StartOutput ->
	// EnsureCanvasEncoders -> VideoForCanvas must resolve the new canvas's mix, so it has
	// to exist first. Retargeting onto a previously-inert canvas would otherwise fail the
	// encoder build ("no video mix for canvas") and the stream would silently die.
	ObsBootstrap::CanvasRuntime().ReconcileAll(); // retarget may (de)activate old+new canvas

	if (wasLive && b->enabled) {
		std::string startError;
		engine.StartOutput(uuid, startError);
	}

	EmitEvent(EventNames::kOutputBindingChanged, json::object());
	result = OutputBindingToJson(*b);
	return PersistOrFail(saved, error);
}

bool MethodOutputBindingSetEnabled(const json &params, json &result, std::string &error)
{
	std::string uuid;
	if (!RequireStr(params, "outputBinding.setEnabled", "uuid", uuid, error)) {
		return false;
	}
	if (!params.is_object() || !params.contains("enabled") || !params["enabled"].is_boolean()) {
		error = "outputBinding.setEnabled requires a boolean 'enabled'";
		return false;
	}
	OutputBindings &bindings = ObsBootstrap::OutputBindings().Bindings();
	OutputBinding *b = bindings.Find(uuid);
	if (!b) {
		error = "no output binding with uuid '" + uuid + "'";
		return false;
	}

	const bool enabled = params["enabled"].get<bool>();

	// Enabling must obey the single-live-stream rule: refuse if this profile is already
	// enabled on another canvas (one RTMP key = one live stream). Mirrors the engine's
	// ProfileLiveElsewhere guard so config and the live layer agree.
	if (enabled && !b->profileUuid.empty() && bindings.ProfileEnabledElsewhere(uuid, b->profileUuid)) {
		error = "that stream profile is already enabled on another canvas";
		return false;
	}

	b->enabled = enabled;
	const bool saved = ObsBootstrap::OutputBindings().Save();

	// A disabled binding must not stay live: the canvas stops rendering for it, so a
	// still-running output would encode a frozen frame. StopOutput is a no-op if the
	// binding isn't live; it manages its own locking (we hold none across it).
	if (!enabled) {
		ObsBootstrap::Multistream().StopOutput(uuid);
	}

	// Enabling builds the canvas mix (before any StartOutput); disabling the last
	// enabled destination lets it go inert (StopOutput above already stopped it).
	ObsBootstrap::CanvasRuntime().ReconcileAll();

	// outputBinding.changed is also the hook 4.4.4/4.4.5 use to re-decide whether a
	// canvas renders (AnyEnabledForCanvas may have flipped on this toggle).
	EmitEvent(EventNames::kOutputBindingChanged, json::object());
	result = json{{"uuid", uuid}, {"enabled", enabled}};
	return PersistOrFail(saved, error);
}

bool MethodOutputBindingRemove(const json &params, json &result, std::string &error)
{
	std::string uuid;
	if (!RequireStr(params, "outputBinding.remove", "uuid", uuid, error)) {
		return false;
	}
	OutputBindings &bindings = ObsBootstrap::OutputBindings().Bindings();
	if (!bindings.Find(uuid)) {
		error = "no output binding with uuid '" + uuid + "'";
		return false;
	}
	// Stop a still-running output before deleting its binding, else it keeps streaming
	// orphaned (gone from the UI, its encoder leaked until shutdown, IsCanvasLive stuck
	// true). No-op if the binding isn't live; manages its own locking.
	ObsBootstrap::Multistream().StopOutput(uuid);
	bindings.Remove(uuid);
	const bool saved = ObsBootstrap::OutputBindings().Save();
	ObsBootstrap::CanvasRuntime().ReconcileAll(); // removed the last enabled dest -> inert

	EmitEvent(EventNames::kOutputBindingChanged, json::object());
	result = json{{"removed", uuid}};
	return PersistOrFail(saved, error);
}

// --- multistream live status + per-row control (4.4.4) ----------------------

bool MethodMultistreamStatus(const json & /*params*/, json &result, std::string & /*error*/)
{
	result = json{{"outputs", BuildStatusArray()}};
	return true;
}

bool MethodTransportsHealth(const json & /*params*/, json &result, std::string & /*error*/)
{
	result = json{{"transports", BuildTransportHealthArray()}};
	return true;
}

bool MethodMultistreamStartOutput(const json &params, json &result, std::string &error)
{
	std::string uuid;
	if (!RequireStr(params, "multistream.startOutput", "uuid", uuid, error)) {
		return false;
	}
	std::string startError;
	const bool ok = ObsBootstrap::Multistream().StartOutput(uuid, startError);
	result = json{{"ok", ok}};
	if (!ok && !startError.empty()) {
		// Surface the refusal/failure reason so the row shows WHY it won't go live
		// instead of a bare {ok:false} (the same reason is on multistream.changed).
		result["error"] = startError;
	}
	return true;
}

bool MethodMultistreamStopOutput(const json &params, json &result, std::string &error)
{
	std::string uuid;
	if (!RequireStr(params, "multistream.stopOutput", "uuid", uuid, error)) {
		return false;
	}
	ObsBootstrap::Multistream().StopOutput(uuid);
	result = json{{"ok", true}};
	return true;
}

// --- undo / redo ------------------------------------------------------------

bool MethodUndoUndo(const json & /*params*/, json &result, std::string & /*error*/)
{
	ObsBootstrap::Undo().Undo();
	result = json::object();
	return true;
}

bool MethodUndoRedo(const json & /*params*/, json &result, std::string & /*error*/)
{
	ObsBootstrap::Undo().Redo();
	result = json::object();
	return true;
}

bool MethodUndoState(const json & /*params*/, json &result, std::string & /*error*/)
{
	result = UndoStateJson();
	return true;
}

// --- stats snapshot (general perf + per-output streaming, polled ~1x/s) ------

// Per-binding bitrate cache. stats.get is polled ~1x/s; bitrate is the delta of
// the output's cumulative bytes since the previous poll, so we keep the prior
// sample per binding keyed by uuid. First sample, a missing binding, or a counter
// reset (bytes < prev, e.g. a reconnect) yields 0 kbps for that tick.
struct BitrateSample {
	uint64_t prevBytes = 0;
	uint64_t prevTimeNs = 0;
};

// CPU sampling needs a persistent handle (start once, query per call); kept at
// file scope so Shutdown() can os_cpu_usage_info_destroy it (a function-static
// would leak it past obs_shutdown). The bitrate cache rides along, cleared on
// Shutdown so a re-init starts fresh.
os_cpu_usage_info_t *g_cpuInfo = nullptr;
std::unordered_map<std::string, BitrateSample> g_bitrateCache;

// "Since reset" baselines, mirroring OBS's Stats "Reset" button. stats.reset
// snapshots the current cumulative frame counters here; stats.get reports
// value-minus-baseline so render-lag / encode-skip / per-output drop rates rebase
// from that moment instead of accumulating for the whole session. Per-output
// baselines are keyed by binding uuid. All cleared on Shutdown.
struct StatsBaseline {
	uint32_t renderLagged = 0;
	uint32_t renderTotal = 0;
	uint32_t encodeSkipped = 0;
	uint32_t encodeTotal = 0;
};
StatsBaseline g_statsBaseline;
std::unordered_map<std::string, std::pair<int, int>> g_outputStatsBaseline; // uuid -> {dropped, total}

// value - baseline, self-healing: if the counter dropped below its baseline the
// pipeline reset underneath (e.g. obs_reset_video, or an output restart), so drop the
// baseline to 0 and report the raw value. Mutates `baseline` in place on that heal.
template<typename T> T RebaseCounter(T value, T &baseline)
{
	if (value < baseline) {
		baseline = 0;
	}
	return static_cast<T>(value - baseline);
}

bool MethodStatsGet(const json & /*params*/, json &result, std::string & /*error*/)
{
	if (!g_cpuInfo) {
		g_cpuInfo = os_cpu_usage_info_start();
	}
	std::unordered_map<std::string, BitrateSample> &bitrateCache = g_bitrateCache;

	// --- general performance ---
	// The first query after start can return NaN until two samples exist; clamp
	// it to 0 so the payload stays clean (real CPU populates on the next poll).
	double cpu = g_cpuInfo ? os_cpu_usage_info_query(g_cpuInfo) : 0.0;
	if (!(cpu == cpu)) {
		cpu = 0.0;
	}
	const double memoryMB = static_cast<double>(os_get_proc_resident_size()) / (1024.0 * 1024.0);
	const double fps = obs_get_active_fps();
	const double avgFrameMs = static_cast<double>(obs_get_average_frame_time_ns()) / 1.0e6;

	const uint32_t renderLagged = RebaseCounter(obs_get_lagged_frames(), g_statsBaseline.renderLagged);
	const uint32_t renderTotal = RebaseCounter(obs_get_total_frames(), g_statsBaseline.renderTotal);
	const double renderLagPct = renderTotal > 0 ? (static_cast<double>(renderLagged) / renderTotal) * 100.0 : 0.0;

	uint32_t encodeSkipped = 0;
	uint32_t encodeTotal = 0;
	if (video_t *video = obs_get_video()) {
		encodeSkipped = RebaseCounter(video_output_get_skipped_frames(video), g_statsBaseline.encodeSkipped);
		encodeTotal = RebaseCounter(video_output_get_total_frames(video), g_statsBaseline.encodeTotal);
	}
	const double encodeSkipPct = encodeTotal > 0 ? (static_cast<double>(encodeSkipped) / encodeTotal) * 100.0 : 0.0;

	json general = json{
		{"cpu", cpu},
		{"memoryMB", memoryMB},
		{"fps", fps},
		{"avgFrameMs", avgFrameMs},
		{"renderLagged", static_cast<int>(renderLagged)},
		{"renderTotal", static_cast<int>(renderTotal)},
		{"renderLagPct", renderLagPct},
		{"encodeSkipped", static_cast<int>(encodeSkipped)},
		{"encodeTotal", static_cast<int>(encodeTotal)},
		{"encodeSkipPct", encodeSkipPct},
	};

	// --- per-output streaming ---
	const uint64_t nowNs = os_gettime_ns();
	json outputs = json::array();
	std::unordered_map<std::string, BitrateSample> nextCache;
	// Rebuilt to only the bindings present this tick (same prune-forward pattern as the
	// bitrate cache), carrying each binding's rebased-and-self-healed drop/total baseline.
	std::unordered_map<std::string, std::pair<int, int>> nextDropBaseline;
	for (const MultistreamEngine::OutputStats &s : ObsBootstrap::Multistream().StatsSnapshot()) {
		// bitrate = delta-bytes / delta-seconds * 8 / 1000 (kbps). First sample
		// (no prior) or a counter reset -> 0. Re-key nextCache so stale bindings
		// no longer present are dropped automatically.
		double bitrateKbps = 0.0;
		auto it = bitrateCache.find(s.bindingUuid);
		if (it != bitrateCache.end() && s.totalBytes >= it->second.prevBytes && nowNs > it->second.prevTimeNs) {
			const double deltaBytes = static_cast<double>(s.totalBytes - it->second.prevBytes);
			const double deltaSec = static_cast<double>(nowNs - it->second.prevTimeNs) / 1.0e9;
			if (deltaSec > 0.0) {
				bitrateKbps = (deltaBytes * 8.0 / 1000.0) / deltaSec;
			}
		}
		nextCache[s.bindingUuid] = BitrateSample{s.totalBytes, nowNs};

		// Rebase dropped/total against the reset baseline (0 if never reset for this
		// binding), self-healing on an output restart that zeroed the raw counters.
		std::pair<int, int> base = {0, 0};
		auto bit = g_outputStatsBaseline.find(s.bindingUuid);
		if (bit != g_outputStatsBaseline.end()) {
			base = bit->second;
		}
		const int droppedFrames = RebaseCounter(s.droppedFrames, base.first);
		const int totalFrames = RebaseCounter(s.totalFrames, base.second);
		nextDropBaseline[s.bindingUuid] = base;

		const double dropPct =
			totalFrames > 0 ? (static_cast<double>(droppedFrames) / totalFrames) * 100.0 : 0.0;

		outputs.push_back(json{
			{"bindingUuid", s.bindingUuid},
			{"profileLabel", s.profileLabel},
			{"canvasName", s.canvasName},
			{"state", MultistreamEngine::StateName(s.state)},
			{"bitrateKbps", bitrateKbps},
			{"droppedFrames", droppedFrames},
			{"totalFrames", totalFrames},
			{"dropPct", dropPct},
			{"congestionPct", s.congestion * 100.0},
			{"durationMs", s.uptimeMs},
		});
	}
	bitrateCache.swap(nextCache);
	g_outputStatsBaseline.swap(nextDropBaseline);

	result = json{{"general", std::move(general)}, {"outputs", std::move(outputs)}};
	return true;
}

// Rebase the "since reset" stats baselines to the current cumulative counters, so the
// render-lag / encode-skip / per-output drop rates restart from zero. Mirrors OBS's
// Stats "Reset" button. Instantaneous readings (cpu/mem/fps/bitrate) are unaffected.
bool MethodStatsReset(const json & /*params*/, json &result, std::string & /*error*/)
{
	g_statsBaseline.renderLagged = obs_get_lagged_frames();
	g_statsBaseline.renderTotal = obs_get_total_frames();
	if (video_t *video = obs_get_video()) {
		g_statsBaseline.encodeSkipped = video_output_get_skipped_frames(video);
		g_statsBaseline.encodeTotal = video_output_get_total_frames(video);
	}
	g_outputStatsBaseline.clear();
	for (const MultistreamEngine::OutputStats &s : ObsBootstrap::Multistream().StatsSnapshot()) {
		g_outputStatsBaseline[s.bindingUuid] = {s.droppedFrames, s.totalFrames};
	}
	result = json{{"ok", true}};
	return true;
}

// --- audio mixer (per-source faders + volmeters, levels) --------------------

// Defined below (in the advanced-audio block); forward-declared so the earlier
// per-source setters can share the one uuid|name resolver.
obs_source_t *ResolveAudioSource(const json &params);

// --- per-source mixer flags (hide / lock-volume / pin) -----------------------
// Three booleans stored in each audio source's private settings (persist with the
// scene collection via PersistSourceState), surfaced on every audio.list row and
// mutated by audio.set{Hidden,VolumeLocked,Pinned}/audio.unhideAll. Reuses
// ResolveAudioSource + the private-settings idiom; each mutation emits audio.changed.
namespace {
constexpr const char *kMixerHiddenKey = "mixer_hidden";
constexpr const char *kVolumeLockedKey = "volume_locked";
constexpr const char *kMixerPinnedKey = "mixer_pinned";
} // namespace

// Read one mixer flag from a source's private settings (false when absent/null).
bool GetSourceMixerFlag(obs_source_t *s, const char *key)
{
	OBSDataAutoRelease priv = obs_source_get_private_settings(s);
	return priv ? obs_data_get_bool(priv, key) : false;
}

// Shared setter for the three mixer flags: validate the boolean field, resolve the
// source (uuid|name), write the private-settings key, persist, echo, emit changed.
// setHidden/setVolumeLocked/setPinned differ only by (key, field), so they share this.
bool SetSourceMixerFlag(const json &params, const char *method, const char *key, const char *field, json &result,
			std::string &error)
{
	if (!params.is_object() || !params.contains(field) || !params[field].is_boolean()) {
		error = std::string(method) + " requires a boolean '" + field + "'";
		return false;
	}
	OBSSourceAutoRelease s = ResolveAudioSource(params);
	if (!s) {
		error = std::string(method) + ": no source for the given 'uuid'/'source'";
		return false;
	}
	const bool value = params[field].get<bool>();
	{
		OBSDataAutoRelease priv = obs_source_get_private_settings(s);
		obs_data_set_bool(priv, key, value);
	}
	PersistSourceState(s);
	result = json{{"uuid", obs_source_get_uuid(s)}, {field, value}};
	EmitEvent(EventNames::kAudioChanged, json::object());
	return true;
}

bool MethodAudioSetHidden(const json &params, json &result, std::string &error)
{
	return SetSourceMixerFlag(params, "audio.setHidden", kMixerHiddenKey, "hidden", result, error);
}

bool MethodAudioSetVolumeLocked(const json &params, json &result, std::string &error)
{
	return SetSourceMixerFlag(params, "audio.setVolumeLocked", kVolumeLockedKey, "locked", result, error);
}

bool MethodAudioSetPinned(const json &params, json &result, std::string &error)
{
	return SetSourceMixerFlag(params, "audio.setPinned", kMixerPinnedKey, "pinned", result, error);
}

// Clear mixer_hidden on every active audio source (the "Unhide All" action). Persists
// per cleared source; one audio.changed afterward refreshes the mixer.
bool MethodAudioUnhideAll(const json & /*params*/, json &result, std::string & /*error*/)
{
	int cleared = 0;
	for (const AudioMonitor::SourceInfo &s : ObsBootstrap::AudioMonitor().List()) {
		OBSSourceAutoRelease src = obs_get_source_by_uuid(s.uuid.c_str()); // addref'd
		if (!src) {
			continue;
		}
		OBSDataAutoRelease priv = obs_source_get_private_settings(src);
		if (priv && obs_data_get_bool(priv, kMixerHiddenKey)) {
			obs_data_set_bool(priv, kMixerHiddenKey, false);
			PersistSourceState(src);
			cleared++;
		}
	}
	result = json{{"cleared", cleared}};
	EmitEvent(EventNames::kAudioChanged, json::object());
	return true;
}

bool MethodAudioList(const json & /*params*/, json &result, std::string & /*error*/)
{
	std::vector<json> rows;
	for (const AudioMonitor::SourceInfo &s : ObsBootstrap::AudioMonitor().List()) {
		// The mixer flags live in the source's private settings, not on the
		// AudioMonitor entry -- read them from the resolved source per row.
		bool hidden = false;
		bool volumeLocked = false;
		bool pinned = false;
		if (OBSSourceAutoRelease src = obs_get_source_by_uuid(s.uuid.c_str())) {
			hidden = GetSourceMixerFlag(src, kMixerHiddenKey);
			volumeLocked = GetSourceMixerFlag(src, kVolumeLockedKey);
			pinned = GetSourceMixerFlag(src, kMixerPinnedKey);
		}
		rows.push_back(json{
			{"uuid", s.uuid},
			{"name", s.name},
			{"deflection", s.deflection},
			{"volumeDb", s.volumeDb},
			{"muted", s.muted},
			{"hidden", hidden},
			{"volumeLocked", volumeLocked},
			{"pinned", pinned},
		});
	}
	// Pinned sources sort first; stable_partition keeps each group's existing relative
	// order (a pinned-FLAG sort, not arbitrary drag-ordering).
	std::stable_partition(rows.begin(), rows.end(), [](const json &r) { return r.value("pinned", false); });
	json arr = json::array();
	for (json &r : rows) {
		arr.push_back(std::move(r));
	}
	result = json{{"sources", std::move(arr)}};
	return true;
}

bool MethodAudioSetDeflection(const json &params, json &result, std::string &error)
{
	std::string uuid;
	if (!RequireStr(params, "audio.setDeflection", "uuid", uuid, error)) {
		return false;
	}
	if (!params.is_object() || !params.contains("deflection") || !params["deflection"].is_number()) {
		error = "audio.setDeflection requires a number 'deflection'";
		return false;
	}
	// A volume-locked source refuses fader moves (audio.setVolumeLocked): no-op with a
	// clean success echoing the current position so an optimistic UI drag reverts.
	if (OBSSourceAutoRelease locked = obs_get_source_by_uuid(uuid.c_str())) {
		if (GetSourceMixerFlag(locked, kVolumeLockedKey)) {
			float curDef = 0.0f;
			float curDb = 0.0f;
			for (const AudioMonitor::SourceInfo &si : ObsBootstrap::AudioMonitor().List()) {
				if (si.uuid == uuid) {
					curDef = si.deflection;
					curDb = si.volumeDb;
					break;
				}
			}
			result = json{{"uuid", uuid}, {"deflection", curDef}, {"volumeDb", curDb}, {"locked", true}};
			return true;
		}
	}
	const float deflection = params["deflection"].get<float>();
	float appliedDef = 0.0f;
	float appliedDb = 0.0f;
	if (!ObsBootstrap::AudioMonitor().SetDeflection(uuid, deflection, appliedDef, appliedDb)) {
		error = "no active audio source with uuid '" + uuid + "'";
		return false;
	}
	if (OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str())) {
		PersistSourceState(source);
	}
	result = json{{"uuid", uuid}, {"deflection", appliedDef}, {"volumeDb", appliedDb}};
	return true;
}

bool MethodAudioSetMuted(const json &params, json &result, std::string &error)
{
	if (!params.is_object() || !params.contains("muted") || !params["muted"].is_boolean()) {
		error = "audio.setMuted requires a boolean 'muted'";
		return false;
	}
	// Resolve by uuid or name (via the shared resolver) so name-addressing works
	// the same as its sibling audio handlers (getAdvanced/setAdvanced).
	OBSSourceAutoRelease source = ResolveAudioSource(params);
	if (!source) {
		error = "audio.setMuted: no source for the given 'uuid'/'source'";
		return false;
	}
	const bool muted = params["muted"].get<bool>();
	obs_source_set_muted(source, muted);
	PersistSourceState(source);
	result = json{{"uuid", obs_source_get_uuid(source)}, {"muted", muted}};
	return true;
}

// --- advanced per-source audio properties ------------------------------------
// Mirrors stock OBS's "Advanced Audio Properties" dialog: per-source gain,
// downmix-to-mono, stereo balance, sync offset, mixer-track routing, and
// monitoring type. State lives on the source and saves with the scene
// collection automatically (no separate persistence needed).

static const std::pair<obs_monitoring_type, const char *> kMonitoringTypes[] = {
	{OBS_MONITORING_TYPE_NONE, "none"},
	{OBS_MONITORING_TYPE_MONITOR_ONLY, "monitorOnly"},
	{OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT, "monitorAndOutput"},
};

const char *MonitoringTypeName(obs_monitoring_type type)
{
	for (const auto &m : kMonitoringTypes) {
		if (m.first == type) {
			return m.second;
		}
	}
	return "none";
}

bool MonitoringTypeFromName(const std::string &name, obs_monitoring_type &out)
{
	for (const auto &m : kMonitoringTypes) {
		if (name == m.second) {
			out = m.first;
			return true;
		}
	}
	return false;
}

// Resolve an audio source by uuid (preferred) or name, from either the "uuid"
// or "source" param key. Returns an addref'd source (caller releases).
obs_source_t *ResolveAudioSource(const json &params)
{
	for (const char *key : {"uuid", "source"}) {
		const std::string v = OptString(params, key);
		if (v.empty()) {
			continue;
		}
		if (obs_source_t *s = obs_get_source_by_uuid(v.c_str())) { // addref'd
			return s;
		}
		if (obs_source_t *s = obs_get_source_by_name(v.c_str())) { // addref'd
			return s;
		}
	}
	return nullptr;
}

// Build the full advanced-audio state JSON for a source. -inf gain (muted to
// zero multiplier) is reported as null so the UI can render it distinctly.
json BuildAdvancedAudio(obs_source_t *s)
{
	const float db = obs_mul_to_db(obs_source_get_volume(s));
	const uint32_t mixers = obs_source_get_audio_mixers(s);
	json tracks = json::array();
	for (int i = 0; i < 6; ++i) {
		tracks.push_back((mixers & (1u << i)) != 0);
	}
	return json{
		{"volumeDb", std::isfinite(db) ? json(db) : json(nullptr)},
		{"forceMono", (obs_source_get_flags(s) & OBS_SOURCE_FLAG_FORCE_MONO) != 0},
		{"balance", obs_source_get_balance_value(s)},
		{"syncOffsetMs", int(obs_source_get_sync_offset(s) / 1000000)},
		{"tracks", std::move(tracks)},
		{"monitoringType", MonitoringTypeName(obs_source_get_monitoring_type(s))},
	};
}

bool MethodAudioGetAdvanced(const json &params, json &result, std::string &error)
{
	OBSSourceAutoRelease s = ResolveAudioSource(params);
	if (!s) {
		error = "audio.getAdvanced: no source for the given 'uuid'/'source'";
		return false;
	}
	result = BuildAdvancedAudio(s);
	return true;
}

bool MethodAudioSetAdvanced(const json &params, json &result, std::string &error)
{
	if (!RequireObject(params, "audio.setAdvanced", error)) {
		return false;
	}
	OBSSourceAutoRelease s = ResolveAudioSource(params);
	if (!s) {
		error = "audio.setAdvanced: no source for the given 'uuid'/'source'";
		return false;
	}

	// Validate monitoringType up front so an invalid value rejects the whole
	// request before any field is written -- the mutation stays atomic.
	bool setMonitoring = false;
	obs_monitoring_type monitoringType = OBS_MONITORING_TYPE_NONE;
	if (auto it = params.find("monitoringType"); it != params.end() && it->is_string()) {
		if (!MonitoringTypeFromName(it->get<std::string>(), monitoringType)) {
			error = "audio.setAdvanced: unknown monitoringType '" + it->get<std::string>() + "'";
			return false;
		}
		setMonitoring = true;
	}

	// tracks is all-or-nothing: setAdvanced is a partial patch, but a short/partial
	// tracks array would silently disable the trailing mixer tracks. Require the full
	// mixer-count array up front (reject otherwise) so a partial send can't zero
	// tracks; an absent tracks leaves the mixer mask untouched.
	if (auto it = params.find("tracks"); it != params.end()) {
		if (!it->is_array() || it->size() != MAX_AUDIO_MIXES) {
			error = "audio.setAdvanced: 'tracks' must be a " + std::to_string(MAX_AUDIO_MIXES) +
				"-element boolean array";
			return false;
		}
	}

	if (auto it = params.find("volumeDb"); it != params.end() && it->is_number()) {
		obs_source_set_volume(s, obs_db_to_mul(it->get<float>()));
	}

	if (auto it = params.find("forceMono"); it != params.end() && it->is_boolean()) {
		uint32_t flags = obs_source_get_flags(s);
		if (it->get<bool>()) {
			flags |= OBS_SOURCE_FLAG_FORCE_MONO;
		} else {
			flags &= ~uint32_t(OBS_SOURCE_FLAG_FORCE_MONO);
		}
		obs_source_set_flags(s, flags);
	}

	if (auto it = params.find("balance"); it != params.end() && it->is_number()) {
		obs_source_set_balance_value(s, std::clamp(it->get<float>(), 0.0f, 1.0f));
	}

	if (auto it = params.find("syncOffsetMs"); it != params.end() && it->is_number()) {
		obs_source_set_sync_offset(s, it->get<int64_t>() * 1000000);
	}

	if (auto it = params.find("tracks"); it != params.end()) {
		uint32_t mask = 0;
		const json &arr = *it;
		for (size_t i = 0; i < MAX_AUDIO_MIXES; ++i) {
			if (arr[i].is_boolean() && arr[i].get<bool>()) {
				mask |= (1u << i);
			}
		}
		obs_source_set_audio_mixers(s, mask);
	}

	if (setMonitoring) {
		obs_source_set_monitoring_type(s, monitoringType);
	}

	PersistSourceState(s);
	result = BuildAdvancedAudio(s);
	EmitEvent(EventNames::kAudioChanged, json::object());
	return true;
}

// --- per-source deinterlacing ------------------------------------------------
// Token <-> enum maps shared by sources.getDeinterlace (enum->token) and
// sources.setDeinterlace (token->enum), mirroring kScaleFilters/kMonitoringTypes.
// The mode/field-order live on the source and serialize with the scene
// collection (libobs persists "deinterlace_mode"/"deinterlace_field_order").

static const std::pair<obs_deinterlace_mode, const char *> kDeinterlaceModes[] = {
	{OBS_DEINTERLACE_MODE_DISABLE, "disable"},    {OBS_DEINTERLACE_MODE_DISCARD, "discard"},
	{OBS_DEINTERLACE_MODE_RETRO, "retro"},        {OBS_DEINTERLACE_MODE_BLEND, "blend"},
	{OBS_DEINTERLACE_MODE_BLEND_2X, "blend2x"},   {OBS_DEINTERLACE_MODE_LINEAR, "linear"},
	{OBS_DEINTERLACE_MODE_LINEAR_2X, "linear2x"}, {OBS_DEINTERLACE_MODE_YADIF, "yadif"},
	{OBS_DEINTERLACE_MODE_YADIF_2X, "yadif2x"},
};

static const std::pair<obs_deinterlace_field_order, const char *> kDeinterlaceFieldOrders[] = {
	{OBS_DEINTERLACE_FIELD_ORDER_TOP, "top"},
	{OBS_DEINTERLACE_FIELD_ORDER_BOTTOM, "bottom"},
};

const char *DeinterlaceModeToToken(obs_deinterlace_mode mode)
{
	for (const auto &e : kDeinterlaceModes) {
		if (e.first == mode) {
			return e.second;
		}
	}
	return "disable";
}

bool DeinterlaceModeFromToken(const std::string &token, obs_deinterlace_mode &out)
{
	for (const auto &e : kDeinterlaceModes) {
		if (token == e.second) {
			out = e.first;
			return true;
		}
	}
	return false;
}

const char *DeinterlaceFieldOrderToToken(obs_deinterlace_field_order order)
{
	for (const auto &e : kDeinterlaceFieldOrders) {
		if (e.first == order) {
			return e.second;
		}
	}
	return "top";
}

bool DeinterlaceFieldOrderFromToken(const std::string &token, obs_deinterlace_field_order &out)
{
	for (const auto &e : kDeinterlaceFieldOrders) {
		if (token == e.second) {
			out = e.first;
			return true;
		}
	}
	return false;
}

// Reuses ResolveAudioSource (a misnomer -- it resolves any source by uuid|name).
json BuildDeinterlace(obs_source_t *s)
{
	return json{
		{"mode", DeinterlaceModeToToken(obs_source_get_deinterlace_mode(s))},
		{"fieldOrder", DeinterlaceFieldOrderToToken(obs_source_get_deinterlace_field_order(s))},
	};
}

bool MethodSourcesGetDeinterlace(const json &params, json &result, std::string &error)
{
	OBSSourceAutoRelease s = ResolveAudioSource(params);
	if (!s) {
		error = "sources.getDeinterlace: no source for the given 'source'/'uuid'";
		return false;
	}
	result = BuildDeinterlace(s);
	return true;
}

bool MethodSourcesSetDeinterlace(const json &params, json &result, std::string &error)
{
	if (!RequireObject(params, "sources.setDeinterlace", error)) {
		return false;
	}
	OBSSourceAutoRelease s = ResolveAudioSource(params);
	if (!s) {
		error = "sources.setDeinterlace: no source for the given 'source'/'uuid'";
		return false;
	}

	// Validate both tokens before applying so an invalid value rejects the whole
	// request -- the mutation stays atomic.
	bool setMode = false;
	obs_deinterlace_mode mode = OBS_DEINTERLACE_MODE_DISABLE;
	if (auto it = params.find("mode"); it != params.end() && it->is_string()) {
		if (!DeinterlaceModeFromToken(it->get<std::string>(), mode)) {
			error = "sources.setDeinterlace: unknown mode '" + it->get<std::string>() + "'";
			return false;
		}
		setMode = true;
	}
	bool setField = false;
	obs_deinterlace_field_order fieldOrder = OBS_DEINTERLACE_FIELD_ORDER_TOP;
	if (auto it = params.find("fieldOrder"); it != params.end() && it->is_string()) {
		if (!DeinterlaceFieldOrderFromToken(it->get<std::string>(), fieldOrder)) {
			error = "sources.setDeinterlace: unknown fieldOrder '" + it->get<std::string>() + "'";
			return false;
		}
		setField = true;
	}

	if (setMode) {
		obs_source_set_deinterlace_mode(s, mode);
	}
	if (setField) {
		obs_source_set_deinterlace_field_order(s, fieldOrder);
	}
	// Persist now (matching the sibling per-item setters) so the choice survives an
	// unclean exit rather than only the shutdown save.
	if (setMode || setField) {
		SceneCollection::Save();
	}

	result = BuildDeinterlace(s);
	return true;
}

// List the available audio monitoring output devices. Stock OBS prepends a
// "Default" entry (id "default") before enumerating the real devices.
bool MethodAudioListMonitorDevices(const json & /*params*/, json &result, std::string & /*error*/)
{
	json arr = json::array();
	arr.push_back(json{{"id", "default"}, {"name", "Default"}});
	obs_enum_audio_monitoring_devices(
		[](void *data, const char *name, const char *id) -> bool {
			auto *out = static_cast<json *>(data);
			out->push_back(json{{"id", id ? std::string(id) : std::string()},
					    {"name", name ? std::string(name) : std::string()}});
			return true;
		},
		&arr);
	result = std::move(arr);
	return true;
}

// --- global audio devices (Desktop Audio / Mic on output channels 1..6) ------
// The per-channel slot table + seed/restore/persist/apply now live in the
// bootstrap-owned GlobalAudioChannels subsystem; the handlers below drive it. The
// device enumeration helper stays here since it serves the generic audio.listDevices
// method, not the global-channel state.

// Enumerate {id,name} audio devices for the matching wasapi type. Reads the
// "device_id" string-list property off the source type. Some libobs builds need a
// live instance for the list to populate, so fall back to a temp source if the
// type-level property list is empty.
std::vector<std::pair<std::string, std::string>> EnumAudioDevices(bool input)
{
	const char *typeId = input ? "wasapi_input_capture" : "wasapi_output_capture";

	auto readList = [](obs_properties_t *props, std::vector<std::pair<std::string, std::string>> &out) -> bool {
		if (!props) {
			return false;
		}
		obs_property_t *p = obs_properties_get(props, "device_id");
		if (!p) {
			return false;
		}
		const size_t count = obs_property_list_item_count(p);
		for (size_t i = 0; i < count; ++i) {
			const char *name = obs_property_list_item_name(p, i);
			const char *id = obs_property_list_item_string(p, i);
			out.emplace_back(id ? id : std::string(), name ? name : std::string());
		}
		return count > 0;
	};

	std::vector<std::pair<std::string, std::string>> out;

	// Path 1: properties straight off the source type.
	obs_properties_t *typeProps = obs_get_source_properties(typeId);
	const bool typePath = readList(typeProps, out);
	if (typeProps) {
		obs_properties_destroy(typeProps);
	}
	if (typePath) {
		HostLog(std::string("[bridge] audio.enumDevices ") + typeId + " -> " + std::to_string(out.size()) +
			" via source-type properties");
		return out;
	}

	// Path 2 (fallback): a temp instance whose properties carry the populated list.
	out.clear();
	OBSSourceAutoRelease tmp = obs_source_create(typeId, "audio-enum-probe", nullptr, nullptr);
	if (tmp) {
		obs_properties_t *instProps = obs_source_properties(tmp);
		readList(instProps, out);
		if (instProps) {
			obs_properties_destroy(instProps);
		}
	}
	HostLog(std::string("[bridge] audio.enumDevices ") + typeId + " -> " + std::to_string(out.size()) +
		" via temp-instance properties (type-level list empty)");
	return out;
}

bool MethodAudioListDevices(const json &params, json &result, std::string & /*error*/)
{
	const std::string kind = OptString(params, "kind");
	const bool input = kind == "input"; // default "output"
	json arr = json::array();
	for (const auto &dev : EnumAudioDevices(input)) {
		arr.push_back(json{{"id", dev.first}, {"name", dev.second}});
	}
	result = std::move(arr);
	return true;
}

bool MethodAudioGetGlobalDevices(const json & /*params*/, json &result, std::string & /*error*/)
{
	GlobalAudioChannels &audio = ObsBootstrap::GlobalAudioChannels();
	json arr = json::array();
	for (const GlobalAudioChannels::Slot &slot : audio.Slots()) {
		std::optional<std::string> device = audio.CurrentDevice(slot.channel);
		json deviceId = device ? json(*device) : json(nullptr);
		arr.push_back(json{
			{"channel", slot.channel},
			{"role", slot.role},
			{"label", slot.label},
			{"isInput", slot.input},
			{"deviceId", deviceId},
			{"active", device.has_value()},
		});
	}
	result = std::move(arr);
	return true;
}

bool MethodAudioSetGlobalDevice(const json &params, json &result, std::string &error)
{
	if (!params.is_object() || !params.contains("channel") || !params["channel"].is_number_integer()) {
		error = "audio.setGlobalDevice requires an integer 'channel'";
		return false;
	}
	const int channel = params["channel"].get<int>();

	// null/absent/"" all mean disable.
	std::string deviceId;
	auto it = params.find("deviceId");
	if (it != params.end() && it->is_string()) {
		deviceId = it->get<std::string>();
	}

	GlobalAudioChannels &audio = ObsBootstrap::GlobalAudioChannels();
	if (!audio.ApplyDevice(channel, deviceId, error)) {
		return false;
	}
	const bool saved = audio.Persist();
	ObsBootstrap::AudioMonitor().Rebuild();
	EmitEvent(EventNames::kAudioChanged, json::object());

	result = json{{"channel", channel}, {"deviceId", deviceId.empty() ? json(nullptr) : json(deviceId)}};
	return PersistOrFail(saved, error);
}

// Post the actual ExecuteJavaScript on TID_UI, broadcasting to every registered
// browser. Built from JSON dumps so the name and payload are correctly
// quoted/escaped. With a single registered browser this is identical to a
// single-target emit.
void DoEmit(const std::string &name, const std::string &payloadDump)
{
	CEF_REQUIRE_UI_THREAD();

	// Snapshot the registry so a callback can't mutate it under iteration and so
	// we never hold the lock across ExecuteJavaScript.
	std::vector<CefRefPtr<CefBrowser>> browsers;
	{
		std::lock_guard<std::mutex> lock(g_browser_mutex);
		browsers = g_browsers;
	}

	// window.__obsEmit("<name>", <payload>); guarded so it no-ops before the
	// bridge client script has defined the sink.
	const std::string nameDump = json(name).dump();
	const std::string script = "if(window.__obsEmit){window.__obsEmit(" + nameDump + "," + payloadDump + ");}";
	for (const CefRefPtr<CefBrowser> &browser : browsers) {
		if (!browser) {
			continue;
		}
		CefRefPtr<CefFrame> frame = browser->GetMainFrame();
		if (frame) {
			frame->ExecuteJavaScript(script, frame->GetURL(), 0);
		}
	}
}

// --- theme + layout persistence (P1) ----------------------------------------
// The Svelte shell persists the active theme preset and the Dockview layout JSON
// to <config>/braidcast/basic/{theme,layout}.json, reusing the same
// obs_data_* round-trip the stream/canvas stores use. The payloads are opaque
// strings to C++ (the shell owns their shape); we read and write them verbatim.
// WriteJsonString/ReadJsonString are defined at namespace-Bridge scope (below the
// anonymous namespace) so other TUs (transitions.cpp) can share them.

// --- transitions ------------------------------------------------------------

bool MethodTransitionTypesList(const json & /*params*/, json &result, std::string & /*error*/)
{
	json types = json::array();
	for (const auto &[id, name] : Transitions::TypeList()) {
		types.push_back(json{{"id", id}, {"name", name}});
	}
	result = std::move(types);
	return true;
}

bool MethodTransitionsGetCurrent(const json & /*params*/, json &result, std::string & /*error*/)
{
	std::string id;
	std::string name;
	uint32_t durationMs = 0;
	Transitions::Current(id, name, durationMs);
	result = json{{"id", id}, {"name", name}, {"durationMs", durationMs}};
	return true;
}

bool MethodTransitionsSetCurrent(const json &params, json &result, std::string &error)
{
	std::string id;
	if (!RequireStr(params, "transitions.setCurrent", "id", id, error)) {
		return false;
	}
	if (!Transitions::SetCurrentType(id, error)) {
		return false;
	}

	std::string curId;
	std::string curName;
	uint32_t durationMs = 0;
	Transitions::Current(curId, curName, durationMs);
	result = json{{"id", curId}, {"name", curName}};
	EmitEvent(EventNames::kTransitionsChanged, json::object());
	return true;
}

bool MethodTransitionsSetDuration(const json &params, json &result, std::string &error)
{
	if (!params.is_object()) {
		error = "transitions.setDuration requires a 'durationMs'";
		return false;
	}
	auto it = params.find("durationMs");
	int64_t ms = -1;
	if (it != params.end() && it->is_number_integer()) {
		ms = it->get<int64_t>();
	} else if (it != params.end() && it->is_string()) {
		try {
			ms = std::stoll(it->get<std::string>());
		} catch (const std::exception &) {
			ms = -1;
		}
	}
	if (ms < 0) {
		error = "transitions.setDuration requires a non-negative integer 'durationMs'";
		return false;
	}
	Transitions::SetDuration(static_cast<uint32_t>(ms));
	result = json{{"durationMs", static_cast<uint32_t>(ms)}};
	EmitEvent(EventNames::kTransitionsChanged, json::object());
	return true;
}

bool MethodThemeSave(const json &params, json &result, std::string &error)
{
	const std::string state = OptString(params, "state");
	if (state.empty()) {
		error = "theme.save requires a non-empty 'state' string";
		return false;
	}
	if (!WriteJsonString("theme.json", "state", state)) {
		error = "failed to write theme.json";
		return false;
	}
	result = json{{"saved", true}};
	return true;
}

bool MethodThemeLoad(const json & /*params*/, json &result, std::string & /*error*/)
{
	// Empty => nothing saved yet; the shell falls back to the default preset/tokens.
	// `state` is an opaque JSON string the JS theme store stringifies/parses itself.
	result = json{{"state", ReadJsonString("theme.json", "state")}};
	return true;
}

bool MethodLayoutSave(const json &params, json &result, std::string &error)
{
	const std::string layout = OptString(params, "layout");
	if (layout.empty()) {
		error = "layout.save requires a non-empty 'layout' string";
		return false;
	}
	if (!WriteJsonString("layout.json", "layout", layout)) {
		error = "failed to write layout.json";
		return false;
	}
	result = json{{"saved", true}};
	return true;
}

bool MethodLayoutLoad(const json & /*params*/, json &result, std::string & /*error*/)
{
	// Empty => no saved layout yet; the shell builds the default arrangement.
	result = json{{"layout", ReadJsonString("layout.json", "layout")}};
	return true;
}

// Detached-window control surface — drives WindowManager and broadcasts the
// window.opened/closed lifecycle to every live browser via EmitEvent.

bool MethodWindowDetach(const json &params, json &result, std::string &error)
{
	WindowManager *wm = WindowManager::Instance();
	if (!wm) {
		error = "window manager not active";
		return false;
	}
	std::string dock;
	if (!RequireStr(params, "window.detach", "dock", dock, error)) {
		return false;
	}
	const int windowId = wm->Detach(dock);
	if (windowId <= 0) {
		error = "failed to create detached window";
		return false;
	}
	EmitEvent(EventNames::kWindowOpened, json{{"windowId", windowId}, {"dock", dock}});
	HostLog("[bridge] window.detach -> ok id=" + std::to_string(windowId) + " dock=" + dock);
	result = json{{"windowId", windowId}};
	return true;
}

bool MethodWindowRedock(const json &params, json &result, std::string &error)
{
	WindowManager *wm = WindowManager::Instance();
	if (!wm) {
		error = "window manager not active";
		return false;
	}
	if (!params.is_object() || !params.contains("windowId") || !params["windowId"].is_number_integer()) {
		error = "window.redock requires an integer 'windowId'";
		return false;
	}
	const int windowId = params["windowId"].get<int>();
	// Capture the dock id before Redock removes the window, so the broadcast can tell
	// the main window which dock to restore.
	std::string dock;
	for (const WindowManager::WindowInfo &w : wm->List()) {
		if (w.windowId == windowId) {
			dock = w.dockId;
			break;
		}
	}
	if (!wm->Redock(windowId)) {
		error = "no detached window with id " + std::to_string(windowId);
		return false;
	}
	EmitEvent(EventNames::kWindowClosed, json{{"windowId", windowId}, {"dock", dock}});
	HostLog("[bridge] window.redock -> ok id=" + std::to_string(windowId));
	result = json{{"redocked", windowId}};
	return true;
}

bool MethodWindowList(const json & /*params*/, json &result, std::string & /*error*/)
{
	WindowManager *wm = WindowManager::Instance();
	json arr = json::array();
	if (wm) {
		for (const WindowManager::WindowInfo &w : wm->List()) {
			arr.push_back(json{{"windowId", w.windowId}, {"dock", w.dockId}});
		}
	}
	result = json{{"windows", std::move(arr)}};
	return true;
}

// Host-window borderless-fullscreen toggle (Fullscreen Interface / F11). Saved
// across calls so the toggle can restore the original framed geometry. UI thread
// only, so plain file-scope state is safe.
bool g_hostFullscreen = false;
LONG_PTR g_savedHostStyle = 0;
WINDOWPLACEMENT g_savedHostPlacement = {sizeof(WINDOWPLACEMENT)};

bool MethodWindowToggleFullscreen(const json & /*params*/, json &result, std::string &error)
{
	PreviewManager *pm = Preview::Instance();
	HWND host = pm ? pm->MainHostHwnd() : nullptr;
	if (!host) {
		error = "no host window";
		return false;
	}

	if (!g_hostFullscreen) {
		// Enter: save the framed style + placement, then go borderless WS_POPUP
		// covering the current monitor. SWP_FRAMECHANGED fires WM_SIZE, which
		// re-runs LayoutBrowser (CEF child resizes) and re-reports preview rects.
		g_savedHostStyle = GetWindowLongPtrW(host, GWL_STYLE);
		g_savedHostPlacement.length = sizeof(g_savedHostPlacement);
		GetWindowPlacement(host, &g_savedHostPlacement);

		MONITORINFO mi = {};
		mi.cbSize = sizeof(mi);
		if (!GetMonitorInfoW(MonitorFromWindow(host, MONITOR_DEFAULTTONEAREST), &mi)) {
			error = "failed to query monitor";
			return false;
		}
		SetWindowLongPtrW(host, GWL_STYLE, WS_POPUP | WS_VISIBLE);
		SetWindowPos(host, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
			     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
			     SWP_FRAMECHANGED | SWP_NOACTIVATE);
		g_hostFullscreen = true;
	} else {
		// Exit: restore the saved style, re-apply the frame, then the saved
		// placement (also restores a prior maximized/normal state).
		SetWindowLongPtrW(host, GWL_STYLE, g_savedHostStyle);
		SetWindowPos(host, nullptr, 0, 0, 0, 0,
			     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		SetWindowPlacement(host, &g_savedHostPlacement);
		g_hostFullscreen = false;
	}

	HostLog(std::string("[bridge] window.toggleFullscreen -> ") + (g_hostFullscreen ? "on" : "off"));
	result = json{{"fullscreen", g_hostFullscreen}};
	return true;
}

// Resolve the caller-supplied windowId (0 = main shell; >0 = a detached window) to
// its top-level host HWND. The window controls below all act on the calling window
// via the same id plumbing detached windows already use for redock.
HWND ResolveWindowHwnd(const json &params)
{
	int windowId = 0;
	if (params.is_object() && params.contains("windowId") && params["windowId"].is_number_integer()) {
		windowId = params["windowId"].get<int>();
	}
	if (windowId == 0) {
		PreviewManager *pm = Preview::Instance();
		return pm ? pm->MainHostHwnd() : nullptr;
	}
	WindowManager *wm = WindowManager::Instance();
	return wm ? wm->HwndFor(windowId) : nullptr;
}

bool MethodWindowMinimize(const json &params, json &result, std::string &error)
{
	HWND hwnd = ResolveWindowHwnd(params);
	if (!hwnd) {
		error = "no such window";
		return false;
	}
	// Route through WM_SYSCOMMAND so the main window's minimize-to-tray policy (its
	// WM_SYSCOMMAND handler) still applies; detached windows just minimize.
	PostMessageW(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
	result = json{{"ok", true}};
	return true;
}

bool MethodWindowToggleMaximize(const json &params, json &result, std::string &error)
{
	HWND hwnd = ResolveWindowHwnd(params);
	if (!hwnd) {
		error = "no such window";
		return false;
	}
	const bool wasMaximized = IsZoomed(hwnd) != FALSE;
	ShowWindow(hwnd, wasMaximized ? SW_RESTORE : SW_MAXIMIZE);
	// The authoritative window.stateChanged push comes from the WM_SIZE handler; the
	// return value is a convenience echo.
	result = json{{"maximized", !wasMaximized}};
	return true;
}

bool MethodWindowClose(const json &params, json &result, std::string &error)
{
	HWND hwnd = ResolveWindowHwnd(params);
	if (!hwnd) {
		error = "no such window";
		return false;
	}
	// Post (not send) so the caller's cefQuery returns before the window teardown
	// runs its own message pump.
	PostMessageW(hwnd, WM_CLOSE, 0, 0);
	result = json{{"ok", true}};
	return true;
}

// Native projectors (fullscreen / windowed) — drive ProjectorManager and broadcast
// projector.changed on open/close.

bool MethodDisplayListMonitors(const json & /*params*/, json &result, std::string & /*error*/)
{
	json arr = json::array();
	for (const MonitorInfo &m : EnumerateMonitors()) {
		arr.push_back(json{
			{"index", m.index},
			{"name", m.name},
			{"x", m.x},
			{"y", m.y},
			{"width", m.width},
			{"height", m.height},
			{"primary", m.primary},
		});
	}
	result = json{{"monitors", std::move(arr)}};
	return true;
}

// Open a native interaction window for an interactive source (uuid or name).
bool MethodSourcesInteract(const json &params, json &result, std::string &error)
{
	obs_source_t *src = ResolveAudioSource(params); // addref'd; uuid-then-name
	if (!src) {
		error = "no such source";
		return false;
	}
	if (!(obs_source_get_output_flags(src) & OBS_SOURCE_INTERACTION)) {
		obs_source_release(src);
		error = "source is not interactive";
		return false;
	}

	InteractManager *im = Interact::Instance();
	if (!im) {
		obs_source_release(src);
		error = "interact manager not active";
		return false;
	}

	const int id = im->Open(src, error); // Open takes its own ref
	obs_source_release(src);
	if (id <= 0) {
		if (error.empty()) {
			error = "failed to open interaction window";
		}
		return false;
	}

	HostLog("[bridge] sources.interact -> ok id=" + std::to_string(id));
	result = json{{"ok", true}, {"interactId", id}};
	return true;
}

// Close a native interaction window by its id.
bool MethodSourcesCloseInteract(const json &params, json &result, std::string &error)
{
	InteractManager *im = Interact::Instance();
	if (!im) {
		error = "interact manager not active";
		return false;
	}
	if (!params.is_object() || !params.contains("interactId") || !params["interactId"].is_number_integer()) {
		error = "sources.closeInteract requires an integer 'interactId'";
		return false;
	}
	const int id = params["interactId"].get<int>();
	im->Close(id);
	HostLog("[bridge] sources.closeInteract id=" + std::to_string(id));
	result = json{{"ok", true}};
	return true;
}

bool MethodProjectorOpen(const json &params, json &result, std::string &error)
{
	ProjectorManager *pm = Projector::Instance();
	if (!pm) {
		error = "projector manager not active";
		return false;
	}

	// target: an object carrying the kind + an optional name/canvas.
	auto t = params.is_object() ? params.find("target") : params.end();
	if (!params.is_object() || t == params.end() || !t->is_object()) {
		error = "projector.open requires a 'target' object";
		return false;
	}
	const json &target = *t;

	ProjectorKind kind;
	if (!KindFromString(OptString(target, "kind"), kind)) {
		error = "unknown/missing target.kind";
		return false;
	}

	std::string name;
	std::string canvasUuid;
	switch (kind) {
	case ProjectorKind::Scene:
	case ProjectorKind::Source: {
		name = OptString(target, "name");
		if (name.empty()) {
			error = "target.name is required for a scene/source projector";
			return false;
		}
		obs_source_t *s = obs_get_source_by_name(name.c_str()); // addref'd; validation only
		if (!s) {
			error = "no source named '" + name + "'";
			return false;
		}
		const bool isScene = obs_scene_from_source(s) != nullptr;
		obs_source_release(s); // Open re-acquires its own addref
		if (kind == ProjectorKind::Scene && !isScene) {
			error = "'" + name + "' is not a scene";
			return false;
		}
		break;
	}
	case ProjectorKind::Canvas: {
		canvasUuid = OptString(target, "canvas");
		if (canvasUuid.empty()) {
			error = "target.canvas is required for a canvas projector";
			return false;
		}
		if (!ObsBootstrap::CanvasRuntime().Find(canvasUuid)) {
			error = "no live canvas mix for '" + canvasUuid + "'";
			return false;
		}
		break;
	}
	case ProjectorKind::Multiview: {
		// canvas is OPTIONAL for a multiview: empty = the Default canvas (global
		// scenes); a non-empty uuid must name a live additional-canvas mix.
		canvasUuid = OptString(target, "canvas");
		if (!canvasUuid.empty() && !ObsBootstrap::CanvasRuntime().Find(canvasUuid)) {
			error = "no live canvas mix for '" + canvasUuid + "'";
			return false;
		}
		break;
	}
	case ProjectorKind::Program:
		break;
	}

	const std::string mode = OptString(params, "mode");
	if (mode != "fullscreen" && mode != "windowed") {
		error = "mode must be 'fullscreen' or 'windowed'";
		return false;
	}
	const bool fullscreen = mode == "fullscreen";

	// Optional monitor index; required + bounds-checked for fullscreen, optional
	// (falls back to primary) for windowed.
	int monitor = -1;
	if (params.is_object()) {
		auto it = params.find("monitor");
		if (it != params.end() && it->is_number_integer()) {
			monitor = it->get<int>();
		}
	}
	const int monitorCount = int(EnumerateMonitors().size());
	if (fullscreen) {
		if (monitor < 0 || monitor >= monitorCount) {
			error = "fullscreen requires a valid 'monitor' index in [0," + std::to_string(monitorCount) +
				")";
			return false;
		}
	} else if (monitor < 0 || monitor >= monitorCount) {
		monitor = -1; // windowed: fall back to primary
	}

	const int id = pm->Open(kind, name, canvasUuid, fullscreen, monitor, error);
	if (id <= 0) {
		if (error.empty()) {
			error = "failed to open projector";
		}
		return false;
	}

	EmitEvent(EventNames::kProjectorChanged, json{{"opened", id}});
	HostLog("[bridge] projector.open -> ok id=" + std::to_string(id) + " kind=" + KindToString(kind) +
		" mode=" + mode);
	result = json{{"projectorId", id}};
	return true;
}

bool MethodProjectorClose(const json &params, json &result, std::string &error)
{
	ProjectorManager *pm = Projector::Instance();
	if (!pm) {
		error = "projector manager not active";
		return false;
	}
	if (!params.is_object() || !params.contains("projectorId") || !params["projectorId"].is_number_integer()) {
		error = "projector.close requires an integer 'projectorId'";
		return false;
	}
	const int id = params["projectorId"].get<int>();
	const bool closed = pm->Close(id);
	EmitEvent(EventNames::kProjectorChanged, json{{"closed", id}});
	HostLog("[bridge] projector.close id=" + std::to_string(id) + " -> " + (closed ? "ok" : "not found"));
	result = json{{"closed", closed}};
	return true;
}

bool MethodProjectorList(const json & /*params*/, json &result, std::string & /*error*/)
{
	ProjectorManager *pm = Projector::Instance();
	json arr = json::array();
	if (pm) {
		for (const ProjectorManager::ProjectorInfo &p : pm->List()) {
			json entry = json{
				{"projectorId", p.projectorId},
				{"kind", KindToString(p.kind)},
				{"mode", p.mode == ProjectorMode::Fullscreen ? "fullscreen" : "windowed"},
			};
			if (p.kind == ProjectorKind::Scene || p.kind == ProjectorKind::Source) {
				entry["name"] = p.name;
			}
			if (p.kind == ProjectorKind::Canvas) {
				entry["canvas"] = p.canvasUuid;
			}
			if (p.monitorIndex >= 0) {
				entry["monitor"] = p.monitorIndex;
			}
			arr.push_back(std::move(entry));
		}
	}
	result = json{{"projectors", std::move(arr)}};
	return true;
}

// --- embedded MCP server config (Settings UI bridge, Phase 5 stage 2) ---------

// Serialize the MCP server's config view to the McpConfig shape the Settings UI
// expects. When the server is absent (should not happen post-bootstrap), report a
// disabled, not-listening default so the UI degrades gracefully.
json McpConfigToJson(const McpServer *server)
{
	if (!server) {
		return json{{"enabled", false},     {"port", 47800},
			    {"token", ""},          {"allowMutations", true},
			    {"allowGoLive", false}, {"listening", false},
			    {"lastError", ""},      {"endpoint", "http://127.0.0.1:47800/mcp"}};
	}
	const McpServer::ConfigView v = server->GetConfigView();
	return json{{"enabled", v.enabled},
		    {"port", v.port},
		    {"token", v.token},
		    {"allowMutations", v.allowMutations},
		    {"allowGoLive", v.allowGoLive},
		    {"listening", v.listening},
		    {"lastError", v.lastError},
		    {"endpoint", v.endpoint}};
}

bool MethodMcpGetConfig(const json &, json &result, std::string &error)
{
	McpServer *server = Mcp::Instance();
	if (!server) {
		error = "mcp server not available";
		return false;
	}
	result = McpConfigToJson(server);
	return true;
}

bool MethodMcpSetConfig(const json &params, json &result, std::string &error)
{
	McpServer *server = Mcp::Instance();
	if (!server) {
		error = "mcp server not available";
		return false;
	}

	McpServer::ConfigPatch patch;
	if (params.is_object()) {
		auto en = params.find("enabled");
		if (en != params.end() && en->is_boolean()) {
			patch.hasEnabled = true;
			patch.enabled = en->get<bool>();
		}
		auto pt = params.find("port");
		if (pt != params.end() && pt->is_number_integer()) {
			patch.hasPort = true;
			patch.port = pt->get<int>();
		}
		auto am = params.find("allowMutations");
		if (am != params.end() && am->is_boolean()) {
			patch.hasAllowMutations = true;
			patch.allowMutations = am->get<bool>();
		}
		auto ag = params.find("allowGoLive");
		if (ag != params.end() && ag->is_boolean()) {
			patch.hasAllowGoLive = true;
			patch.allowGoLive = ag->get<bool>();
		}
	}

	const McpServer::ConfigView v = server->ApplyConfigPatch(patch);
	result = json{{"enabled", v.enabled},
		      {"port", v.port},
		      {"token", v.token},
		      {"allowMutations", v.allowMutations},
		      {"allowGoLive", v.allowGoLive},
		      {"listening", v.listening},
		      {"lastError", v.lastError},
		      {"endpoint", v.endpoint}};
	EmitEvent(EventNames::kMcpChanged, json::object());
	return true;
}

bool MethodMcpRegenerateToken(const json &, json &result, std::string &error)
{
	McpServer *server = Mcp::Instance();
	if (!server) {
		error = "mcp server not available";
		return false;
	}
	const std::string token = server->RegenerateToken();
	result = json{{"token", token}};
	EmitEvent(EventNames::kMcpChanged, json::object());
	return true;
}

// --- transactional settings snapshot / restore ------------------------------
// One C++ pair that captures EVERY settings category to a single blob and reverts
// it, so the Settings dialog's OK/Apply/Cancel footer can roll back on Cancel
// without per-item diffing in JS. Capture reuses each category's existing getter /
// store serializer; restore reuses each setter / store FromJson and re-emits the
// change events so the UI + preview resync. Token regeneration is intentionally
// NOT captured/reverted -- it is irreversible.

bool MethodSettingsSnapshot(const json & /*params*/, json &result, std::string & /*error*/)
{
	json snap;
	{
		json v;
		std::string e;
		MethodSettingsGetVideo(json::object(), v, e);
		snap["video"] = v;
	}
	{
		json a;
		std::string e;
		MethodSettingsGetAudio(json::object(), a, e);
		snap["audio"] = a;
	}
	{
		json d;
		std::string e;
		MethodAudioGetGlobalDevices(json::object(), d, e);
		snap["globalDevices"] = d;
	}
	snap["canvases"] = ObsBootstrap::Canvases().ToJson();
	snap["streamProfiles"] = ObsBootstrap::StreamProfiles().ToJson();
	snap["outputBindings"] = ObsBootstrap::OutputBindings().ToJson();
	snap["hotkeys"] = Hotkeys::Snapshot();
	{
		json m;
		std::string e;
		MethodMcpGetConfig(json::object(), m, e);
		snap["mcp"] = m;
	}
	result = std::move(snap);
	return true;
}

bool MethodSettingsRestore(const json &params, json &result, std::string &error)
{
	if (!RequireObject(params, "settings.restore", error)) {
		return false;
	}

	// Restore applies each section for real before the next can fail, and the earlier
	// setters (video/audio/global devices) commit side effects to libobs and disk that
	// can't be rolled back once a later section fails -- so this is partial-application,
	// not all-or-nothing. Collect each section's failure instead of discarding it, and
	// report an honest aggregate at the end rather than a blanket {ok:true}.
	json failures = json::array();
	auto fail = [&failures](const char *section, const std::string &e) {
		failures.push_back(json{{"section", section}, {"error", e}});
	};

	if (params.contains("video")) {
		json r;
		std::string e;
		if (!MethodSettingsSetVideo(params["video"], r, e)) {
			fail("video", e);
		}
	}
	if (params.contains("audio")) {
		json r;
		std::string e;
		if (!MethodSettingsSetAudio(params["audio"], r, e)) {
			fail("audio", e);
		}
	}
	if (params.contains("globalDevices") && params["globalDevices"].is_array()) {
		for (const json &slot : params["globalDevices"]) {
			if (!slot.is_object() || !slot.contains("channel")) {
				continue;
			}
			json setParams = json::object();
			setParams["channel"] = slot["channel"];
			// null/absent/non-string deviceId all mean "disable this slot".
			setParams["deviceId"] = (slot.contains("deviceId") && slot["deviceId"].is_string())
							? slot["deviceId"]
							: json(nullptr);
			json r;
			std::string e;
			if (!MethodAudioSetGlobalDevice(setParams, r, e)) {
				fail("globalDevices", e);
			}
		}
	}

	if (params.contains("streamProfiles")) {
		StreamProfileStore &s = ObsBootstrap::StreamProfiles();
		s.FromJson(params["streamProfiles"]);
		if (!s.Save()) {
			fail("streamProfiles", "failed to persist stream profiles");
		}
	}
	if (params.contains("outputBindings")) {
		OutputBindingStore &s = ObsBootstrap::OutputBindings();
		s.FromJson(params["outputBindings"]);
		if (!s.Save()) {
			fail("outputBindings", "failed to persist output bindings");
		}
	}

	if (params.contains("canvases")) {
		CanvasStore &cs = ObsBootstrap::Canvases();

		// Snapshot the pre-restore non-Default canvases (uuid + resolution + the
		// pipeline-affecting color tokens) so we can tell which live mixes were created
		// during the edit (tear them down) and which surviving ones had a
		// resolution/color change (revert the mix). A live canvas can't be edited, so
		// it can't appear in a revert delta; we still defensively skip touching one.
		struct Prev {
			std::string uuid;
			uint32_t w, h, outW, outH, fpsN, fpsD;
			std::string scale;
			std::string fmt, space, range;
		};
		std::vector<Prev> before;
		for (const CanvasDefinition &d : cs.Definitions()) {
			if (!d.isDefault) {
				before.push_back({d.uuid, d.width, d.height, d.outputWidth, d.outputHeight, d.fpsNum,
						  d.fpsDen, d.scaleType, d.color.format, d.color.space, d.color.range});
			}
		}

		cs.FromJson(params["canvases"]);
		if (!cs.Save()) {
			fail("canvases", "failed to persist canvases");
		}

		std::unordered_set<std::string> after;
		for (const CanvasDefinition &d : cs.Definitions()) {
			if (!d.isDefault) {
				after.insert(d.uuid);
			}
		}

		CanvasRuntime &rt = ObsBootstrap::CanvasRuntime();

		// 1) Tear down mixes for canvases created during the edit but absent after the
		// revert -- consumers first, then the mix, same as canvas.remove. Skip a
		// live canvas.
		for (const Prev &p : before) {
			if (after.count(p.uuid)) {
				continue;
			}
			if (CanvasIsLive(p.uuid)) {
				continue;
			}
			RemoveCanvasMixAndConsumers(p.uuid);
		}

		// 2) (Re)create objects for any restored canvas missing one (removed during
		// the edit, brought back by the revert). EnsureCanvas is idempotent.
		rt.SyncFromDefinitions();
		rt.ReconcileAll(); // gate restored canvases' mixes on their post-revert active state

		// 3) Revert the live mix resolution/color for surviving non-Default canvases
		// whose resolution or pipeline-affecting color changed during the edit,
		// invalidating their cached encoders too (mirrors MethodCanvasUpdate). The
		// Default canvas has no runtime mix -- its resolution/color drives global
		// video, handled below.
		for (const CanvasDefinition &d : cs.Definitions()) {
			if (d.isDefault || CanvasIsLive(d.uuid)) {
				continue;
			}
			for (const Prev &p : before) {
				if (p.uuid != d.uuid) {
					continue;
				}
				if (p.w != d.width || p.h != d.height || p.outW != d.outputWidth ||
				    p.outH != d.outputHeight || p.fpsN != d.fpsNum || p.fpsD != d.fpsDen ||
				    p.scale != d.scaleType || p.fmt != d.color.format || p.space != d.color.space ||
				    p.range != d.color.range) {
					ObsBootstrap::Multistream().InvalidateCanvasEncoders(d.uuid);
					rt.ResetVideo(d);
				}
				break;
			}
		}

		// 4) Make global video follow the restored Default canvas def so the main
		// pipeline (and output resolution + color) reverts too. Same apply the
		// single-canvas edit uses; skip while an output is live (a live pipeline
		// can't be reset). The reconcile ignores a failed reset so one bad canvas
		// can't abort the whole restore.
		if (!AnyOutputActive()) {
			std::string e;
			ApplyDefaultCanvasVideo(cs.Default(), e);
		}
	}

	if (params.contains("hotkeys")) {
		if (!Hotkeys::RestoreFromSnapshot(params["hotkeys"])) {
			fail("hotkeys", "failed to restore or persist hotkeys");
		}
	}
	if (params.contains("mcp")) {
		json r;
		std::string e;
		if (!MethodMcpSetConfig(params["mcp"], r, e)) {
			fail("mcp", e);
		}
	}

	EmitEvent(EventNames::kCanvasChanged, json::object());
	EmitEvent(EventNames::kStreamProfileChanged, json::object());
	EmitEvent(EventNames::kOutputBindingChanged, json::object());
	EmitEvent(EventNames::kMultistreamChanged, json{{"outputs", BuildStatusArray()}});
	EmitEvent(EventNames::kMcpChanged, json::object());

	// Honest aggregate: some sections applied, others may have failed after committing
	// side effects. Report which failed rather than a blanket {ok:true}. Returns true so
	// the partial result (and the events already emitted) reach JS; ok=false + the
	// per-section list is the same soft-failure shape multistream.startOutput uses.
	const bool ok = failures.empty();
	result = json{{"ok", ok}};
	if (!ok) {
		result["failed"] = std::move(failures);
	}
	return true;
}

// --- Scene collections ------------------------------------------------------
//
// The registry of per-collection scene sets (Phase 6a). list/create/rename/
// remove operate on the bootstrap-owned SceneCollections; switch reloads the
// active collection's scenes. Each mutation re-saves the index and emits
// collections.changed so every window re-lists.

bool MethodCollectionsList(const json & /*params*/, json &result, std::string & /*error*/)
{
	const SceneCollections &store = ObsBootstrap::SceneCollections();
	const std::string activeId = store.ActiveId();
	json arr = json::array();
	for (const SceneCollectionRecord &c : store.List()) {
		arr.push_back(json{{"id", c.id}, {"name", c.name}, {"active", c.id == activeId}});
	}
	result = std::move(arr);
	return true;
}

bool MethodCollectionsCreate(const json &params, json &result, std::string &error)
{
	if (ObsBootstrap::SceneCollections().IndexWasCorrupt()) {
		error = "scene collection index is corrupt; cannot modify collections";
		return false;
	}
	std::string name;
	if (!RequireStr(params, "collections.create", "name", name, error)) {
		return false;
	}
	const SceneCollectionRecord &added = ObsBootstrap::SceneCollections().Create(name);
	result = json{{"id", added.id}};
	EmitEvent(EventNames::kCollectionsChanged, json::object());
	return true;
}

bool MethodCollectionsRename(const json &params, json &result, std::string &error)
{
	if (ObsBootstrap::SceneCollections().IndexWasCorrupt()) {
		error = "scene collection index is corrupt; cannot modify collections";
		return false;
	}
	std::string id;
	if (!RequireStr(params, "collections.rename", "id", id, error)) {
		return false;
	}
	std::string name;
	if (!RequireStr(params, "collections.rename", "name", name, error)) {
		return false;
	}
	if (!ObsBootstrap::SceneCollections().Rename(id, name)) {
		error = "no scene collection with id '" + id + "'";
		return false;
	}
	result = json{{"id", id}, {"name", name}};
	EmitEvent(EventNames::kCollectionsChanged, json::object());
	return true;
}

bool MethodCollectionsDuplicate(const json &params, json &result, std::string &error)
{
	SceneCollections &store = ObsBootstrap::SceneCollections();
	if (store.IndexWasCorrupt()) {
		error = "scene collection index is corrupt; cannot modify collections";
		return false;
	}
	std::string name;
	if (!RequireStr(params, "collections.duplicate", "name", name, error)) {
		return false;
	}
	// Reject a name already in use so the duplicate stays distinguishable in the list.
	for (const SceneCollectionRecord &c : store.List()) {
		if (c.name == name) {
			error = "a scene collection named '" + name + "' already exists";
			return false;
		}
	}
	// Source: the explicit id, or the active collection when omitted.
	std::string sourceId = OptString(params, "id");
	if (sourceId.empty()) {
		sourceId = store.ActiveId();
	}
	const SceneCollectionRecord *added = store.Duplicate(sourceId, name, error);
	if (!added) {
		return false;
	}
	result = json{{"id", added->id}, {"name", added->name}};
	EmitEvent(EventNames::kCollectionsChanged, json::object());
	return true;
}

bool MethodCollectionsSwitch(const json &params, json &result, std::string &error)
{
	if (ObsBootstrap::SceneCollections().IndexWasCorrupt()) {
		error = "scene collection index is corrupt; cannot modify collections";
		return false;
	}
	std::string id;
	if (!RequireStr(params, "collections.switch", "id", id, error)) {
		return false;
	}
	if (!ObsBootstrap::SceneCollections().Switch(id, error)) {
		return false;
	}
	result = json{{"active", id}};
	// Switch already emitted collections.changed / scenes.changed / transitions.changed.
	return true;
}

bool MethodCollectionsRemove(const json &params, json &result, std::string &error)
{
	if (ObsBootstrap::SceneCollections().IndexWasCorrupt()) {
		error = "scene collection index is corrupt; cannot modify collections";
		return false;
	}
	std::string id;
	if (!RequireStr(params, "collections.remove", "id", id, error)) {
		return false;
	}
	if (!ObsBootstrap::SceneCollections().Remove(id, error)) {
		return false;
	}
	result = json{{"removed", id}};
	EmitEvent(EventNames::kCollectionsChanged, json::object());
	return true;
}

// --- OBS Studio importer (read-only) ----------------------------------------
//
// Thin wrappers over ObsImporter, which reads an external obs-studio install
// strictly read-only and creates NEW fork collections / stream profiles / canvas +
// audio state. All logic (scan, dependency closure, field mappings, live guard)
// lives in obs_importer.cpp; the importer emits its own change events.

bool MethodImporterScan(const json &params, json &result, std::string & /*error*/)
{
	result = ObsImporter::Scan(OptString(params, "path"));
	return true;
}

bool MethodImporterImport(const json &params, json &result, std::string &error)
{
	json r = ObsImporter::Import(params);
	if (!r.value("ok", false)) {
		error = r.value("error", std::string("import failed"));
		return false;
	}
	result = std::move(r);
	return true;
}

// Shared {active, canvas} payload so the virtualCam.* method results and the
// virtualCam.changed push report an identical shape.
json VirtualCamStatusJson()
{
	return json{
		{"active", ObsBootstrap::VirtualCam().IsActive()},
		{"canvas", ObsBootstrap::VirtualCam().TargetCanvas()},
	};
}

bool MethodVirtualCamStart(const json & /*params*/, json &result, std::string &error)
{
	if (!ObsBootstrap::VirtualCam().Start(error)) {
		return false;
	}
	result = json{{"ok", true}};
	return true;
}

bool MethodVirtualCamStop(const json & /*params*/, json &result, std::string & /*error*/)
{
	ObsBootstrap::VirtualCam().Stop();
	result = json{{"ok", true}};
	return true;
}

bool MethodVirtualCamStatus(const json & /*params*/, json &result, std::string & /*error*/)
{
	result = VirtualCamStatusJson();
	return true;
}

bool MethodVirtualCamGetConfig(const json & /*params*/, json &result, std::string & /*error*/)
{
	result = json{{"canvas", ObsBootstrap::VirtualCam().TargetCanvas()}};
	return true;
}

bool MethodVirtualCamSetConfig(const json &params, json &result, std::string &error)
{
	if (!params.is_object()) {
		error = "virtualCam.setConfig expects an object {canvas}";
		return false;
	}
	const std::string canvas = OptString(params, "canvas");
	ObsBootstrap::VirtualCam().SetTargetCanvas(canvas);
	const bool saved = ObsBootstrap::VirtualCam().Save();
	result = json{{"canvas", canvas}};
	EmitVirtualCamChanged();
	return PersistOrFail(saved, error);
}

// Custom browser docks (user-defined {id,title,url} web panels, Task 12). A flat
// per-app list persisted to browser_docks.json under the shared basic dir; no obs
// refs. The frontend owns add/edit/remove and writes back the whole list.

bool MethodBrowserDocksList(const json & /*params*/, json &result, std::string & /*error*/)
{
	const std::string path = MultistreamBasicPath("browser_docks.json");
	json arr = json::array();
	OBSDataAutoRelease root = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (root) {
		OBSDataArrayAutoRelease docks = obs_data_get_array(root, "docks");
		const size_t count = docks ? obs_data_array_count(docks) : 0;
		for (size_t i = 0; i < count; i++) {
			OBSDataAutoRelease item = obs_data_array_item(docks, i);
			const char *id = obs_data_get_string(item, "id");
			if (!id || !*id) {
				continue;
			}
			const char *title = obs_data_get_string(item, "title");
			const char *url = obs_data_get_string(item, "url");
			arr.push_back(json{{"id", id}, {"title", title ? title : ""}, {"url", url ? url : ""}});
		}
	}
	result = std::move(arr);
	return true;
}

bool MethodBrowserDocksSet(const json &params, json &result, std::string &error)
{
	if (!params.is_object() || !params.contains("docks") || !params["docks"].is_array()) {
		error = "browserDocks.set requires a 'docks' array";
		return false;
	}

	OBSDataAutoRelease root = obs_data_create();
	OBSDataArrayAutoRelease arr = obs_data_array_create();
	json saved = json::array();
	for (const auto &d : params["docks"]) {
		if (!d.is_object()) {
			continue;
		}
		auto idIt = d.find("id");
		auto urlIt = d.find("url");
		if (idIt == d.end() || !idIt->is_string() || idIt->get<std::string>().empty()) {
			continue; // malformed: drop
		}
		if (urlIt == d.end() || !urlIt->is_string()) {
			continue;
		}
		auto titleIt = d.find("title");
		const std::string id = idIt->get<std::string>();
		const std::string url = urlIt->get<std::string>();
		const std::string title = (titleIt != d.end() && titleIt->is_string()) ? titleIt->get<std::string>()
										       : std::string();

		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "id", id.c_str());
		obs_data_set_string(item, "title", title.c_str());
		obs_data_set_string(item, "url", url.c_str());
		obs_data_array_push_back(arr, item);
		saved.push_back(json{{"id", id}, {"title", title}, {"url", url}});
	}
	obs_data_set_array(root, "docks", arr);

	if (!SaveJsonAtomic(root, MultistreamBasicPath("browser_docks.json"))) {
		error = "failed to write browser_docks.json";
		return false;
	}
	result = std::move(saved);
	return true;
}

} // namespace

bool SwitchDefaultProgramScene(const std::string &sceneUuid)
{
	OBSSourceAutoRelease source = obs_get_source_by_uuid(sceneUuid.c_str()); // addref'd
	if (!source || !obs_scene_from_source(source)) {
		return false;
	}
	Transitions::SetProgramScene(source, true); // animate through the program transition
	ObsBootstrap::ApplyCanvasSceneLinks(sceneUuid);
	EmitScenesChanged(std::string());
	SceneCollection::Save();
	return true;
}

bool WriteJsonString(const char *file, const char *key, const std::string &value)
{
	const std::string path = MultistreamBasicPath(file);
	if (path.empty()) {
		return false;
	}
	OBSDataAutoRelease root = obs_data_create();
	obs_data_set_string(root, key, value.c_str());
	return SaveJsonAtomic(root, path);
}

std::string ReadJsonString(const char *file, const char *key)
{
	const std::string path = MultistreamBasicPath(file);
	if (path.empty()) {
		return std::string();
	}
	OBSDataAutoRelease root = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!root) {
		return std::string();
	}
	const char *v = obs_data_get_string(root, key);
	return v ? std::string(v) : std::string();
}

// Encode a tightly-packed (stride == w*4) GS_RGBA buffer to a PNG file via the
// Windows Imaging Component, mirroring the legacy SaveJxr COM pattern with the
// PNG container. WIC needs COM up on the calling thread (the CEF UI thread); we
// init MULTITHREADED and only balance with CoUninitialize when our init actually
// took (S_OK/S_FALSE) -- RPC_E_CHANGED_MODE means COM was already up in another
// model and our ref was not taken, so we must not uninit then.
bool EncodePngFile(const wchar_t *wpath, const uint8_t *pixels, uint32_t w, uint32_t h, std::string &errOut)
{
	using Microsoft::WRL::ComPtr;

	const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool needUninit = SUCCEEDED(coInit);

	HRESULT hr;
	{
		ComPtr<IWICImagingFactory> factory;
		hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
				      IID_PPV_ARGS(factory.GetAddressOf()));
		ComPtr<IWICStream> stream;
		if (SUCCEEDED(hr)) {
			hr = factory->CreateStream(stream.GetAddressOf());
		}
		if (SUCCEEDED(hr)) {
			hr = stream->InitializeFromFilename(wpath, GENERIC_WRITE);
		}
		ComPtr<IWICBitmapEncoder> encoder;
		if (SUCCEEDED(hr)) {
			hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
		}
		if (SUCCEEDED(hr)) {
			hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
		}
		ComPtr<IWICBitmapFrameEncode> frame;
		ComPtr<IPropertyBag2> options;
		if (SUCCEEDED(hr)) {
			hr = encoder->CreateNewFrame(frame.GetAddressOf(), options.GetAddressOf());
		}
		if (SUCCEEDED(hr)) {
			hr = frame->Initialize(options.Get());
		}
		if (SUCCEEDED(hr)) {
			hr = frame->SetSize(w, h);
		}
		WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
		if (SUCCEEDED(hr)) {
			hr = frame->SetPixelFormat(&format);
		}
		if (SUCCEEDED(hr)) {
			// GS_RGBA is R,G,B,A byte order with opaque alpha; a WIC-negotiated substitute
			// format is acceptable for opaque RGBA, so a format mismatch is not rejected.
			hr = frame->WritePixels(h, w * 4, w * h * 4, const_cast<BYTE *>(pixels));
		}
		if (SUCCEEDED(hr)) {
			hr = frame->Commit();
		}
		if (SUCCEEDED(hr)) {
			hr = encoder->Commit();
		}
	}

	if (needUninit) {
		CoUninitialize();
	}

	if (FAILED(hr)) {
		errOut = "failed to encode PNG";
		return false;
	}
	return true;
}

// Render `renderFn` into an outW*outH GS_RGBA texture (ortho'd to srcW*srcH source
// units, so outW/outH smaller than srcW/srcH downscales on the GPU instead of after
// the fact) and return the packed RGBA pixels. Shared core for the on-disk PNG
// screenshot path (CaptureToPng, src==out) and the in-memory thumbnail path
// (CaptureToThumbnailDataUri, out capped below src) below. Runs synchronously
// inside one obs graphics block: on D3D11 the staging map flushes and blocks until
// the copy completes, so the pixels are valid immediately (the legacy tick-split
// only existed to avoid stalling the render thread, irrelevant for a one-shot
// bridge call). Both gs objects are destroyed and the graphics context left on
// every path; the stage surface is unmapped before destroy.
bool RenderToRgbaPixels(uint32_t srcW, uint32_t srcH, uint32_t outW, uint32_t outH,
			 const std::function<void()> &renderFn, std::vector<uint8_t> &pixels, std::string &errOut)
{
	if (!srcW || !srcH || !outW || !outH) {
		errOut = "source has no video";
		return false;
	}

	obs_enter_graphics();

	gs_texrender_t *texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(outW, outH, GS_RGBA);

	bool beginOk = false;
	if (texrender && stage && gs_texrender_begin(texrender, outW, outH)) {
		beginOk = true;

		vec4 zero;
		vec4_zero(&zero);
		gs_clear(GS_CLEAR_COLOR, &zero, 0.0f, 0);

		gs_viewport_push();
		gs_projection_push();
		gs_ortho(0.0f, (float)srcW, 0.0f, (float)srcH, -100.0f, 100.0f);
		gs_set_viewport(0, 0, (int)outW, (int)outH);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		renderFn();

		gs_blend_state_pop();
		gs_projection_pop();
		gs_viewport_pop();
		gs_texrender_end(texrender);
	}

	bool mapped = false;
	if (beginOk) {
		gs_stage_texture(stage, gs_texrender_get_texture(texrender));
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(stage, &data, &linesize)) {
			pixels.resize((size_t)outW * outH * 4);
			for (uint32_t y = 0; y < outH; ++y) {
				memcpy(pixels.data() + (size_t)y * outW * 4, data + (size_t)y * linesize,
				       (size_t)outW * 4);
			}
			gs_stagesurface_unmap(stage);
			mapped = true;
		}
	}

	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(texrender);
	obs_leave_graphics();

	if (!beginOk || !mapped) {
		errOut = "failed to render screenshot";
		return false;
	}
	return true;
}

// Render `renderFn` into a w*h GS_RGBA texture and write it out as PNG. Output is
// native size with a 1:1 viewport (no letterbox).
bool CaptureToPng(uint32_t w, uint32_t h, const std::function<void()> &renderFn, const std::string &outPath,
		  std::string &errOut)
{
	std::vector<uint8_t> pixels;
	if (!RenderToRgbaPixels(w, h, w, h, renderFn, pixels, errOut)) {
		return false;
	}

	wchar_t *wpath = nullptr;
	os_utf8_to_wcs_ptr(outPath.c_str(), 0, &wpath);
	if (!wpath) {
		errOut = "failed to encode PNG";
		return false;
	}
	const bool ok = EncodePngFile(wpath, pixels.data(), w, h, errOut);
	bfree(wpath);
	return ok;
}

// Same as EncodePngFile but sinks into a growable in-memory IStream (SHCreateMemStream)
// instead of a file, for callers that need the PNG bytes directly rather than a file
// on disk (an inline data-URI thumbnail).
bool EncodePngMemory(const uint8_t *pixels, uint32_t w, uint32_t h, std::vector<unsigned char> &out,
		      std::string &errOut)
{
	using Microsoft::WRL::ComPtr;

	const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool needUninit = SUCCEEDED(coInit);

	ComPtr<IStream> memStream;
	memStream.Attach(SHCreateMemStream(nullptr, 0));
	HRESULT hr = memStream ? S_OK : E_OUTOFMEMORY;
	{
		ComPtr<IWICImagingFactory> factory;
		if (SUCCEEDED(hr)) {
			hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
					      IID_PPV_ARGS(factory.GetAddressOf()));
		}
		ComPtr<IWICStream> wicStream;
		if (SUCCEEDED(hr)) {
			hr = factory->CreateStream(wicStream.GetAddressOf());
		}
		if (SUCCEEDED(hr)) {
			hr = wicStream->InitializeFromIStream(memStream.Get());
		}
		ComPtr<IWICBitmapEncoder> encoder;
		if (SUCCEEDED(hr)) {
			hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
		}
		if (SUCCEEDED(hr)) {
			hr = encoder->Initialize(wicStream.Get(), WICBitmapEncoderNoCache);
		}
		ComPtr<IWICBitmapFrameEncode> frame;
		ComPtr<IPropertyBag2> options;
		if (SUCCEEDED(hr)) {
			hr = encoder->CreateNewFrame(frame.GetAddressOf(), options.GetAddressOf());
		}
		if (SUCCEEDED(hr)) {
			hr = frame->Initialize(options.Get());
		}
		if (SUCCEEDED(hr)) {
			hr = frame->SetSize(w, h);
		}
		WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
		if (SUCCEEDED(hr)) {
			hr = frame->SetPixelFormat(&format);
		}
		if (SUCCEEDED(hr)) {
			// GS_RGBA is R,G,B,A byte order with opaque alpha; a WIC-negotiated substitute
			// format is acceptable for opaque RGBA, so a format mismatch is not rejected.
			hr = frame->WritePixels(h, w * 4, w * h * 4, const_cast<BYTE *>(pixels));
		}
		if (SUCCEEDED(hr)) {
			hr = frame->Commit();
		}
		if (SUCCEEDED(hr)) {
			hr = encoder->Commit();
		}
	}

	if (SUCCEEDED(hr)) {
		STATSTG stat{};
		hr = memStream->Stat(&stat, STATFLAG_NONAME);
		if (SUCCEEDED(hr)) {
			const ULONG size = static_cast<ULONG>(stat.cbSize.QuadPart);
			out.resize(size);
			LARGE_INTEGER zero{};
			hr = memStream->Seek(zero, STREAM_SEEK_SET, nullptr);
		}
		if (SUCCEEDED(hr)) {
			ULONG read = 0;
			hr = memStream->Read(out.data(), (ULONG)out.size(), &read);
		}
	}

	if (needUninit) {
		CoUninitialize();
	}

	if (FAILED(hr)) {
		errOut = "failed to encode PNG";
		return false;
	}
	return true;
}

// Render `renderFn` (a source's own, standalone render -- not a specific scene
// item) at a capped thumbnail size and return it inlined as a PNG data URI. Used
// by sources.thumbnail for the add-source picker's existing-source tiles: no disk
// file (CEF's app:// origin can't load file:// paths anyway, see file.readDataUri,
// and a per-tile timestamped screenshot would spam the user's screenshots folder
// for what is purely a UI preview).
bool CaptureToThumbnailDataUri(uint32_t srcW, uint32_t srcH, const std::function<void()> &renderFn,
				std::string &dataUri, std::string &errOut)
{
	constexpr uint32_t kMaxDim = 160;
	uint32_t outW = srcW;
	uint32_t outH = srcH;
	if (outW > kMaxDim || outH > kMaxDim) {
		const double scale = std::min((double)kMaxDim / outW, (double)kMaxDim / outH);
		outW = std::max<uint32_t>(1, (uint32_t)(outW * scale));
		outH = std::max<uint32_t>(1, (uint32_t)(outH * scale));
	}

	std::vector<uint8_t> pixels;
	if (!RenderToRgbaPixels(srcW, srcH, outW, outH, renderFn, pixels, errOut)) {
		return false;
	}
	std::vector<unsigned char> png;
	if (!EncodePngMemory(pixels.data(), outW, outH, png, errOut)) {
		return false;
	}
	dataUri = "data:image/png;base64," + EncodeBase64(png);
	return true;
}

// Existing-source picker thumbnail: params {name} -> {dataUri}. Renders the named
// source standalone (not tied to any particular scene item, since the whole point
// of sources.listExisting is offering sources not yet in the target scene) at a
// capped size and inlines it as a PNG data URI. Sources with no video (audio-only,
// zero-size, or a failed render) return an error; the caller falls back to a type
// icon.
bool MethodSourcesThumbnail(const json &params, json &result, std::string &error)
{
	std::string name;
	if (!RequireStr(params, "sources.thumbnail", "name", name, error)) {
		return false;
	}
	OBSSourceAutoRelease source = obs_get_source_by_name(name.c_str());
	if (!source) {
		error = "no source named '" + name + "'";
		return false;
	}
	const uint32_t w = obs_source_get_width(source);
	const uint32_t h = obs_source_get_height(source);
	if (!w || !h) {
		error = "source has no video";
		return false;
	}

	obs_source_t *src = source;
	auto renderFn = [src]() {
		obs_source_inc_showing(src);
		obs_source_video_render(src);
		obs_source_dec_showing(src);
	};

	std::string dataUri;
	if (!CaptureToThumbnailDataUri(w, h, renderFn, dataUri, error)) {
		return false;
	}
	result = json{{"dataUri", dataUri}};
	return true;
}

// Replace characters Windows forbids in file names (and control chars) with '_';
// fall back to `fallback` when nothing usable remains.
std::string SanitizeScreenshotName(const std::string &name, const char *fallback)
{
	std::string out;
	out.reserve(name.size());
	for (unsigned char c : name) {
		if (c < 0x20 || strchr("<>:\"/\\|?*", c) != nullptr) {
			out.push_back('_');
		} else {
			out.push_back((char)c);
		}
	}
	if (out.empty()) {
		out = fallback;
	}
	return out;
}

// Build <config>/braidcast/screenshots/<name>_<YYYY-MM-DD_HH-MM-SS>.png,
// creating the directory if needed. Returns false (with errOut) when the path
// can't be resolved or the directory can't be created.
bool BuildScreenshotPath(const std::string &name, const char *fallback, std::string &fullPath, std::string &errOut)
{
	const std::string dir = BraidcastConfigPath("screenshots");
	if (dir.empty()) {
		errOut = "failed to resolve screenshots directory";
		return false;
	}
	if (os_mkdirs(dir.c_str()) == MKDIR_ERROR) {
		errOut = "failed to create screenshots directory";
		return false;
	}

	const time_t t = time(nullptr);
	struct tm lt;
	localtime_s(&lt, &t);
	char ts[32];
	strftime(ts, sizeof(ts), "%Y-%m-%d_%H-%M-%S", &lt);

	fullPath = dir + "/" + SanitizeScreenshotName(name, fallback) + "_" + ts + ".png";
	return true;
}

// Capture the composited program for the addressed canvas (absent/Default ->
// the global pipeline; otherwise the additional canvas's mix) to a PNG.
bool MethodScreenshotTakeProgram(const json &params, json &result, std::string &error)
{
	const CanvasTarget t = ResolveCanvasTarget(params);

	uint32_t w = 0;
	uint32_t h = 0;
	std::function<void()> renderFn;
	std::string name;

	if (t.isAdditional) {
		obs_canvas_t *cv = ObsBootstrap::CanvasRuntime().Find(t.uuid);
		if (!cv) {
			error = "canvas not found";
			return false;
		}
		obs_video_info ovi;
		if (!obs_canvas_get_video_info(cv, &ovi)) {
			error = "canvas has no video";
			return false;
		}
		w = ovi.base_width;
		h = ovi.base_height;
		renderFn = [cv]() {
			obs_canvas_render(cv);
		};
		const char *n = obs_canvas_get_name(cv);
		name = (n && *n) ? n : "Program";
	} else {
		obs_video_info ovi;
		if (!obs_get_video_info(&ovi)) {
			error = "no video";
			return false;
		}
		w = ovi.base_width;
		h = ovi.base_height;
		renderFn = []() {
			obs_render_main_texture();
		};
		name = ObsBootstrap::Canvases().Default().name;
		if (name.empty()) {
			name = "Program";
		}
	}

	std::string fullPath;
	if (!BuildScreenshotPath(name, "Program", fullPath, error)) {
		return false;
	}
	const bool captured = CaptureToPng(w, h, renderFn, fullPath, error);
	if (!captured) {
		return false;
	}

	blog(LOG_INFO, "Saved program screenshot to '%s'", fullPath.c_str());
	EmitEvent(EventNames::kScreenshotSaved, json{{"path", fullPath}});
	result = json{{"ok", true}, {"path", fullPath}};
	return true;
}

// Capture a single scene item's source to a PNG. Resolution mirrors
// sceneItems.setScaleFilter ({canvas,scene,id}); the addref'd scene source is
// held across the capture (it owns the item + source) and released on every path.
bool MethodScreenshotTakeSource(const json &params, json &result, std::string &error)
{
	obs_source_t *sceneSource = ResolveTargetScene(params); // addref'd
	if (!sceneSource) {
		error = "no scene";
		return false;
	}

	// `id` is optional: with a scene-item id, screenshot that item's source; without
	// one, screenshot the scene source itself ("Screenshot Scene" -- a scene renders
	// as a source). The Default path resolves the scene by name; an additional canvas
	// returns its current scene.
	obs_source_t *src = sceneSource; // borrowed default: the scene itself
	if (params.contains("id") && !params["id"].is_null()) {
		int64_t id = 0;
		if (!ItemIdFromParams(params, id, error)) {
			obs_source_release(sceneSource);
			return false;
		}
		obs_sceneitem_t *item = FindSceneItem(obs_scene_from_source(sceneSource), id);
		if (!item) {
			obs_source_release(sceneSource);
			error = "no scene item with id " + std::to_string(id);
			return false;
		}
		src = obs_sceneitem_get_source(item); // borrowed; kept alive by sceneSource
	}
	const uint32_t w = obs_source_get_width(src);
	const uint32_t h = obs_source_get_height(src);
	if (!w || !h) {
		obs_source_release(sceneSource);
		error = "source has no video";
		return false;
	}
	const char *n = obs_source_get_name(src);
	const std::string name = (n && *n) ? n : "Source";

	std::string fullPath;
	if (!BuildScreenshotPath(name, "Source", fullPath, error)) {
		obs_source_release(sceneSource);
		return false;
	}

	auto renderFn = [src]() {
		obs_source_inc_showing(src);
		obs_source_video_render(src);
		obs_source_dec_showing(src);
	};
	const bool ok = CaptureToPng(w, h, renderFn, fullPath, error);
	obs_source_release(sceneSource);
	if (!ok) {
		return false;
	}

	blog(LOG_INFO, "Saved source screenshot to '%s'", fullPath.c_str());
	EmitEvent(EventNames::kScreenshotSaved, json{{"path", fullPath}});
	result = json{{"ok", true}, {"path", fullPath}};
	return true;
}

// Return this session's log file path plus its contents. The tail is capped at
// kLogTailCap bytes so a long-running session can't produce a giant payload; the
// file is read fresh from disk so it reflects everything flushed so far.
bool MethodLogGetCurrent(const json & /*params*/, json &result, std::string & /*error*/)
{
	constexpr std::streamoff kLogTailCap = 512 * 1024;

	const std::string path = SessionLog::CurrentPath();
	std::string contents;
	if (!path.empty()) {
		std::ifstream in(std::filesystem::u8path(path), std::ios::in | std::ios::binary);
		if (in) {
			in.seekg(0, std::ios::end);
			const std::streamoff size = in.tellg();
			std::streamoff start = 0;
			if (size > kLogTailCap) {
				start = size - kLogTailCap;
			}
			in.seekg(start, std::ios::beg);
			contents.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
	}

	result = json{{"path", path}, {"contents", contents}};
	return true;
}

// ---- diagnostics (gated DEBUG channel) -------------------------------------

// The current DEBUG gate + this session's log file path (so the UI can show and
// open it).
bool MethodDiagnosticsGet(const json & /*params*/, json &result, std::string & /*error*/)
{
	result = json{{"debug", Log::DebugEnabled()}, {"logPath", SessionLog::CurrentPath()}};
	return true;
}

// Flip + persist the DEBUG gate, then push debug.changed so every window updates.
bool MethodDiagnosticsSetDebug(const json &params, json &result, std::string &error)
{
	if (!RequireObject(params, "diagnostics.setDebug", error)) {
		return false;
	}
	auto it = params.find("enabled");
	if (it == params.end() || !it->is_boolean()) {
		error = "diagnostics.setDebug: 'enabled' must be a boolean";
		return false;
	}
	const bool enabled = it->get<bool>();

	DiagnosticsSettings ds;
	ds.Load();
	ds.debugLogging = enabled;
	const bool saved = ds.Save();
	Log::SetDebug(enabled);

	result = json{{"debug", enabled}};
	EmitEvent(EventNames::kDebugChanged, result);
	return PersistOrFail(saved, error);
}

// Reveal the session log's containing folder in the OS file manager.
bool MethodDiagnosticsOpenLogFolder(const json & /*params*/, json &result, std::string &error)
{
	const std::string path = SessionLog::CurrentPath();
	if (path.empty()) {
		error = "no session log file open";
		return false;
	}
	if (!RevealInFileManager(std::filesystem::u8path(path).parent_path().wstring(), error)) {
		return false;
	}
	result = json{{"ok", true}};
	return true;
}

// ---- OAuth (Phase 8a) ------------------------------------------------------
//
// Generic, registry-dispatched account-connection surface. The interactive grant
// runs on a detached worker (blocking HTTP / a loopback listener); progress is
// reported back to JS via EmitEvent (oauth.connectProgress / oauth.status /
// oauth.connectError), all of which marshal to TID_UI and no-op after teardown. No
// per-platform branches here -- everything routes through the provider the registry
// resolves by id, and the per-strategy grant lives in AuthStrategy::authorize.

// Connection state from the account store: one row per stored account, keyed by
// accountId (providerId:userId).
json BuildOAuthStatusArray()
{
	json arr = json::array();
	for (const auto &entry : OAuth::Accounts().All()) {
		const std::string &accountId = entry.first;
		const OAuth::OAuthAccount &acct = entry.second;
		// Connected and needs-reconnect are the shared OAuth gates (see registry.hpp):
		// connected requires a current-scope token WITH a refresh credential; a partial
		// no-refresh record is neither, so it stops surfacing chips in Events/Multichat.
		arr.push_back(json{
			{"accountId", accountId},
			{"providerId", acct.providerId},
			{"connected", OAuth::IsAccountConnected(acct)},
			{"needsReconnect", OAuth::AccountNeedsReconnect(acct)},
			{"login", acct.login},
			{"displayName", acct.displayName},
			{"avatarUrl", acct.avatarUrl},
		});
	}
	return arr;
}

void EmitOAuthStatus()
{
	AsyncTask::PostToUi([] { EmitEvent(EventNames::kOauthStatus, BuildOAuthStatusArray()); });
}

void EmitOAuthConnectError(const std::string &profileUuid, const std::string &providerId, const std::string &message)
{
	AsyncTask::PostToUi([profileUuid, providerId, message] {
		EmitEvent(EventNames::kOauthConnectError,
			  json{{"profileUuid", profileUuid}, {"providerId", providerId}, {"error", message}});
	});
}

void OpenUrlInBrowser(const std::string &url)
{
	if (url.empty()) {
		return;
	}
	ShellExecuteW(nullptr, L"open", Utf8ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// The interactive connect flow, run on a detached worker thread. Owns everything
// by value so it can safely outlive the originating bridge call. Strategy-agnostic:
// it builds an AuthContext (progress emitter + cancel probe) and hands the whole
// grant to the provider's AuthStrategy::authorize -- no per-strategy branching here.
// The strategy reports progress via oauth.connectProgress (device-code:
// phase="deviceCode"; loopback: phase="browser"); a top-level "openUrl" in a
// progress payload is opened in the system browser here and stripped before the
// event reaches JS.
void RunOAuthConnect(std::string providerId, std::string profileUuid, uint64_t gen)
try {
	// This worker owns the connect flow only while it remains the newest attempt.
	const auto superseded = [gen] {
		return g_oauthGen.load(std::memory_order_acquire) != gen;
	};

	OAuth::StreamProvider *provider = OAuth::Registry().Get(providerId);
	if (!provider) {
		EmitOAuthConnectError(profileUuid, providerId, "unknown provider");
		return;
	}
	OAuth::AuthStrategy *auth = provider->auth();
	if (!auth) {
		EmitOAuthConnectError(profileUuid, providerId, "provider has no auth strategy");
		return;
	}

	OAuth::AuthContext ctx;
	ctx.canceled = [gen] {
		return !g_oauthRunning.load(std::memory_order_acquire) ||
		       g_oauthCancel.load(std::memory_order_acquire) ||
		       g_oauthGen.load(std::memory_order_acquire) != gen;
	};
	ctx.emitProgress = [profileUuid, providerId](const json &payload) {
		json p = payload;
		std::string openUrl;
		auto it = p.find("openUrl");
		if (it != p.end() && it->is_string()) {
			openUrl = it->get<std::string>();
			p.erase(it);
		}
		AsyncTask::PostToUi([profileUuid, providerId, p, openUrl] {
			json event = json{{"profileUuid", profileUuid}, {"providerId", providerId}};
			event.update(p);
			EmitEvent(EventNames::kOauthConnectProgress, event);
			if (!openUrl.empty()) {
				OpenUrlInBrowser(openUrl);
			}
		});
	};

	OAuth::OAuthAccount acct;
	acct.providerId = providerId;

	std::string err;
	if (!auth->authorize(ctx, acct, err)) {
		// A cancel (user closed the modal, or the bridge tore down) returns false
		// with no error to surface -- stay silent, exactly as the legacy poll loop
		// did. Emit only a genuine failure.
		if (!ctx.canceled()) {
			EmitOAuthConnectError(profileUuid, providerId, err);
		}
		return;
	}

	// Tokens are in hand. Fetch identity (best-effort: keep the precious one-time
	// device-flow tokens even if the identity call fails), then persist + notify.
	std::string idErr;
	if (!provider->fetchIdentity(acct, idErr)) {
		HostLog("[oauth] fetchIdentity failed for " + providerId + ": " + idErr);
	}

	// Stream-key autofill (Twitch /helix/streams/key): fetch before persisting so a
	// token rotated by the key call is captured in the Put below, then write the key
	// into the linked stream profile on the UI thread (the profile store is UI-thread
	// owned). Providers without a key endpoint return false (no-op).
	std::string streamKey, keyErr;
	const bool haveKey = provider->fetchStreamKey(acct, streamKey, keyErr) && !streamKey.empty();
	if (!haveKey && !keyErr.empty()) {
		HostLog("[oauth] fetchStreamKey skipped for " + providerId + ": " + keyErr);
	}

	// A newer connect has taken over, or the bridge is tearing down; don't persist
	// tokens, flip the UI, or -- crucially -- resurrect a hub that Shutdown() may have
	// just stopped. Probe both immediately before the StartAccount/Start block below.
	if (superseded() || g_bridgeShutdown.load(std::memory_order_acquire)) {
		return;
	}

	// An account with no userId cannot be keyed (identity fetch failed). Do not
	// persist a headless record; surface a connect error so the UI can retry.
	if (acct.userId.empty()) {
		EmitOAuthConnectError(profileUuid, providerId, "identity fetch failed; try connecting again");
		return;
	}
	// A record with no refresh token is unusable -- IsAccountConnected treats it as not
	// connected, yet other paths would still act on its access token until it expires.
	// Refuse to persist it and surface a connect error so the UI can retry.
	if (acct.refresh.empty()) {
		EmitOAuthConnectError(profileUuid, providerId, "no refresh token returned; try connecting again");
		return;
	}
	const std::string accountId = OAuth::AccountId(acct);
	OAuth::Accounts().Put(accountId, acct);

	// Link the originating stream profile to this account (UI-thread-owned store).
	AsyncTask::PostToUi([profileUuid, accountId] {
		StreamProfile *p = ObsBootstrap::StreamProfiles().Find(profileUuid);
		if (!p) {
			return;
		}
		p->accountId = accountId;
		ObsBootstrap::StreamProfiles().Save();
		EmitEvent(EventNames::kStreamProfileChanged, json::object());
	});

	EmitOAuthStatus();

	// Phase 9.2a: start the account's live-events transport now that it is connected
	// (the events feed is account-lifecycle, not go-live). No-op until a provider
	// returns a non-null makeEvents(acct) transport.
	Events::Hub().StartAccount(accountId, acct);

	if (haveKey) {
		AsyncTask::PostToUi([profileUuid, streamKey] {
			StreamProfile *p = ObsBootstrap::StreamProfiles().Find(profileUuid);
			if (!p) {
				return;
			}
			if (!p->settings) {
				p->settings = obs_data_create();
			}
			obs_data_set_string(p->settings, "key", streamKey.c_str());
			// rtmp_common resolves an ingest URL only when "server" == "auto"
			// (see rtmp-common.c rtmp_common_url); an OAuth key writeback with no
			// server leaves it empty, so obs_output_start bails with no error.
			// Seed "auto" once so the service can pick its recommended ingest.
			if (!obs_data_has_user_value(p->settings, "server")) {
				obs_data_set_string(p->settings, "server", "auto");
			}
			ObsBootstrap::StreamProfiles().Save();
			EmitEvent(EventNames::kStreamProfileChanged, json::object());
		});
	}
} catch (const std::exception &e) {
	// authorize() touches Winsock/crypto/HTTP; an escape on this detached worker
	// would std::terminate the process. Convert it to a clean connectError, but
	// stay silent if the user already canceled or a newer attempt superseded us
	// (matches the cancel path above).
	if (g_oauthRunning.load(std::memory_order_acquire) && !g_oauthCancel.load(std::memory_order_acquire) &&
	    g_oauthGen.load(std::memory_order_acquire) == gen) {
		EmitOAuthConnectError(profileUuid, providerId, std::string("connect failed: ") + e.what());
	}
} catch (...) {
	if (g_oauthRunning.load(std::memory_order_acquire) && !g_oauthCancel.load(std::memory_order_acquire) &&
	    g_oauthGen.load(std::memory_order_acquire) == gen) {
		EmitOAuthConnectError(profileUuid, providerId, "connect failed: unknown error");
	}
}

bool MethodOAuthProviders(const json & /*params*/, json &result, std::string & /*error*/)
{
	json arr = json::array();
	for (OAuth::StreamProvider *provider : OAuth::Registry().All()) {
		try {
			arr.push_back(provider->capabilityJson());
		} catch (const std::exception &e) {
			HostLog(std::string("[oauth] capabilityJson failed: ") + e.what());
		}
	}
	result = std::move(arr);
	return true;
}

bool MethodOAuthConnect(const json &params, json &result, std::string &error)
{
	const std::string providerId = OptString(params, "providerId");
	const std::string profileUuid = OptString(params, "profileUuid");
	if (providerId.empty() || profileUuid.empty()) {
		error = "oauth.connect requires non-empty 'providerId' and 'profileUuid'";
		return false;
	}
	if (!OAuth::Registry().Get(providerId)) {
		error = "unknown provider: " + providerId;
		return false;
	}

	// The flow is long-running (user authorizes out-of-band); run it detached and
	// answer immediately. Progress arrives as oauth.deviceCode/status/connectError.
	// Bump the generation first so any in-flight worker sees itself superseded and
	// tears down before this one begins waiting.
	g_oauthCancel.store(false, std::memory_order_release);
	const uint64_t gen = g_oauthGen.fetch_add(1, std::memory_order_acq_rel) + 1;
	AsyncTask::RunAsync([providerId, profileUuid, gen] { RunOAuthConnect(providerId, profileUuid, gen); });

	result = json{{"ok", true}, {"pending", true}};
	return true;
}

// Full teardown for one OAuth account: drop it from the store, forget its refresh
// flight lock, stop its live-events + chat transports, unlink every profile still
// pointing at it, then re-resolve chat and push the updated profile/oauth status. The
// single teardown path shared by manual disconnect (MethodOAuthDisconnect), profile
// delete (MethodStreamProfileRemove), and the boot orphan reconcile. UI thread only.
void TeardownAccount(const std::string &accountId)
{
	// Captured before Remove() so the provider's grant can still be revoked below --
	// the store entry (and its tokens) is gone once Remove() returns.
	const std::optional<OAuth::OAuthAccount> acctForRevoke = OAuth::Accounts().Get(accountId);
	OAuth::Accounts().Remove(accountId);
	// Prune the provider's per-account refresh flight lock along with the account so a
	// disconnected account leaves no mutex behind (accountId is "<providerId>:<userId>").
	if (OAuth::StreamProvider *prov = OAuth::Registry().Get(accountId.substr(0, accountId.find(':')))) {
		prov->auth()->ForgetAccount(accountId);
		// Best-effort: revoke the grant at the provider so disconnect actually kills
		// it there too (YouTube Developer Policies require this), not just locally.
		// Never blocks or can fail this teardown -- see AuthStrategy::Revoke.
		if (acctForRevoke) {
			prov->auth()->Revoke(*acctForRevoke);
		}
	}
	Events::Hub().StopAccount(accountId);
	// Chat is live-only, so re-resolve it only while streaming: a mid-stream disconnect
	// drops the removed account's transport (Start() enumerates only still-connected
	// accounts); off-air the hub is stopped and must stay down.
	if (ObsBootstrap::Multistream().AnyLive()) {
		Chat::Hub().Start();
	}

	// Unlink every profile that referenced this account.
	for (StreamProfile &p : ObsBootstrap::StreamProfiles().AllMutable()) {
		if (p.accountId == accountId) {
			p.accountId.clear();
		}
	}
	ObsBootstrap::StreamProfiles().Save();
	EmitEvent(EventNames::kStreamProfileChanged, json::object());
	EmitEvent(EventNames::kOauthStatus, BuildOAuthStatusArray());
}

bool MethodOAuthDisconnect(const json &params, json &result, std::string &error)
{
	std::string accountId;
	if (!RequireStr(params, "oauth.disconnect", "accountId", accountId, error)) {
		return false;
	}
	const bool force = params.is_object() && params.value("force", false);

	// An account referenced by more than one profile gets a confirm gate: unlinking it
	// drops it from every profile at once, so name them and require force to proceed.
	json refs = json::array();
	for (const StreamProfile &p : ObsBootstrap::StreamProfiles().Profiles()) {
		if (p.accountId == accountId) {
			refs.push_back(json{{"uuid", p.uuid}, {"name", p.label}});
		}
	}
	if (!force && refs.size() > 1) {
		result = json{{"needsConfirm", true}, {"profiles", std::move(refs)}};
		return true;
	}

	TeardownAccount(accountId);
	result = json{{"ok", true}};
	return true;
}

void ReconcileOrphanedAccounts()
{
	// Reclaim every stored account no profile references -- an orphan left behind when
	// its owning stream profile was deleted before the delete path cleaned up accounts
	// (pre-fix leak). All() returns a snapshot copy, and TeardownAccount mutates the
	// store, so collect the orphan ids first, then tear each down through the shared path.
	std::vector<std::string> orphans;
	for (const auto &entry : OAuth::Accounts().All()) {
		if (!ObsBootstrap::StreamProfiles().ReferencesAccount(entry.first)) {
			orphans.push_back(entry.first);
		}
	}
	for (const std::string &accountId : orphans) {
		HostLog("[oauth] reclaiming orphaned account (no stream profile references it): " + accountId);
		TeardownAccount(accountId);
	}
}

// One rtmp_common profile whose key predates the OAuth connect flow's key writeback
// and must be re-fetched from the provider (see SelfHealStreamCredentials below).
struct StaleCredentialProfile {
	std::string profileUuid;
	std::string accountId;
};

// Re-fetch + write back the stream key for each profile in `profiles`, one at a time
// on this detached worker (fetchStreamKey does blocking network and may rotate the
// account's token, so it must not run on the UI thread). WriteIngestToProfile
// marshals its own writeback to TID_UI; a fetch failure is logged and the profile is
// left untouched -- this is best-effort background healing, never a user-facing
// connect error. Spawned once by SelfHealStreamCredentials.
void SelfHealFetchWorker(std::vector<StaleCredentialProfile> profiles)
{
	for (const StaleCredentialProfile &sp : profiles) {
		if (g_bridgeShutdown.load(std::memory_order_acquire)) {
			return;
		}
		std::optional<OAuth::OAuthAccount> acct = OAuth::Accounts().Get(sp.accountId);
		if (!acct) {
			continue;
		}
		OAuth::StreamProvider *provider = OAuth::Registry().Get(acct->providerId);
		if (!provider) {
			continue;
		}
		std::string key, err;
		if (!provider->fetchStreamKey(*acct, key, err) || key.empty()) {
			HostLog("[oauth] self-heal: fetchStreamKey failed for profile " + sp.profileUuid +
				(err.empty() ? "" : (": " + err)));
			continue;
		}
		if (WriteIngestToProfile(sp.profileUuid, "auto", key)) {
			HostLog("[oauth] self-heal: re-fetched stream key for profile " + sp.profileUuid);
		}
	}
}

// Launch-time self-heal for stream profiles seeded before the OAuth connect flow
// wrote "server=auto" alongside the key (see RunOAuthConnect above): such a profile
// has a key but no server, so rtmp_common can't resolve an ingest and
// obs_output_start bails deep in libobs with an empty error. Only rtmp_common
// profiles linked to a currently connected account are touched -- custom-RTMP/WHIP
// profiles carry a user-entered server/key, and YouTube profiles are never
// rtmp_common (their ingest is seeded per-broadcast by WriteIngestToProfile at go
// live, and the base fetchStreamKey it doesn't override returns false), so both are
// naturally excluded. The key-present/server-missing case is healed inline here (no
// network); a profile missing its key entirely is snapshotted and re-fetched on a
// background worker. Call once, on the UI thread, after the profile + account stores
// load.
void SelfHealStreamCredentials()
{
	std::vector<StaleCredentialProfile> needFetch;
	bool changed = false;

	for (StreamProfile &p : ObsBootstrap::StreamProfiles().AllMutable()) {
		if (p.accountId.empty() || p.serviceId != "rtmp_common") {
			continue;
		}
		std::optional<OAuth::OAuthAccount> acct = OAuth::Accounts().Get(p.accountId);
		if (!acct || !OAuth::IsAccountConnected(*acct)) {
			continue;
		}

		const std::string key = p.Key();
		const bool hasServer = p.settings && obs_data_has_user_value(p.settings, "server");
		if (!key.empty() && hasServer) {
			continue; // already fully seeded
		}
		if (!key.empty()) {
			if (!p.settings) {
				p.settings = obs_data_create();
			}
			obs_data_set_string(p.settings, "server", "auto");
			HostLog("[oauth] self-heal: seeded server=auto for stale stream profile " + p.uuid);
			changed = true;
			continue;
		}
		needFetch.push_back({p.uuid, p.accountId});
	}

	if (changed) {
		ObsBootstrap::StreamProfiles().Save();
		EmitEvent(EventNames::kStreamProfileChanged, json::object());
	}
	if (!needFetch.empty()) {
		// Route through the registered-async seam (not a raw detached thread) so
		// WaitForDrain counts this worker at teardown; otherwise it is invisible to the
		// drain and can touch the profile store / bridge after shutdown frees them.
		AsyncTask::RunAsync(
			[profiles = std::move(needFetch)]() mutable { SelfHealFetchWorker(std::move(profiles)); });
	}
}

// The connected accounts for one provider -- the reuse-picker source. Filters the
// account store by providerId; needsReconnect flags a scope-stale token (same rule as
// oauth.status) so the picker can label it.
bool MethodOAuthAccounts(const json &params, json &result, std::string &error)
{
	std::string providerId;
	if (!RequireStr(params, "oauth.accounts", "providerId", providerId, error)) {
		return false;
	}
	json arr = json::array();
	for (const auto &entry : OAuth::Accounts().All()) {
		const OAuth::OAuthAccount &acct = entry.second;
		if (acct.providerId != providerId) {
			continue;
		}
		arr.push_back(json{
			{"accountId", entry.first},
			{"login", acct.login},
			{"displayName", acct.displayName},
			{"needsReconnect", OAuth::AccountNeedsReconnect(acct)},
			{"avatarUrl", acct.avatarUrl},
		});
	}
	result = std::move(arr);
	return true;
}

// Link a profile to an already-connected account (connect-once reuse -- no grant). The
// account must exist in the store; this is the sole difference from the connect flow's
// implicit link (which runs after a fresh grant).
bool MethodOAuthLinkAccount(const json &params, json &result, std::string &error)
{
	const std::string profileUuid = OptString(params, "profileUuid");
	const std::string accountId = OptString(params, "accountId");
	if (profileUuid.empty() || accountId.empty()) {
		error = "oauth.linkAccount requires 'profileUuid' and 'accountId'";
		return false;
	}
	if (!OAuth::Accounts().Get(accountId)) {
		error = "account not found; reconnect";
		return false;
	}
	StreamProfile *p = ObsBootstrap::StreamProfiles().Find(profileUuid);
	if (!p) {
		error = "profile not found";
		return false;
	}
	p->accountId = accountId;
	const bool saved = ObsBootstrap::StreamProfiles().Save();
	EmitEvent(EventNames::kStreamProfileChanged, json::object());
	result = json{{"ok", true}};
	return PersistOrFail(saved, error);
}

bool MethodOAuthStatus(const json & /*params*/, json &result, std::string & /*error*/)
{
	result = BuildOAuthStatusArray();
	return true;
}

bool MethodOAuthCancelConnect(const json & /*params*/, json &result, std::string & /*error*/)
{
	g_oauthCancel.store(true, std::memory_order_release);
	result = json{{"ok", true}};
	return true;
}

// ---- streamMeta (Phase 8a; async lane Phase 8b) ----------------------------
//
// Platform stream-metadata read/search/write, dispatched through the provider the
// registry resolves. Coherence: load the account from the store by accountId (passed
// directly by the caller -- the frontend resolves it from the profile's linked account,
// so this async lane never touches the UI-thread-owned StreamProfileStore) and let the
// provider refresh it in place via ensureFresh, which is the SOLE token writer -- it
// re-reads + writes back rotated tokens under its single-flight lock (keyed by
// AccountId(acct)). The handler bodies must NEVER Put a pre-call
// snapshot back: with concurrent same-profile calls a non-refreshing caller would
// clobber a peer's freshly rotated (one-time-use) refresh token, bricking the
// account. So they read tokens via Get and never write them.
// These run on the deferred-callback async lane (g_asyncMethods + RunAsyncMethod):
// each body executes on its own detached worker (one thread per call; a thread pool
// is a future optimization, not needed now) so libcurl never blocks TID_UI, then the
// captured CEF callback resolves on the UI thread. The bodies stay shaped as plain
// MethodFn (params -> result|error); RunAsyncMethod drives them off-thread. The 8a
// single-flight refresh made the concurrent provider path safe to run off-thread.

bool MethodStreamMetaGet(const json &params, json &result, std::string &error)
{
	std::string accountId;
	if (!RequireStr(params, "streamMeta.get", "accountId", accountId, error)) {
		return false;
	}
	std::optional<OAuth::OAuthAccount> stored = OAuth::Accounts().Get(accountId);
	if (!stored) {
		error = "not connected";
		return false;
	}
	OAuth::StreamProvider *provider = OAuth::Registry().Get(stored->providerId);
	if (!provider) {
		error = "unknown provider: " + stored->providerId;
		return false;
	}
	// No scopeVer gate here: reading channel metadata uses a scope present in every
	// scope version (Twitch's Get Channel Information needs no user scope at all), so
	// gating on scopeVer would force needless reconnects on tokens that can already
	// read it. A genuinely missing/insufficient scope surfaces as an HTTP 403 that
	// getMetadata already returns. Newly-added event scopes stay gated on their own
	// paths (chat/event hubs, viewer poller). A refresh-less record is still refused:
	// it is a broken/partial connect that must not drive a read on an access token
	// that can't be renewed.
	if (stored->refresh.empty()) {
		error = "account not connected; reconnect";
		return false;
	}

	OAuth::OAuthAccount acct = *stored;
	json out;
	std::string err;
	bool ok;
	try {
		ok = provider->getMetadata(acct, out, err);
	} catch (const std::exception &e) {
		error = std::string("streamMeta.get failed: ") + e.what();
		return false;
	}
	// No Put: ensureFresh already persisted any rotated token under its lock.
	// Writing back acct here would clobber a concurrent peer's rotated token.
	if (!ok) {
		error = err;
		return false;
	}
	result = std::move(out);
	return true;
}

bool MethodStreamMetaSearchCategories(const json &params, json &result, std::string &error)
{
	std::string providerId;
	const std::string query = OptString(params, "query");
	if (!RequireStr(params, "streamMeta.searchCategories", "providerId", providerId, error)) {
		return false;
	}
	OAuth::StreamProvider *provider = OAuth::Registry().Get(providerId);
	if (!provider) {
		error = "unknown provider: " + providerId;
		return false;
	}

	// Use any connected account for this provider (search needs a usable user token;
	// a behind-scope or partial no-refresh account must not be used). Shared gate.
	bool found = false;
	OAuth::OAuthAccount acct;
	for (const auto &entry : OAuth::Accounts().All()) {
		if (entry.second.providerId == providerId && OAuth::IsAccountConnected(entry.second)) {
			acct = entry.second;
			found = true;
			break;
		}
	}
	if (!found) {
		error = "connect an account first";
		return false;
	}

	json out;
	std::string err;
	bool ok;
	try {
		ok = provider->searchCategories(acct, query, out, err);
	} catch (const std::exception &e) {
		error = std::string("streamMeta.searchCategories failed: ") + e.what();
		return false;
	}
	// No Put: ensureFresh is the sole token writer (it re-reads/writes by AccountId).
	if (!ok) {
		error = err;
		return false;
	}
	result = std::move(out);
	return true;
}

bool MethodStreamMetaSet(const json &params, json &result, std::string &error)
{
	// accountId identifies the account whose token/provider the write goes through;
	// profileUuid is forwarded only into applyMetadata, which marshals its ingest
	// writeback (WriteIngestToProfile) to the UI thread -- the sole safe profile touch.
	std::string accountId;
	const std::string profileUuid = OptString(params, "profileUuid");
	if (!RequireStr(params, "streamMeta.set", "accountId", accountId, error)) {
		return false;
	}
	std::optional<OAuth::OAuthAccount> stored = OAuth::Accounts().Get(accountId);
	if (!stored) {
		error = "not connected";
		return false;
	}
	OAuth::StreamProvider *provider = OAuth::Registry().Get(stored->providerId);
	if (!provider) {
		error = "unknown provider: " + stored->providerId;
		return false;
	}
	// No scopeVer gate here: writing channel metadata needs only the channel-edit
	// scope (Twitch channel:manage:broadcast), which predates every scopeVer bump --
	// the v3 bump added EventSub read scopes, unrelated to editing title/category. A
	// token issued before the bump is fully authorized to edit channel info, so gating
	// on scopeVer would block it needlessly. A genuinely missing scope surfaces as an
	// HTTP 403 that applyMetadata already returns. Newly-added event scopes stay gated
	// on their own paths (chat/event hubs, viewer poller). A refresh-less record is
	// still refused: a broken/partial connect must not drive a write.
	if (stored->refresh.empty()) {
		error = "account not connected; reconnect";
		return false;
	}
	const json fields = params.is_object() && params.contains("fields") ? params["fields"] : json::object();
	// goingLive distinguishes the go-live prelude from a standalone "Edit stream info"
	// push (same account state otherwise). A create-per-go-live provider (YouTube) uses
	// it to avoid creating a broadcast for a pre-live edit; persistent-channel providers
	// ignore it. Absent -> false.
	const bool goingLive = params.is_object() && params.value("goingLive", false);

	OAuth::OAuthAccount acct = *stored;
	std::string err;
	bool ok;
	try {
		ok = provider->applyMetadata(acct, profileUuid, fields, goingLive, err);
	} catch (const std::exception &e) {
		error = std::string("streamMeta.set failed: ") + e.what();
		return false;
	}
	// No Put: ensureFresh already persisted any rotated token under its lock.
	// Writing back acct here would clobber a concurrent peer's rotated token.
	if (!ok) {
		error = err;
		return false;
	}
	EmitEvent(EventNames::kStreamMetaChanged, json{{"profileUuid", profileUuid}});
	result = json{{"ok", true}};
	return true;
}

// getSaved/save read and write the remembered-metadata store (StreamMetaStore).
// Unlike get/set/searchCategories these touch no provider and no network, so they
// stay on the normal synchronous methods table -- a thin JSON<->store adapter that
// never interprets the field bags.
bool MethodStreamMetaGetSaved(const json &params, json &result, std::string &error)
{
	std::string accountId;
	if (!RequireStr(params, "streamMeta.getSaved", "accountId", accountId, error)) {
		return false;
	}
	StreamMetaStore &store = ObsBootstrap::StreamMeta();
	json streams = json::object();
	if (params.is_object() && params.contains("profileUuids") && params["profileUuids"].is_array()) {
		for (const auto &entry : params["profileUuids"]) {
			if (!entry.is_string()) {
				continue;
			}
			const std::string uuid = entry.get<std::string>();
			json bag = store.StreamOverride(uuid);
			if (bag.is_object() && !bag.empty()) {
				streams[uuid] = std::move(bag);
			}
		}
	}
	result = json{{"channel", store.ChannelDefaults(accountId)}, {"streams", std::move(streams)}};
	return true;
}

bool MethodStreamMetaSave(const json &params, json &result, std::string &error)
{
	std::string accountId;
	if (!RequireStr(params, "streamMeta.save", "accountId", accountId, error)) {
		return false;
	}
	StreamMetaStore &store = ObsBootstrap::StreamMeta();
	const json channel = params.is_object() && params.contains("channel") && params["channel"].is_object()
				     ? params["channel"]
				     : json::object();
	store.PutChannelDefaults(accountId, channel);
	if (params.is_object() && params.contains("streams") && params["streams"].is_object()) {
		// The frontend sends the channel's COMPLETE stream set on every save: a
		// non-empty bag upserts that override, an empty bag ({}) clears it. Without
		// the clear path a toggled-off override would linger on disk and resurrect
		// (and re-apply) on the next go-live.
		for (const auto &entry : params["streams"].items()) {
			if (entry.value().is_object() && !entry.value().empty()) {
				store.PutStreamOverride(entry.key(), entry.value());
			} else {
				store.RemoveStreamOverride(entry.key());
			}
		}
	}
	const bool saved = store.Save();
	result = json{{"ok", true}};
	return PersistOrFail(saved, error);
}

// ---- chat (Phase 9.0) ------------------------------------------------------
//
// The multichat send/state surface, routed through the ChatHub (Chat::Hub()).
// The hub owns the live per-platform transports started on go-live; tokens never
// leave C++. Events the hub + the T6 viewer poller push to JS:
//   - "chat.message"    one normalized message (see chat_transport.hpp)
//   - "chat.state"      per-platform { platform, connected, error? }
//   - "viewers.changed" { perAccount: {accountId:n}, total }  (emitted directly
//                        by the T6 ViewerPoller via Bridge::EmitEvent)
// All three flow through the existing alive-guarded EmitEvent path -- no new emit
// plumbing is needed; the chat.* helpers live in the hub (RouteEmit).

bool MethodChatSend(const json &params, json &result, std::string &error)
{
	std::vector<std::string> platforms;
	if (params.is_object() && params.contains("platforms") && params["platforms"].is_array()) {
		for (const auto &p : params["platforms"]) {
			if (p.is_string()) {
				platforms.push_back(p.get<std::string>());
			}
		}
	}
	std::string text;
	if (!RequireStr(params, "chat.send", "text", text, error)) {
		return false;
	}
	// Empty platforms = send to every connected platform. Each transport's send runs
	// on its own worker inside the hub; a failure emits a chat.state error.
	Chat::Hub().SendToPlatforms(platforms, text);
	result = json{{"ok", true}};
	return true;
}

bool MethodChatState(const json & /*params*/, json &result, std::string & /*error*/)
{
	result = Chat::Hub().State();
	return true;
}

// ---- events (Phase 9.2a) ---------------------------------------------------
//
// The persisted, de-duplicated live-events feed. events.list returns the stored
// history newest-first; events.clear wipes it and pushes an empty events.backfill so
// the dock resets. Real-time pushes arrive as events.new (one event) / events.backfill
// (an array) from the EventHub, started on account connect and stopped on disconnect /
// shutdown -- account-lifecycle, not go-live.

bool MethodEventsList(const json & /*params*/, json &result, std::string & /*error*/)
{
	json arr = json::array();
	for (const Events::NormalizedEvent &ev : Events::Store().List()) {
		arr.push_back(ev.ToJson());
	}
	result = std::move(arr);
	return true;
}

bool MethodEventsClear(const json & /*params*/, json &result, std::string & /*error*/)
{
	Events::Store().Clear();
	EmitEvent(EventNames::kEventsBackfill, json::array());
	result = json{{"ok", true}};
	return true;
}

// --- overlays (Phase 9.3 widget layer) --------------------------------------

// Compact standard-base64 decoder (no other decoder exists in this TU). Ignores
// ASCII whitespace; rejects any non-alphabet byte and data after padding.
bool DecodeBase64(const std::string &in, std::vector<unsigned char> &out)
{
	auto val = [](unsigned char c) -> int {
		if (c >= 'A' && c <= 'Z') {
			return c - 'A';
		}
		if (c >= 'a' && c <= 'z') {
			return c - 'a' + 26;
		}
		if (c >= '0' && c <= '9') {
			return c - '0' + 52;
		}
		if (c == '+') {
			return 62;
		}
		if (c == '/') {
			return 63;
		}
		return -1;
	};
	out.clear();
	int buf = 0;
	int bits = 0;
	bool padded = false;
	for (unsigned char c : in) {
		if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
			continue;
		}
		if (c == '=') {
			padded = true;
			continue;
		}
		if (padded) {
			return false; // data after padding
		}
		const int v = val(c);
		if (v < 0) {
			return false;
		}
		buf = (buf << 6) | v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out.push_back(static_cast<unsigned char>((buf >> bits) & 0xFF));
		}
	}
	return true;
}

bool MethodOverlaysList(const json & /*params*/, json &result, std::string & /*error*/)
{
	const int port = Overlay::Store().Port();
	json arr = json::array();
	for (const Overlay::Widget &w : Overlay::Store().List()) {
		arr.push_back(w.ToListJson(port));
	}
	result = std::move(arr);
	return true;
}

bool MethodOverlaysGet(const json &p, json &result, std::string &error)
{
	const std::string id = OptString(p, "id");
	std::optional<Overlay::Widget> w = Overlay::Store().Get(id);
	if (!w) {
		error = "no such overlay: " + id;
		return false;
	}
	result = w->ToJson();
	result["url"] = Overlay::WidgetUrl(*w, Overlay::Store().Port());
	return true;
}

bool MethodOverlaysCreate(const json &p, json &result, std::string & /*error*/)
{
	const std::string name = OptString(p, "name");
	std::string type = OptString(p, "type");
	if (type.empty()) {
		type = "alertbox";
	}
	Overlay::Widget w = Overlay::Store().Create(name.empty() ? "New Overlay" : name, type);
	result = w.ToJson();
	result["url"] = Overlay::WidgetUrl(w, Overlay::Store().Port());
	EmitEvent(EventNames::kOverlaysChanged, json::object());
	return true;
}

bool MethodOverlaysUpdate(const json &p, json &result, std::string &error)
{
	const std::string id = OptString(p, "id");
	if (!Overlay::Store().Update(id, p)) {
		error = "no such overlay: " + id;
		return false;
	}
	EmitEvent(EventNames::kOverlaysChanged, json::object());
	result = json{{"ok", true}};
	return true;
}

bool MethodOverlaysDuplicate(const json &p, json &result, std::string &error)
{
	const std::string id = OptString(p, "id");
	std::optional<Overlay::Widget> w = Overlay::Store().Duplicate(id);
	if (!w) {
		error = "no such overlay: " + id;
		return false;
	}
	result = w->ToJson();
	result["url"] = Overlay::WidgetUrl(*w, Overlay::Store().Port());
	EmitEvent(EventNames::kOverlaysChanged, json::object());
	return true;
}

bool MethodOverlaysDelete(const json &p, json &result, std::string &error)
{
	const std::string id = OptString(p, "id");
	if (!Overlay::Store().Delete(id)) {
		error = "no such overlay: " + id;
		return false;
	}
	EmitEvent(EventNames::kOverlaysChanged, json::object());
	result = json{{"removed", id}};
	return true;
}

bool MethodOverlaysUrl(const json &p, json &result, std::string &error)
{
	const std::string id = OptString(p, "id");
	std::optional<Overlay::Widget> w = Overlay::Store().Get(id);
	if (!w) {
		error = "no such overlay: " + id;
		return false;
	}
	result = json{{"url", Overlay::WidgetUrl(*w, Overlay::Store().Port())}};
	return true;
}

bool MethodOverlaysServerInfo(const json & /*params*/, json &result, std::string & /*error*/)
{
	int port = Overlay::Server().Port();
	if (port == 0) {
		port = Overlay::Store().Port();
	}
	result = json{
		{"port", port},
		{"listening", Overlay::Server().IsListening()},
		{"portChanged", Overlay::Server().PortChanged()},
	};
	return true;
}

// overlays.test {id,type,overrides?}: fire a synthetic event straight to one widget's
// SSE sockets (never the store, so test alerts don't persist or dedupe against real
// events). Per-type defaults are a data table so a new type is a data change. Runs on
// the async lane: BroadcastTo does blocking socket sends (bounded by the overlay
// server's send timeout), which must never run on TID_UI -- every browser source on
// stream renders there.
bool MethodOverlaysTest(const json &p, json &result, std::string &error)
{
	const std::string id = OptString(p, "id");
	const std::string type = OptString(p, "type");
	if (id.empty() || type.empty()) {
		error = "overlays.test requires id and type";
		return false;
	}
	const json overrides = (p.is_object() && p.contains("overrides") && p["overrides"].is_object())
				       ? p["overrides"]
				       : json::object();

	Events::NormalizedEvent ev;
	static std::atomic<uint64_t> counter{0};
	ev.id = "test-" + std::to_string(++counter);
	ev.platform = "twitch";
	ev.type = type;
	ev.ts = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
					     std::chrono::system_clock::now().time_since_epoch())
					     .count());
	ev.actorName = "Test User";

	static const std::unordered_map<std::string, std::function<void(Events::NormalizedEvent &)>> kDefaults = {
		{"cheer",
		 [](Events::NormalizedEvent &e) {
			 e.amount = 500;
		 }},
		{"raid",
		 [](Events::NormalizedEvent &e) {
			 e.amount = 12;
		 }},
		{"sub",
		 [](Events::NormalizedEvent &e) {
			 e.tier = "1000";
			 e.months = 3;
		 }},
		{"resub",
		 [](Events::NormalizedEvent &e) {
			 e.tier = "1000";
			 e.months = 3;
		 }},
		{"subgift",
		 [](Events::NormalizedEvent &e) {
			 e.count = 5;
			 e.tier = "1000";
		 }},
		{"superchat",
		 [](Events::NormalizedEvent &e) {
			 e.amount = 499;
			 e.currency = "USD";
			 e.message = "Nice!";
		 }},
		{"supersticker",
		 [](Events::NormalizedEvent &e) {
			 e.amount = 499;
			 e.currency = "USD";
			 e.message = "Nice!";
		 }},
	};
	auto def = kDefaults.find(type);
	if (def != kDefaults.end()) {
		def->second(ev);
	}

	// Overrides win over every default (present, well-typed keys only).
	if (overrides.contains("platform") && overrides["platform"].is_string()) {
		ev.platform = overrides["platform"].get<std::string>();
	}
	if (overrides.contains("actorName") && overrides["actorName"].is_string()) {
		ev.actorName = overrides["actorName"].get<std::string>();
	}
	if (overrides.contains("actorColor") && overrides["actorColor"].is_string()) {
		ev.actorColor = overrides["actorColor"].get<std::string>();
	}
	if (overrides.contains("amount") && overrides["amount"].is_number()) {
		ev.amount = overrides["amount"].get<int64_t>();
	}
	if (overrides.contains("currency") && overrides["currency"].is_string()) {
		ev.currency = overrides["currency"].get<std::string>();
	}
	if (overrides.contains("tier") && overrides["tier"].is_string()) {
		ev.tier = overrides["tier"].get<std::string>();
	}
	if (overrides.contains("months") && overrides["months"].is_number()) {
		ev.months = overrides["months"].get<int>();
	}
	if (overrides.contains("count") && overrides["count"].is_number()) {
		ev.count = overrides["count"].get<int>();
	}
	if (overrides.contains("message") && overrides["message"].is_string()) {
		ev.message = overrides["message"].get<std::string>();
	}

	Overlay::Server().BroadcastTo(id, ev);
	result = json{{"ok", true}};
	return true;
}

// overlays.uploadAsset {id,key,kind,base64} -> {path:"assets/<file>"} (async lane).
bool MethodOverlaysUploadAsset(const json &p, json &result, std::string &error)
{
	const std::string id = OptString(p, "id");
	const std::string key = OptString(p, "key");
	const std::string kind = OptString(p, "kind");
	const std::string b64 = OptString(p, "base64");
	if (id.empty() || key.empty() || b64.empty()) {
		error = "overlays.uploadAsset requires id, key, base64";
		return false;
	}
	std::vector<unsigned char> bytes;
	if (!DecodeBase64(b64, bytes)) {
		error = "invalid base64";
		return false;
	}
	if (bytes.size() > 8u * 1024 * 1024) {
		error = "asset exceeds 8 MB";
		return false;
	}
	const std::string rel = Overlay::Store().AddAsset(id, key, kind, bytes);
	if (rel.empty()) {
		error = "failed to store asset";
		return false;
	}
	result = json{{"path", rel}};
	return true;
}

// overlays.addToScene {id, canvas?, scene?} -> {id:<sceneItemId>, source:<name>}.
// Mirrors MethodSourcesCreate exactly, differing only in creating a browser_source
// pre-seeded with the widget URL + the target canvas's base resolution.
bool MethodOverlaysAddToScene(const json &params, json &result, std::string &error)
{
	const std::string id = OptString(params, "id");
	std::optional<Overlay::Widget> w = Overlay::Store().Get(id);
	if (!w) {
		error = "no such overlay: " + id;
		return false;
	}
	const std::string url = Overlay::WidgetUrl(*w, Overlay::Store().Port());

	std::string name = OptString(params, "name");
	if (name.empty()) {
		name = w->name + " (Overlay)";
	}
	obs_source_t *clash = obs_get_source_by_name(name.c_str());
	if (clash) {
		obs_source_release(clash);
		error = "a source named '" + name + "' already exists";
		return false;
	}

	obs_source_t *sceneSource = ResolveTargetScene(params); // addref'd
	if (!sceneSource) {
		error = "no scene to add the source to";
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);

	uint32_t baseW = 1920;
	uint32_t baseH = 1080;
	ResolveBaseSize(params, baseW, baseH);

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "url", url.c_str());
	obs_data_set_int(settings, "width", baseW);
	obs_data_set_int(settings, "height", baseH);
	obs_source_t *source = obs_source_create("browser_source", name.c_str(), settings, nullptr); // create-ref
	obs_data_release(settings);
	if (!source) {
		obs_source_release(sceneSource);
		error = "obs_source_create failed for browser_source";
		return false;
	}

	obs_sceneitem_t *item = obs_scene_add(scene, source); // scene takes its own ref
	const int64_t itemId = item ? obs_sceneitem_get_id(item) : 0;

	json before, after;
	if (item) {
		before = StateBase(params, source);
		after = CaptureItemSnapshot(params, sceneSource, item);
	}
	obs_source_release(source); // drop the create-ref; scene holds the source

	EmitSceneItemsChanged(sceneSource, ResolveCanvasTarget(params).uuid);

	if (!item) {
		obs_source_release(sceneSource);
		error = "obs_scene_add failed";
		return false;
	}
	PersistSourceState(sceneSource);
	obs_source_release(sceneSource);
	ObsBootstrap::Undo().AddAction("Add " + name, kRemoveItemBySource, kAddItemFromSnapshot, before.dump(),
				       after.dump());
	result = json{{"id", itemId}, {"source", name}};
	return true;
}

// The async-lane driver: run a plain MethodFn `work` on a detached worker, then
// resolve the captured CEF `callback` back on the UI thread. Exactly one
// resolution per query (the only path is the single PostToUi below); a resolve
// that lands after Bridge::Shutdown is dropped by PostToUi's alive-guard; any
// exception on the worker becomes a Failure rather than escaping the thread.
// `work` may call EmitEvent (e.g. streamMeta.set's "streamMeta.changed") -- that
// self-marshals to TID_UI. This is the shared lane 8c/8d platform calls reuse.
void RunAsyncMethod(std::string method, const json &params, CefRefPtr<CefMessageRouterBrowserSide::Callback> callback,
		    MethodFn work)
{
	AsyncTask::RunAsync([method = std::move(method), params, callback, work = std::move(work)]() mutable {
		json result;
		std::string error;
		bool ok;
		try {
			ok = work(params, result, error);
		} catch (const std::exception &e) {
			ok = false;
			error = std::string("unhandled exception: ") + e.what();
		} catch (...) {
			ok = false;
			error = "unhandled exception";
		}
		AsyncTask::PostToUi([method = std::move(method), ok, result = std::move(result),
				     error = std::move(error), callback]() {
			// Wrap result.dump(): the handler already ran in the worker's try/catch, but
			// dump() can still throw (e.g. invalid UTF-8 in the result), which must not
			// escape into CEF. Convert to the same 500 Failure shape the sync lane
			// produces, and tag the FAIL message with the method name (which the async
			// lane's callback body otherwise dropped) for parity with the sync lane's log.
			try {
				if (ok) {
					callback->Success(result.dump());
					HostLog("[bridge] " + method + " -> ok (async)");
				} else {
					callback->Failure(404, method + ": " + error);
					HostLog("[bridge] " + method + " -> FAIL (async: " + error + ")");
				}
			} catch (const std::exception &e) {
				const std::string msg = "unhandled exception in " + method + ": " + e.what();
				callback->Failure(500, msg);
				HostLog("[bridge] " + msg);
			} catch (...) {
				const std::string msg = "unhandled exception in " + method;
				callback->Failure(500, msg);
				HostLog("[bridge] " + msg);
			}
		});
	});
}

void Init()
{
	g_methods = {
		{"getVersion", MethodGetVersion},
		{"getCurrentScene", MethodGetCurrentScene},
		{"listScenes", MethodListScenes},
		{"getStreamingState", MethodGetStreamingState},
		{"streaming.start", MethodStreamingStart},
		{"streaming.stop", MethodStreamingStop},
		{"preview.setRect", MethodPreviewSetRect},
		{"preview.hide", MethodPreviewHide},
		{"preview.destroy", MethodPreviewDestroy},
		{"preview.select", MethodPreviewSelect},
		{"scenes.list", MethodScenesList},
		{"scenes.create", MethodScenesCreate},
		{"scenes.remove", MethodScenesRemove},
		{"scenes.setCurrent", MethodScenesSetCurrent},
		{"scenes.rename", MethodScenesRename},
		{"scenes.duplicate", MethodScenesDuplicate},
		{"scenes.duplicateToCanvas", MethodScenesDuplicateToCanvas},
		{"scenes.reorder", MethodScenesReorder},
		{"collections.list", MethodCollectionsList},
		{"collections.create", MethodCollectionsCreate},
		{"collections.rename", MethodCollectionsRename},
		{"collections.duplicate", MethodCollectionsDuplicate},
		{"collections.switch", MethodCollectionsSwitch},
		{"collections.remove", MethodCollectionsRemove},
		{"importer.scan", MethodImporterScan},
		{"importer.import", MethodImporterImport},
		{"sceneItems.list", MethodSceneItemsList},
		{"sceneItems.setVisible", MethodSceneItemsSetVisible},
		{"sceneItems.setLocked", MethodSceneItemsSetLocked},
		{"sceneItems.remove", MethodSceneItemsRemove},
		{"sceneItems.reorder", MethodSceneItemsReorder},
		{"sceneItems.getTransform", MethodSceneItemsGetTransform},
		{"sceneItems.setTransform", MethodSceneItemsSetTransform},
		{"sceneItems.transformAction", MethodSceneItemsTransformAction},
		{"sceneItems.setScaleFilter", MethodSceneItemsSetScaleFilter},
		{"sceneItems.setShowTransition", MethodSceneItemsSetShowTransition},
		{"sceneItems.setHideTransition", MethodSceneItemsSetHideTransition},
		{"sceneItems.setBlendingMode", MethodSceneItemsSetBlendingMode},
		{"sceneItems.setBlendingMethod", MethodSceneItemsSetBlendingMethod},
		{"sceneItems.setColor", MethodSceneItemsSetColor},
		{"sceneItems.group", MethodSceneItemsGroup},
		{"sceneItems.createGroup", MethodSceneItemsCreateGroup},
		{"sceneItems.ungroup", MethodSceneItemsUngroup},
		{"screenshot.takeProgram", MethodScreenshotTakeProgram},
		{"screenshot.takeSource", MethodScreenshotTakeSource},
		{"sourceTypes.list", MethodSourceTypesList},
		{"sources.create", MethodSourcesCreate},
		{"sources.listExisting", MethodSourcesListExisting},
		{"sources.addExisting", MethodSourcesAddExisting},
		{"sources.thumbnail", MethodSourcesThumbnail},
		{"sources.duplicate", MethodSourcesDuplicate},
		{"sources.duplicateInto", MethodSourcesDuplicateInto},
		{"sources.rename", MethodSourcesRename},
		{"sources.renameByName", MethodSourcesRenameByName},
		{"sources.findMissing", MethodSourcesFindMissing},
		{"sources.relinkMissing", MethodSourcesRelinkMissing},
		{"sources.getDeinterlace", MethodSourcesGetDeinterlace},
		{"sources.setDeinterlace", MethodSourcesSetDeinterlace},
		{"properties.get", MethodPropertiesGet},
		{"properties.set", MethodPropertiesSet},
		{"properties.defaults", MethodPropertiesDefaults},
		{"properties.button", MethodPropertiesButton},
		{"dialog.openFile", MethodDialogOpenFile},
		{"shell.revealPath", MethodShellRevealPath},
		{"file.readDataUri", MethodFileReadDataUri},
		{"filterTypes.list", MethodFilterTypesList},
		{"filters.list", MethodFiltersList},
		{"filters.add", MethodFiltersAdd},
		{"filters.remove", MethodFiltersRemove},
		{"filters.setEnabled", MethodFiltersSetEnabled},
		{"filters.reorder", MethodFiltersReorder},
		{"filters.rename", MethodFiltersRename},
		{"filters.duplicate", MethodFiltersDuplicate},
		{"filters.copyChain", MethodFiltersCopyChain},
		{"filters.pasteChain", MethodFiltersPasteChain},
		{"settings.getVideo", MethodSettingsGetVideo},
		{"settings.setVideo", MethodSettingsSetVideo},
		{"settings.getAudio", MethodSettingsGetAudio},
		{"settings.setAudio", MethodSettingsSetAudio},
		{"settings.getGeneral", MethodSettingsGetGeneral},
		{"settings.setGeneral", MethodSettingsSetGeneral},
		{"settings.getAdvanced", MethodSettingsGetAdvanced},
		{"settings.setAdvanced", MethodSettingsSetAdvanced},
		{"settings.snapshot", MethodSettingsSnapshot},
		{"settings.restore", MethodSettingsRestore},
		{"canvas.list", MethodCanvasList},
		{"canvas.create", MethodCanvasCreate},
		{"canvas.update", MethodCanvasUpdate},
		{"canvas.remove", MethodCanvasRemove},
		{"canvas.reorder", MethodCanvasReorder},
		{"encoderTypes.list", MethodEncoderTypesList},
		{"streamProfile.list", MethodStreamProfileList},
		{"streamProfile.create", MethodStreamProfileCreate},
		{"streamProfile.update", MethodStreamProfileUpdate},
		{"streamProfile.remove", MethodStreamProfileRemove},
		{"streamProfile.setPrimary", MethodStreamProfileSetPrimary},
		{"streamProfile.reorder", MethodStreamProfileReorder},
		{"serviceTypes.list", MethodServiceTypesList},
		{"outputBinding.list", MethodOutputBindingList},
		{"outputBinding.create", MethodOutputBindingCreate},
		{"outputBinding.update", MethodOutputBindingUpdate},
		{"outputBinding.setEnabled", MethodOutputBindingSetEnabled},
		{"outputBinding.remove", MethodOutputBindingRemove},
		{"sceneLink.list", MethodSceneLinkList},
		{"sceneLink.set", MethodSceneLinkSet},
		{"sceneLink.clear", MethodSceneLinkClear},
		{"multistream.status", MethodMultistreamStatus},
		{"transports.health", MethodTransportsHealth},
		{"multistream.startOutput", MethodMultistreamStartOutput},
		{"multistream.stopOutput", MethodMultistreamStopOutput},
		{"virtualCam.start", MethodVirtualCamStart},
		{"virtualCam.stop", MethodVirtualCamStop},
		{"virtualCam.status", MethodVirtualCamStatus},
		{"virtualCam.getConfig", MethodVirtualCamGetConfig},
		{"virtualCam.setConfig", MethodVirtualCamSetConfig},
		{"undo.undo", MethodUndoUndo},
		{"undo.redo", MethodUndoRedo},
		{"undo.state", MethodUndoState},
		{"stats.get", MethodStatsGet},
		{"stats.reset", MethodStatsReset},
		{"audio.list", MethodAudioList},
		{"audio.setDeflection", MethodAudioSetDeflection},
		{"audio.setMuted", MethodAudioSetMuted},
		{"audio.setHidden", MethodAudioSetHidden},
		{"audio.unhideAll", MethodAudioUnhideAll},
		{"audio.setVolumeLocked", MethodAudioSetVolumeLocked},
		{"audio.setPinned", MethodAudioSetPinned},
		{"audio.getAdvanced", MethodAudioGetAdvanced},
		{"audio.setAdvanced", MethodAudioSetAdvanced},
		{"audio.listMonitorDevices", MethodAudioListMonitorDevices},
		{"audio.listDevices", MethodAudioListDevices},
		{"audio.getGlobalDevices", MethodAudioGetGlobalDevices},
		{"audio.setGlobalDevice", MethodAudioSetGlobalDevice},
		{"transitionTypes.list", MethodTransitionTypesList},
		{"transitions.getCurrent", MethodTransitionsGetCurrent},
		{"transitions.setCurrent", MethodTransitionsSetCurrent},
		{"transitions.setDuration", MethodTransitionsSetDuration},
		{"theme.save", MethodThemeSave},
		{"theme.load", MethodThemeLoad},
		{"layout.save", MethodLayoutSave},
		{"layout.load", MethodLayoutLoad},
		{"window.detach", MethodWindowDetach},
		{"window.redock", MethodWindowRedock},
		{"window.list", MethodWindowList},
		{"window.toggleFullscreen", MethodWindowToggleFullscreen},
		{"window.minimize", MethodWindowMinimize},
		{"window.toggleMaximize", MethodWindowToggleMaximize},
		{"window.close", MethodWindowClose},
		{"display.listMonitors", MethodDisplayListMonitors},
		{"projector.open", MethodProjectorOpen},
		{"projector.close", MethodProjectorClose},
		{"projector.list", MethodProjectorList},
		{"sources.interact", MethodSourcesInteract},
		{"sources.closeInteract", MethodSourcesCloseInteract},
		{"hotkeys.list", Hotkeys::MethodList},
		{"hotkeys.set", Hotkeys::MethodSet},
		{"hotkeys.clear", Hotkeys::MethodClear},
		{"mcp.getConfig", MethodMcpGetConfig},
		{"mcp.setConfig", MethodMcpSetConfig},
		{"mcp.regenerateToken", MethodMcpRegenerateToken},
		{"browserDocks.list", MethodBrowserDocksList},
		{"browserDocks.set", MethodBrowserDocksSet},
		{"log.getCurrent", MethodLogGetCurrent},
		{"diagnostics.get", MethodDiagnosticsGet},
		{"diagnostics.setDebug", MethodDiagnosticsSetDebug},
		{"diagnostics.openLogFolder", MethodDiagnosticsOpenLogFolder},
		{"oauth.providers", MethodOAuthProviders},
		{"oauth.connect", MethodOAuthConnect},
		{"oauth.cancelConnect", MethodOAuthCancelConnect},
		{"oauth.disconnect", MethodOAuthDisconnect},
		{"oauth.accounts", MethodOAuthAccounts},
		{"oauth.linkAccount", MethodOAuthLinkAccount},
		{"oauth.status", MethodOAuthStatus},
		{"chat.state", MethodChatState},
		{"streamMeta.getSaved", MethodStreamMetaGetSaved},
		{"streamMeta.save", MethodStreamMetaSave},
		{"events.list", MethodEventsList},
		{"events.clear", MethodEventsClear},
		{"overlays.list", MethodOverlaysList},
		{"overlays.get", MethodOverlaysGet},
		{"overlays.create", MethodOverlaysCreate},
		{"overlays.update", MethodOverlaysUpdate},
		{"overlays.duplicate", MethodOverlaysDuplicate},
		{"overlays.delete", MethodOverlaysDelete},
		{"overlays.url", MethodOverlaysUrl},
		{"overlays.serverInfo", MethodOverlaysServerInfo},
		{"overlays.addToScene", MethodOverlaysAddToScene},
	};

	// Methods whose bodies block go on the async lane (libcurl platform calls, the
	// overlay server's socket sends): they run off-thread and resolve the CEF
	// callback later (same JS contract). Each body is the existing MethodFn, driven
	// off-thread by RunAsyncMethod.
	g_asyncMethods = {
		{"streamMeta.get",
		 [](const json &p, CefRefPtr<CefMessageRouterBrowserSide::Callback> cb) {
			 RunAsyncMethod("streamMeta.get", p, cb, MethodStreamMetaGet);
		 }},
		{"streamMeta.searchCategories",
		 [](const json &p, CefRefPtr<CefMessageRouterBrowserSide::Callback> cb) {
			 RunAsyncMethod("streamMeta.searchCategories", p, cb, MethodStreamMetaSearchCategories);
		 }},
		{"streamMeta.set",
		 [](const json &p, CefRefPtr<CefMessageRouterBrowserSide::Callback> cb) {
			 RunAsyncMethod("streamMeta.set", p, cb, MethodStreamMetaSet);
		 }},
		{"chat.send",
		 [](const json &p, CefRefPtr<CefMessageRouterBrowserSide::Callback> cb) {
			 RunAsyncMethod("chat.send", p, cb, MethodChatSend);
		 }},
		{"overlays.uploadAsset",
		 [](const json &p, CefRefPtr<CefMessageRouterBrowserSide::Callback> cb) {
			 RunAsyncMethod("overlays.uploadAsset", p, cb, MethodOverlaysUploadAsset);
		 }},
		{"overlays.test",
		 [](const json &p, CefRefPtr<CefMessageRouterBrowserSide::Callback> cb) {
			 RunAsyncMethod("overlays.test", p, cb, MethodOverlaysTest);
		 }},
	};

	// Notify JS whenever the undo stack changes (add/undo/redo/clear) so the UI's
	// undo/redo affordance re-reads canUndo/canRedo + names.
	ObsBootstrap::Undo().onChanged = [] {
		EmitUndoChanged();
	};

	// Phase 9.0: libcurl global init + a one-time WebSocket-support feature check,
	// logged at startup so a dep bundle built without WebSockets fails loudly before
	// any chat transport tries to connect.
	Chat::WsClient::EnsureInit();

	// R14/G1: route every transport health transition (chat/events/overlay) to the
	// transports.healthChanged push. Mirrors the engine's onStatusChanged wiring; the
	// aggregator is a permanent singleton, so this is set once and cleared in Shutdown.
	Transports::Health().onChanged = EmitTransportsHealthChanged;

	HostLog("[bridge] init: " + std::to_string(g_methods.size()) + " methods, " +
		std::to_string(g_asyncMethods.size()) + " async methods");
}

void Shutdown()
{
	// Signal every detached worker to stop BEFORE anything it might touch is torn
	// down. The shutdown flag makes an OAuth connect worker bail before it can
	// resurrect a stopped hub; clearing g_oauthRunning drops the ctx.canceled() latch
	// so any device-code poll unwinds; the alive-guard no-ops late PostToUi marshals
	// (the CEF loop has already returned by the time Stop() reaches here).
	g_bridgeShutdown.store(true, std::memory_order_release);
	g_oauthRunning.store(false, std::memory_order_release);
	AsyncTask::SetAlive(false);

	// Signal the always-on hub/poller loops to stop BEFORE the drain. They spawn on the
	// shared RunAsync worker counter (chat hub, event hub, viewer poller) and only ever
	// unwind when their generation's cancel flag is set here, so draining first would
	// always time out with a connected account (the loops never exit on their own). Each
	// Stop/StopAll is signal-only (set the cancel flag + disconnect the transport, no
	// thread join), so it just lets the drain observe the loops exiting.
	Chat::Hub().Stop();
	Chat::Viewers().Stop();
	Chat::Channels().Stop();
	Events::Hub().StopAll();

	// Now give the signaled workers a bounded window to actually unwind before the
	// hubs/statics they may still be mid-call on are torn down below. With the loops
	// already signaled this returns as soon as each transport's connect()/sleep observes
	// the cancel; a clean quit with no in-flight worker returns immediately.
	if (!AsyncTask::WaitForDrain(std::chrono::seconds(5))) {
		HostLog("[bridge] shutdown: async workers still running after drain timeout");
	}

	// Repeat the idempotent stops to catch a worker an OAuth connect worker may have
	// resurrected between the first stop and its g_bridgeShutdown probe (a late account
	// re-Start slipping past the flag). Signal-only again; any resurrected worker is
	// detached and unwinds on its own after this returns.
	Chat::Hub().Stop();
	Chat::Viewers().Stop();
	Chat::Channels().Stop();
	Events::Hub().StopAll();
	// Persist any debounced trailing event now that the workers are stopped and no
	// further Add can race the write (the store coalesces writes; this is the flush).
	Events::Store().Flush();
	// Phase 9.3: stop the overlay loopback server (closes every SSE socket + joins its
	// threads) after the event transports are down, so no in-flight Broadcast races the
	// teardown, and before CEF shutdown so no dangling send hits a torn-down host.
	Overlay::Server().Stop();
	// The overlay server is down; mark it so, then stop routing health transitions and
	// drop every row. The hub Stops above already reported their transports Disconnected
	// (onChanged was still live, so those pushes reached the UI); nulling onChanged now
	// keeps a late detached-worker report from emitting through the tearing-down bridge,
	// and Clear() leaves the aggregator empty for the next session.
	Transports::Health().Report(Transports::kOverlayTransportId, Transports::TransportHealth::State::Disconnected);
	Transports::Health().onChanged = nullptr;
	Transports::Health().Clear();
	// Drop the undo->event hook so the g_undo.Clear() later in Stop() (after the CEF
	// loop has returned) doesn't try to emit through a torn-down bridge.
	ObsBootstrap::Undo().onChanged = nullptr;
	g_methods.clear();
	g_asyncMethods.clear();
	if (g_cpuInfo) {
		os_cpu_usage_info_destroy(g_cpuInfo);
		g_cpuInfo = nullptr;
	}
	g_bitrateCache.clear();
	g_outputStatsBaseline.clear();
	g_statsBaseline = StatsBaseline{};
}

void AddBrowser(CefRefPtr<CefBrowser> browser)
{
	std::lock_guard<std::mutex> lock(g_browser_mutex);
	for (const auto &b : g_browsers) {
		if (b && b->IsSame(browser)) {
			return; // already registered
		}
	}
	g_browsers.push_back(browser);
}

void RemoveBrowser(CefRefPtr<CefBrowser> browser)
{
	std::lock_guard<std::mutex> lock(g_browser_mutex);
	for (auto it = g_browsers.begin(); it != g_browsers.end(); ++it) {
		if (*it && (*it)->IsSame(browser)) {
			g_browsers.erase(it);
			return;
		}
	}
}

bool Dispatch(const std::string &method, const json &params, json &result, std::string &error)
{
	auto it = g_methods.find(method);
	if (it == g_methods.end()) {
		error = "unknown method: " + method;
		return false;
	}
	DBG(LogCat::Bridge, "dispatch %s", method.c_str());
	// Exception barrier for the sync lane (mirrors RunAsyncMethod on the async lane):
	// a handler that throws must become a clean Failure, never escape into the CEF
	// query dispatch and terminate the process.
	try {
		const bool ok = it->second(params, result, error);
		if (!ok) {
			DBG(LogCat::Bridge, "dispatch %s failed: %s", method.c_str(), error.c_str());
		}
		return ok;
	} catch (const std::exception &e) {
		error = "unhandled exception in " + method + ": " + e.what();
		return false;
	} catch (...) {
		error = "unhandled exception in " + method;
		return false;
	}
}

bool DispatchAsync(const std::string &method, const json &params,
		   CefRefPtr<CefMessageRouterBrowserSide::Callback> callback)
{
	auto it = g_asyncMethods.find(method);
	if (it == g_asyncMethods.end()) {
		return false;
	}
	DBG(LogCat::Bridge, "dispatch %s (async)", method.c_str());
	it->second(params, callback);
	return true;
}

void EmitEvent(const std::string &name, const json &payload)
{
	std::string payloadDump;
	// A malformed payload (e.g. invalid UTF-8) makes dump() throw; drop the event
	// rather than let it escape -- EmitEvent runs from off-thread-posted CEF tasks
	// where an escaped exception would terminate the process.
	try {
		payloadDump = payload.dump();
	} catch (const std::exception &e) {
		HostLog("[bridge] EmitEvent(" + name + ") payload dump failed: " + e.what());
		return;
	}
	if (CefCurrentlyOn(TID_UI)) {
		DoEmit(name, payloadDump);
		return;
	}
	CefPostTask(TID_UI, base::BindOnce(&DoEmit, name, payloadDump));
}

bool ApplyDefaultCanvasVideo(const CanvasDefinition &desired, std::string &error)
{
	// Resolve the scale + color tokens to libobs enums via ToVideoInfo (single source
	// of truth for the token->enum mapping), then drive the global pipeline reset with
	// only the color fields overridden. The resolution/fps are passed explicitly.
	obs_scale_type st = OBS_SCALE_BICUBIC;
	ScaleFilterFromToken(desired.scaleType, st); // validated by the bridge parse
	obs_video_info tovi = {};
	desired.ToVideoInfo(tovi);
	return ApplyGlobalVideo(desired.width, desired.height, desired.outputWidth, desired.outputHeight,
				desired.fpsNum, desired.fpsDen, st, error, &tovi.output_format, &tovi.colorspace,
				&tovi.range);
}

void EmitMultistreamChanged()
{
	// BuildStatusArray -> Statuses() reads the canvas/profile/binding stores, which
	// are mutated only on the UI thread. This is fired from the off-thread output
	// start/stop signal handlers too, so defer the build to TID_UI; otherwise an
	// output dropping mid-edit would race a concurrent Outputs-tab store mutation
	// (vector reallocation under the read -> torn read / iterator invalidation).
	if (!CefCurrentlyOn(TID_UI)) {
		CefPostTask(TID_UI, base::BindOnce(&EmitMultistreamChanged));
		return;
	}
	// An async output-stop signal can post this task in the window between Stop()
	// resetting g_multistream and CefShutdown draining the CEF loop; bail before
	// BuildStatusArray dereferences the gone engine (mirrors AudioMonitorAlive).
	if (!ObsBootstrap::MultistreamAlive()) {
		return;
	}
	EmitEvent(EventNames::kMultistreamChanged, json{{"outputs", BuildStatusArray()}});
}

void EmitTransportsHealthChanged()
{
	// Wired as Transports::Health().onChanged and fired from chat/events transport
	// worker threads. The aggregator is a permanent singleton (its Snapshot() is always
	// valid), but marshal to TID_UI so the emit + snapshot read never races a concurrent
	// UI-thread report and so EmitEvent broadcasts from the CEF UI thread it requires.
	if (!CefCurrentlyOn(TID_UI)) {
		CefPostTask(TID_UI, base::BindOnce(&EmitTransportsHealthChanged));
		return;
	}
	EmitEvent(EventNames::kTransportsHealthChanged, json{{"transports", BuildTransportHealthArray()}});
}

void EmitVirtualCamChanged()
{
	// Fired from the off-thread virtual-camera output start/stop signal handlers;
	// defer to TID_UI so reading the manager state + emitting never races a
	// concurrent UI-thread setConfig (mirrors EmitMultistreamChanged).
	if (!CefCurrentlyOn(TID_UI)) {
		CefPostTask(TID_UI, base::BindOnce(&EmitVirtualCamChanged));
		return;
	}
	EmitEvent(EventNames::kVirtualCamChanged, VirtualCamStatusJson());
}

void EmitAudioLevels()
{
	// Coalesce the audio-thread fires into a single ~30 Hz emit. The throttle +
	// the snapshot/build both run on TID_UI: building the payload off-thread would
	// race a concurrent UI-thread store/monitor mutation (the 4.4.4 discipline).
	constexpr uint64_t kMinIntervalNs = 33'000'000; // ~30 Hz
	static uint64_t lastEmitNs = 0;

	if (!CefCurrentlyOn(TID_UI)) {
		CefPostTask(TID_UI, base::BindOnce(&EmitAudioLevels));
		return;
	}

	// Now on TID_UI. An audio-thread volmeter fire can post this task in the window
	// between the pre-Stop CEF drain and AudioMonitor teardown; CefShutdown then
	// drains it after Stop() reset the monitor. Bail before touching (and re-posting
	// to) a gone monitor. g_audioMonitor is only mutated on TID_UI (Start/Stop run
	// on this thread), so reading it here never races.
	if (!ObsBootstrap::AudioMonitorAlive()) {
		return;
	}

	const uint64_t now = os_gettime_ns();
	const uint64_t elapsed = now - lastEmitNs;
	if (lastEmitNs != 0 && elapsed < kMinIntervalNs) {
		// Too soon: re-post for the remaining interval rather than emit now. The
		// monitor's emitPending stays armed (we do NOT snapshot here), so this is
		// still a single in-flight task -- it just defers the drain.
		const int64_t delayMs = int64_t((kMinIntervalNs - elapsed) / 1'000'000) + 1;
		CefPostDelayedTask(TID_UI, base::BindOnce(&EmitAudioLevels), delayMs);
		return;
	}

	// Drain the coalesced levels (clears emitPending so the next callback re-arms)
	// and build the dB payload here on the UI thread.
	std::map<std::string, AudioMonitor::Levels> snapshot = ObsBootstrap::AudioMonitor().SnapshotLevels();
	json arr = json::array();
	for (const auto &[uuid, level] : snapshot) {
		arr.push_back(json{{"uuid", uuid}, {"magnitude", level.magnitude}, {"peak", level.peak}});
	}
	lastEmitNs = now;
	EmitEvent(EventNames::kAudioLevels, json{{"levels", std::move(arr)}});
}

void EmitAudioChanged()
{
	EmitEvent(EventNames::kAudioChanged, json::object());
}

} // namespace Bridge

bool ObsQueryHandler::OnQuery(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> /*frame*/, int64_t /*query_id*/,
			      const CefString &request, bool /*persistent*/, CefRefPtr<Callback> callback)
{
	using Bridge::json;

	json envelope;
	try {
		envelope = json::parse(request.ToString());
	} catch (const std::exception &e) {
		callback->Failure(400, std::string("invalid request envelope: ") + e.what());
		return true;
	}

	if (!envelope.is_object() || !envelope.contains("method") || !envelope["method"].is_string()) {
		callback->Failure(400, "envelope missing string 'method'");
		return true;
	}

	const std::string method = envelope["method"].get<std::string>();
	const json params = envelope.contains("params") ? envelope["params"] : json(nullptr);

	// Async lane first (Phase 8b): if the method blocks on the network it runs
	// off-thread and resolves `callback` later, on the UI thread. We hand off the
	// ref-counted callback and return without touching the sync path, so there is
	// exactly one resolution per query.
	// The async lane spawns a worker thread; if that spawn throws (thread/handle
	// exhaustion) it must become a clean Failure, not escape OnQuery and terminate
	// the process with the callback left hung.
	try {
		if (Bridge::DispatchAsync(method, params, callback)) {
			HostLog("[bridge] " + method + " -> async dispatched");
			return true;
		}
	} catch (const std::exception &e) {
		const std::string msg = "async dispatch failed for " + method + ": " + e.what();
		callback->Failure(500, msg);
		HostLog("[bridge] " + msg);
		return true;
	} catch (...) {
		const std::string msg = "async dispatch failed for " + method;
		callback->Failure(500, msg);
		HostLog("[bridge] " + msg);
		return true;
	}

	json result;
	std::string error;
	// Wrap dispatch + result.dump(): Dispatch already traps handler throws, but the
	// dump can still throw (e.g. invalid UTF-8 in the result), which must not escape
	// into CEF. Convert to the same Failure shape the async lane produces.
	try {
		if (Bridge::Dispatch(method, params, result, error)) {
			callback->Success(result.dump());
			HostLog("[bridge] " + method + " -> ok");
		} else {
			callback->Failure(404, error);
			HostLog("[bridge] " + method + " -> FAIL (" + error + ")");
		}
	} catch (const std::exception &e) {
		const std::string msg = "unhandled exception in " + method + ": " + e.what();
		callback->Failure(500, msg);
		HostLog("[bridge] " + msg);
	} catch (...) {
		const std::string msg = "unhandled exception in " + method;
		callback->Failure(500, msg);
		HostLog("[bridge] " + msg);
	}
	return true;
}
