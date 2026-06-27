#include "bridge.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <shobjidl.h>
#include <shlwapi.h>

#include <obs.h>
#include <obs.hpp>
#include <obs-frontend-api.h>

#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#include "audio/AudioMonitor.hpp"
#include "log.hpp"
#include "mcp/McpServer.hpp"
#include "obs_bootstrap.hpp"
#include "preview_window.hpp"
#include "projector_window.hpp"
#include "properties_serializer.hpp"
#include "scene_collections.hpp"
#include "scene_persistence.hpp"
#include "transitions.hpp"
#include "window_manager.hpp"
#include "UndoManager.hpp"

#include "multistream/CanvasRuntime.hpp"
#include "multistream/CanvasStore.hpp"
#include "multistream/Hotkeys.hpp"
#include "multistream/MultistreamEngine.hpp"
#include "multistream/OutputBindingStore.hpp"
#include "multistream/SceneLinkStore.hpp"
#include "multistream/StorePaths.hpp"
#include "multistream/StreamProfileStore.hpp"
#include <util/dstr.h>
#include <util/platform.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/matrix4.h>

namespace Bridge {

namespace {

// One method: (params) -> (result | error). Returns false and fills `error` on
// failure. Runs on the browser UI thread.
using MethodFn = std::function<bool(const json &params, json &result, std::string &error)>;

std::unordered_map<std::string, MethodFn> g_methods;

// All live UI browsers; EmitEvent broadcasts to each. Registered/unregistered on
// the UI thread; read on the UI thread (EmitEvent posts there first). The mutex
// guards the registry against the (off-thread) registration paths and snapshots
// it before iterating so an emit never holds the lock across ExecuteJavaScript.
std::mutex g_browser_mutex;
std::vector<CefRefPtr<CefBrowser>> g_browsers;

// obs frontend event enum -> stable string name forwarded to JS. Data-driven so
// adding a forwarded event is one row, not a switch arm.
struct EventName {
	obs_frontend_event event;
	const char *name;
};
const EventName kForwardedEvents[] = {
	{OBS_FRONTEND_EVENT_FINISHED_LOADING, "OBS_FRONTEND_EVENT_FINISHED_LOADING"},
	{OBS_FRONTEND_EVENT_SCENE_CHANGED, "OBS_FRONTEND_EVENT_SCENE_CHANGED"},
	{OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED, "OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED"},
};

const char *ForwardedEventName(obs_frontend_event event)
{
	for (const auto &e : kForwardedEvents) {
		if (e.event == event) {
			return e.name;
		}
	}
	return nullptr;
}

// obs->shim->bridge->JS: forward whitelisted obs frontend events to JS as
// "obs.event" with {event:<enum name>}. Proves the full chain the real UI uses.
void OnFrontendEvent(enum obs_frontend_event event, void * /*data*/)
{
	const char *name = ForwardedEventName(event);
	if (!name) {
		return;
	}
	EmitEvent("obs.event", json{{"event", name}});
}

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
	EmitEvent("undo.changed", UndoStateJson());
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
	result = json{{"active", ObsBootstrap::Multistream().AnyLive()}};
	EmitEvent("streaming.changed", result);
	return true;
}

bool MethodStreamingStop(const json & /*params*/, json &result, std::string & /*error*/)
{
	ObsBootstrap::Multistream().StopAll();
	result = json{{"active", false}};
	EmitEvent("streaming.changed", result);
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
	{SPEAKERS_MONO, "mono"},     {SPEAKERS_STEREO, "stereo"}, {SPEAKERS_2POINT1, "2.1"},
	{SPEAKERS_4POINT0, "4.0"},   {SPEAKERS_4POINT1, "4.1"},   {SPEAKERS_5POINT1, "5.1"},
	{SPEAKERS_7POINT1, "7.1"},
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
		{"baseWidth", ovi.base_width},   {"baseHeight", ovi.base_height},
		{"outputWidth", ovi.output_width}, {"outputHeight", ovi.output_height},
		{"fpsNum", ovi.fps_num},         {"fpsDen", ovi.fps_den},
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
			     uint32_t fpsDen, std::string &error)
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

	// All non-resolution fields are copied from the current config, so if these six
	// already match there is genuinely nothing to reset. Skip the full pipeline
	// reset (and its preview flicker) -- this also makes the redundant double-apply
	// on a Settings Cancel a no-op.
	if (ovi.base_width == previous.base_width && ovi.base_height == previous.base_height &&
	    ovi.output_width == previous.output_width && ovi.output_height == previous.output_height &&
	    ovi.fps_num == previous.fps_num && ovi.fps_den == previous.fps_den) {
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
	if (!params.is_object()) {
		error = "setVideo expects an object";
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
			      ovi.fps_den, error)) {
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
			EmitEvent("canvas.changed", json::object());
		}
	}

	result = VideoInfoToJson(applied);
	EmitEvent("settings.videoChanged", result);
	HostLog("[bridge] settings.setVideo -> " + std::to_string(applied.base_width) + "x" +
		std::to_string(applied.base_height) + " out " + std::to_string(applied.output_width) + "x" +
		std::to_string(applied.output_height) + " @ " + std::to_string(applied.fps_num) + "/" +
		std::to_string(applied.fps_den));
	return true;
}

bool MethodSettingsGetAudio(const json & /*params*/, json &result, std::string &error)
{
	obs_audio_info oai = {};
	if (!obs_get_audio_info(&oai)) {
		error = "audio not initialized";
		return false;
	}
	result = json{{"sampleRate", oai.samples_per_sec}, {"speakers", SpeakerLayoutName(oai.speakers)}};
	return true;
}

bool MethodSettingsSetAudio(const json &params, json &result, std::string &error)
{
	if (!params.is_object()) {
		error = "setAudio expects an object";
		return false;
	}
	if (AnyOutputActive()) {
		error = "cannot change audio while an output is active";
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

	obs_audio_info applied = {};
	obs_get_audio_info(&applied);
	result = json{{"sampleRate", applied.samples_per_sec}, {"speakers", SpeakerLayoutName(applied.speakers)}};
	EmitEvent("settings.audioChanged", result);
	HostLog("[bridge] settings.setAudio -> " + std::to_string(applied.samples_per_sec) + "Hz " +
		SpeakerLayoutName(applied.speakers));
	return true;
}

// --- scenes / scene-items helpers -------------------------------------------

// Read an optional string field from params; returns "" when absent/not a
// string. Treats an empty string the same as absent for scene resolution.
std::string OptString(const json &params, const char *key)
{
	if (!params.is_object()) {
		return std::string();
	}
	auto it = params.find(key);
	return (it != params.end() && it->is_string()) ? it->get<std::string>() : std::string();
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
	std::string uuid;     // the additional canvas's uuid; empty => global path
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
	EmitEvent("scenes.changed", json{{"canvas", canvasUuid.empty() ? json(nullptr) : json(canvasUuid)}});
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
	// Enumerate scene sources in creation order. `current` flags the scene bound
	// to output channel 0 (unwrapped from the program transition).
	obs_source_t *current = Transitions::GetProgramScene(); // addref'd; may be null
	const char *currentName = current ? obs_source_get_name(current) : nullptr;
	const std::string currentStr = currentName ? currentName : std::string();

	json scenes = json::array();
	struct Ctx {
		json *arr;
		const std::string *current;
	} ctx{&scenes, &currentStr};

	obs_enum_scenes(
		[](void *param, obs_source_t *source) -> bool {
			auto *c = static_cast<Ctx *>(param);
			const char *name = obs_source_get_name(source);
			if (name) {
				c->arr->push_back(json{{"name", name}, {"current", !c->current->empty() &&
											      *c->current == name}});
			}
			return true; // keep enumerating
		},
		&ctx);

	if (current) {
		obs_source_release(current);
	}
	result = std::move(scenes);
	return true;
}

bool MethodScenesCreate(const json &params, json &result, std::string &error)
{
	const std::string name = OptString(params, "name");
	if (name.empty()) {
		error = "scenes.create requires a non-empty 'name'";
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
			std::string canvasSceneName;
			for (const CanvasRuntime::SceneInfo &s : ObsBootstrap::CanvasRuntime().Scenes(canvasUuid)) {
				if (s.uuid == canvasSceneUuid) {
					canvasSceneName = s.name;
					break;
				}
			}
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
	ObsBootstrap::SceneLinks().Save();

	// Instant feedback: if the linked main scene is live now, switch the canvas.
	OBSSourceAutoRelease program = Transitions::GetProgramScene();
	if (program) {
		const char *pu = obs_source_get_uuid(program);
		if (pu && mainUuid == pu) {
			ObsBootstrap::ApplyCanvasSceneLinks(mainUuid);
		}
	}

	EmitEvent("sceneLink.changed", json::object());
	result = json::object();
	return true;
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
	if (it != link.map.end()) {
		it->second.erase(canvasUuid);
		if (it->second.empty()) {
			link.map.erase(it);
		}
		ObsBootstrap::SceneLinks().Save();
	}
	EmitEvent("sceneLink.changed", json::object());
	result = json::object();
	return true;
}

bool MethodScenesRemove(const json &params, json &result, std::string &error)
{
	const std::string name = OptString(params, "name");
	if (name.empty()) {
		error = "scenes.remove requires a non-empty 'name'";
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
	const std::string name = OptString(params, "name");
	if (name.empty()) {
		error = "scenes.setCurrent requires a non-empty 'name'";
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

	obs_source_t *source = obs_get_source_by_name(name.c_str()); // addref'd
	if (!source || !obs_scene_from_source(source)) {
		if (source) {
			obs_source_release(source);
		}
		error = "no scene named '" + name + "'";
		return false;
	}
	Transitions::SetProgramScene(source, true); // animate through the program transition
	const char *switchedUuid = obs_source_get_uuid(source);
	const std::string switchedUuidStr = switchedUuid ? switchedUuid : std::string();
	obs_source_release(source);

	ObsBootstrap::ApplyCanvasSceneLinks(switchedUuidStr);

	EmitScenesChanged(std::string());
	SceneCollection::Save();
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
	const std::string name = OptString(params, "name");
	if (name.empty()) {
		error = "scenes.duplicate requires a non-empty 'name'";
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

// scenes.reorder {name, direction:"up"|"down", canvas?}: reorder a scene within
// the (canvas or global) scene list.
//
// HONEST LIMITATION: libobs has no scene-ordering primitive. Unlike scene ITEMS
// (obs_sceneitem_set_order), global scenes are plain sources enumerated by
// obs_enum_scenes in CREATION order, and an additional canvas's scenes are
// enumerated by obs_canvas_enum_scenes -- neither exposes a settable order. The
// legacy Qt frontend's scene order is a UI-only concept: it persists a
// "scene_order" array inside each scene-collection JSON (SaveSceneListOrder reads
// the QListWidget row order). The new CEF frontend has NO scene-collection
// save/load layer yet (OutputBindingStore notes this), so there is nowhere to
// persist a user-defined order. Implementing a session-only order that
// scenes.list does not consult would be a no-op disguised as success. Per the
// plan ("do NOT fake it"), this returns a clear error until scene-collection
// persistence exists to back a real order.
bool MethodScenesReorder(const json &params, json & /*result*/, std::string &error)
{
	const std::string name = OptString(params, "name");
	const std::string direction = OptString(params, "direction");
	if (name.empty()) {
		error = "scenes.reorder requires a non-empty 'name'";
		return false;
	}
	if (direction != "up" && direction != "down") {
		error = "scenes.reorder 'direction' must be 'up' or 'down'";
		return false;
	}
	error = "scene reordering is not supported: libobs enumerates scenes in creation order and "
		"the new frontend has no scene-collection persistence to store a custom order";
	return false;
}

// --- scene items ------------------------------------------------------------

// Emit sceneItems.changed for the resolved scene. Passes the scene's actual name
// plus the addressed canvas uuid (empty for the global path) so a per-canvas panel
// filters the event to its own canvas before deciding whether the change applies.
void EmitSceneItemsChanged(obs_source_t *sceneSource, const std::string &canvasUuid)
{
	const char *name = sceneSource ? obs_source_get_name(sceneSource) : nullptr;
	EmitEvent("sceneItems.changed",
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

// Emit + persist after an Apply, matching what the original mutation does so the
// UI refreshes and the undo persists.
void CommitStateChange(const json &state, obs_source_t *sceneSource)
{
	const CanvasTarget target = ResolveCanvasTarget(state);
	EmitSceneItemsChanged(sceneSource, target.uuid);
	if (!target.isAdditional) {
		SceneCollection::Save();
	}
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

	CommitStateChange(state, sceneSource);
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
	CommitStateChange(state, sceneSource);
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
	CommitStateChange(state, sceneSource);
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
		CommitStateChange(state, sceneSource);
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
	CommitStateChange(state, sceneSource);
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
	CommitStateChange(state, sceneSource);
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
	CommitStateChange(state, sceneSource);
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

	CommitStateChange(state, sceneSource);
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
			// Prepend to invert bottom-first enumeration into top-first.
			arr->insert(arr->begin(), json{
							  {"id", obs_sceneitem_get_id(item)},
							  {"source", srcName ? json(srcName) : json(nullptr)},
							  {"visible", obs_sceneitem_visible(item)},
							  {"locked", obs_sceneitem_locked(item)},
							  {"scaleFilter",
							   ScaleFilterToToken(obs_sceneitem_get_scale_filter(item))},
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
	EmitSceneItemsChanged(sceneSource, ResolveCanvasTarget(params).uuid);
	obs_source_release(sceneSource);
	if (!ResolveCanvasTarget(params).isAdditional) {
		SceneCollection::Save();
	}
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
	EmitSceneItemsChanged(sceneSource, ResolveCanvasTarget(params).uuid);
	obs_source_release(sceneSource);
	if (!ResolveCanvasTarget(params).isAdditional) {
		SceneCollection::Save();
	}
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
	EmitSceneItemsChanged(sceneSource, ResolveCanvasTarget(params).uuid);
	obs_source_release(sceneSource);
	if (!ResolveCanvasTarget(params).isAdditional) {
		SceneCollection::Save();
	}
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
	if (!movement) {
		error = "reorder 'direction' must be one of up|down|top|bottom";
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
	obs_sceneitem_set_order(item, *movement);
	json after = CaptureOrderState(params, sceneSource);
	EmitSceneItemsChanged(sceneSource, ResolveCanvasTarget(params).uuid);
	obs_source_release(sceneSource);
	if (!ResolveCanvasTarget(params).isAdditional) {
		SceneCollection::Save();
	}
	RecordUndo("Reorder", ApplyOrder, before, after);
	result = json{{"id", id}, {"direction", direction}};
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
	EmitSceneItemsChanged(sceneSource, ResolveCanvasTarget(params).uuid);
	obs_source_release(sceneSource);
	if (!ResolveCanvasTarget(params).isAdditional) {
		SceneCollection::Save();
	}
	RecordUndo("Scale Filtering", ApplyScaleFilter, before, after);
	result = json{{"id", id}, {"filter", filter}};
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

// Resolve the scene + item shared by all three transform methods. On success
// `sceneSource` is addref'd (caller releases) and `item` is borrowed from it.
bool ResolveTransformTarget(const json &params, obs_source_t *&sceneSource, obs_sceneitem_t *&item,
			    std::string &error)
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

// Persist + notify after a transform mutation, matching the sibling item methods.
void CommitSceneItemChange(const json &params, obs_source_t *sceneSource)
{
	EmitSceneItemsChanged(sceneSource, ResolveCanvasTarget(params).uuid);
	if (!ResolveCanvasTarget(params).isAdditional) {
		SceneCollection::Save();
	}
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

	obs_sceneitem_defer_update_begin(item);
	obs_sceneitem_set_info2(item, &info);
	obs_sceneitem_set_crop(item, &crop);
	obs_sceneitem_defer_update_end(item);

	CommitSceneItemChange(params, sceneSource);

	RecordUndo(std::string("Transform ") + (undoSrcName ? undoSrcName : ""), ApplyTransform, undoBefore,
		   CaptureTransformState(params, item));

	uint32_t baseW = 0, baseH = 0;
	ResolveBaseSize(params, baseW, baseH);
	result = SceneItemTransformToJson(item, baseW, baseH);
	obs_source_release(sceneSource);
	return true;
}

// Center the item in the base canvas, replicating the old frontend's
// CenterSelectedSceneItems(CenterType::Scene): shift the item's axis-aligned box
// so its center lands on the canvas center (accounts for rotation/bounds).
void CenterItemInBase(obs_sceneitem_t *item, uint32_t baseWidth, uint32_t baseHeight)
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
	pos.x += offset.x;
	pos.y += offset.y;
	obs_sceneitem_set_pos(item, &pos);
}

bool MethodSceneItemsTransformAction(const json &params, json &result, std::string &error)
{
	const std::string action = OptString(params, "action");
	static const char *kActions[] = {"reset",  "center", "fitToScreen",
					 "stretchToScreen", "flipH",  "flipV"};
	bool known = false;
	for (const char *a : kActions) {
		if (action == a) {
			known = true;
			break;
		}
	}
	if (!known) {
		error = "transformAction 'action' must be one of "
			"reset|center|fitToScreen|stretchToScreen|flipH|flipV";
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
	ResolveBaseSize(params, baseW, baseH);

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
	} else { // center
		CenterItemInBase(item, baseW, baseH);
	}

	obs_sceneitem_defer_update_end(item);

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
			{"caps", json{
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
	const std::string type = OptString(params, "type");
	if (type.empty()) {
		error = "sources.create requires a non-empty 'type'";
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
	obs_source_release(sceneSource);

	if (!item) {
		error = "obs_scene_add failed";
		return false;
	}
	if (!ResolveCanvasTarget(params).isAdditional) {
		SceneCollection::Save();
	}
	ObsBootstrap::Undo().AddAction("Add " + name, kRemoveItemBySource, kAddItemFromSnapshot, before.dump(),
				       after.dump());
	result = json{{"id", itemId}, {"source", name}};
	return true;
}

// List existing input sources NOT already present in the target scene, so the
// UI can offer "Add existing". params: {scene?}.
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

	json names = json::array();
	struct Ctx {
		json *arr;
		std::unordered_map<std::string, bool> *inScene;
	} ctx{&names, &inScene};
	obs_enum_sources(
		[](void *param, obs_source_t *source) -> bool {
			auto *c = static_cast<Ctx *>(param);
			const char *n = obs_source_get_name(source);
			if (n && c->inScene->find(n) == c->inScene->end()) {
				c->arr->push_back(n);
			}
			return true;
		},
		&ctx);

	obs_source_release(sceneSource);
	result = std::move(names);
	return true;
}

// Add an already-existing source to a scene. params: {name, scene?}.
bool MethodSourcesAddExisting(const json &params, json &result, std::string &error)
{
	const std::string name = OptString(params, "name");
	if (name.empty()) {
		error = "sources.addExisting requires a non-empty 'name'";
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
	obs_source_release(sceneSource);

	if (!item) {
		error = "obs_scene_add failed";
		return false;
	}
	if (!ResolveCanvasTarget(params).isAdditional) {
		SceneCollection::Save();
	}
	ObsBootstrap::Undo().AddAction("Add " + name, kRemoveItemBySource, kAddItemFromSnapshot, before.dump(),
				       after.dump());
	result = json{{"id", itemId}, {"source", name}};
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
	const std::string name = OptString(params, "name");
	if (name.empty()) {
		error = "sources.rename requires a non-empty 'name'";
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
	EmitSceneItemsChanged(sceneSource, ResolveCanvasTarget(params).uuid);
	obs_source_release(sceneSource);
	if (!ResolveCanvasTarget(params).isAdditional) {
		SceneCollection::Save();
	}
	RecordUndo("Rename", ApplyRename, before, after);
	result = json{{"id", id}, {"source", name}};
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
	void *(*resolve)(const std::string &ref);                   // addref'd; null if not found
	obs_properties_t *(*get_props)(void *obj);                  // caller frees with obs_properties_destroy
	obs_data_t *(*get_settings)(void *obj);                     // addref'd; caller releases
	void (*update)(void *obj, obs_data_t *settings);            // apply settings
	void (*release)(void *obj);                                 // drop the resolve ref
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
	OBSServiceAutoRelease service;  // transient instance bound to the stored settings
	OBSDataAutoRelease settings;    // the StreamProfile's settings (same obs_data instance)
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
			EmitEvent("canvas.changed", json::object());
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
			EmitEvent("streamProfile.changed", json::object());
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
			patterns = entry.substr(paren + 1, close == std::string::npos ? std::string::npos : close - paren - 1);
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
	return true;
}

// Read+validate {kind, ref} -> resolved object + its kind. Caller releases the
// object via kind->release on success.
bool ResolvePropertyTarget(const json &params, const PropertyKind *&kind, void *&obj, std::string &error)
{
	const std::string kindName = OptString(params, "kind");
	const std::string ref = OptString(params, "ref");
	if (kindName.empty() || ref.empty()) {
		error = "properties requires non-empty 'kind' and 'ref'";
		return false;
	}
	kind = FindPropertyKind(kindName);
	if (!kind) {
		error = "unsupported properties kind: " + kindName;
		return false;
	}
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
			// Persist source-setting edits to the global collection (encoder/service
			// kinds persist their own stores in their update closures). Filters
			// serialize with their parent source, so persist them the same way. Skip
			// the additional-canvas path -- those are persisted per-canvas later.
			const std::string kindName = kind->name;
			if ((kindName == "source" || kindName == "filter") && !ResolveCanvasTarget(params).isAdditional) {
				SceneCollection::Save();
			}
		}
	}

	// Re-fetch so modified-callbacks' visibility/enabled/option changes reflect
	// in the returned schema + values. obs re-runs them during update + get.
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
	const std::string name = OptString(params, "source");
	if (name.empty()) {
		error = "filters requires a non-empty 'source'";
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

// --- canvases (native multistream encode targets, 4.4.1) --------------------

// Whether a canvas is currently encoding/streaming. Structural edits
// (resolution/fps/encoder) are refused while live, since the canvas's encoders
// are bound to its video mix and resetting it would free the mix out from under
// them (UAF).
bool CanvasIsLive(const std::string &uuid)
{
	return ObsBootstrap::Multistream().IsCanvasLive(uuid);
}

// Map one CanvasDefinition to the bridge's canvas shape. Resolution is a single
// value in the model (edit-surface == encode size), so base* and output* mirror
// it; the field pair is kept so the JS contract reads like the video settings.
json CanvasToJson(const CanvasDefinition &def)
{
	return json{
		{"uuid", def.uuid},
		{"name", def.name},
		{"isDefault", def.isDefault},
		{"baseWidth", def.width},
		{"baseHeight", def.height},
		{"outputWidth", def.width},
		{"outputHeight", def.height},
		{"fpsNum", def.fpsNum},
		{"fpsDen", def.fpsDen},
		{"videoEncoder", def.video.id},
		{"audioEncoder", def.audio.id},
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
	if (v <= 0 || v > 1000) {
		error = std::string("'") + key + "' must be in 1..1000";
		return false;
	}
	out = uint32_t(v);
	return true;
}

bool MethodCanvasCreate(const json &params, json &result, std::string &error)
{
	if (!params.is_object()) {
		error = "canvas.create expects an object";
		return false;
	}
	const std::string name = OptString(params, "name");
	if (name.empty()) {
		error = "canvas.create requires a non-empty 'name'";
		return false;
	}

	CanvasDefinition def;
	def.name = name;
	def.isDefault = false;
	// Resolution: base* drives the single stored width/height; output* is accepted
	// for contract symmetry but the model unifies them, so base* wins.
	if (!ReadCanvasDim(params, "baseWidth", 1920, def.width, error) ||
	    !ReadCanvasDim(params, "baseHeight", 1080, def.height, error) ||
	    !ReadCanvasFps(params, "fpsNum", 60, def.fpsNum, error) ||
	    !ReadCanvasFps(params, "fpsDen", 1, def.fpsDen, error)) {
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

	CanvasStore &store = ObsBootstrap::Canvases();
	const CanvasDefinition &added = store.Add(std::move(def));
	const std::string uuid = added.uuid;
	store.Save();

	// Bring up the live obs_canvas_t mix so the new canvas can encode immediately.
	ObsBootstrap::CanvasRuntime().EnsureCanvas(added);

	EmitEvent("canvas.changed", json::object());
	result = json{{"uuid", uuid}};
	return true;
}

bool MethodCanvasUpdate(const json &params, json &result, std::string &error)
{
	if (!params.is_object()) {
		error = "canvas.update expects an object";
		return false;
	}
	const std::string uuid = OptString(params, "uuid");
	if (uuid.empty()) {
		error = "canvas.update requires a non-empty 'uuid'";
		return false;
	}
	CanvasStore &store = ObsBootstrap::Canvases();
	CanvasDefinition *def = store.Find(uuid);
	if (!def) {
		error = "no canvas with uuid '" + uuid + "'";
		return false;
	}

	// Name change is always allowed. Structural fields (resolution/fps/encoder) are
	// refused while the canvas is live.
	const std::string name = OptString(params, "name");
	if (!name.empty()) {
		def->name = name;
	}

	// Resolve the requested structural values first so we can tell whether any
	// actually changed before deciding to refuse.
	uint32_t newW = def->width, newH = def->height, newFpsN = def->fpsNum, newFpsD = def->fpsDen;
	if (!ReadCanvasDim(params, "baseWidth", def->width, newW, error) ||
	    !ReadCanvasDim(params, "baseHeight", def->height, newH, error) ||
	    !ReadCanvasFps(params, "fpsNum", def->fpsNum, newFpsN, error) ||
	    !ReadCanvasFps(params, "fpsDen", def->fpsDen, newFpsD, error)) {
		return false;
	}
	const std::string venc = OptString(params, "videoEncoder");
	const std::string aenc = OptString(params, "audioEncoder");

	const bool resChanged = newW != def->width || newH != def->height || newFpsN != def->fpsNum ||
				newFpsD != def->fpsDen;
	const bool vencChanged = !venc.empty() && venc != def->video.id;
	const bool aencChanged = !aenc.empty() && aenc != def->audio.id;

	if ((resChanged || vencChanged || aencChanged) && CanvasIsLive(uuid)) {
		error = "cannot change resolution/fps/encoder while the canvas is live";
		return false;
	}

	// The Default canvas has no runtime mix -- a resolution/fps change drives the
	// global/main video pipeline instead. Apply it BEFORE committing any def fields
	// so a failed reset (rolled back inside ApplyGlobalVideo) leaves the def -- and
	// thus canvases.json -- untouched and consistent with the live pipeline.
	if (resChanged && def->isDefault) {
		if (!ApplyGlobalVideo(newW, newH, newW, newH, newFpsN, newFpsD, error)) {
			return false;
		}
	}

	def->width = newW;
	def->height = newH;
	def->fpsNum = newFpsN;
	def->fpsDen = newFpsD;
	// Switching an encoder id replaces its stored settings with that type's
	// defaults (the prior blob belongs to a different encoder schema).
	if (vencChanged) {
		def->video.id = venc;
		def->video.settings = obs_encoder_defaults(venc.c_str());
	}
	if (aencChanged) {
		def->audio.id = aenc;
		def->audio.settings = obs_encoder_defaults(aenc.c_str());
	}

	// A structural change invalidates the engine's cached encoder pair (bound to
	// the old resolution/id), so a later restart rebuilds it against the new mix.
	if (resChanged || vencChanged || aencChanged) {
		ObsBootstrap::Multistream().InvalidateCanvasEncoders(uuid);
	}
	// Resolution/fps changes resize the live mix; the guard above already refused
	// while the canvas is live, so this only runs on an idle canvas. (Encoder-id
	// changes don't touch the mix.) The Default canvas was already handled above
	// (it drives global video, not a runtime mix); ResetVideo reads *def so it must
	// run after the commit.
	if (resChanged && !def->isDefault) {
		ObsBootstrap::CanvasRuntime().ResetVideo(*def);
	}

	store.Save();
	EmitEvent("canvas.changed", json::object());
	result = CanvasToJson(*def);
	return true;
}

bool MethodCanvasRemove(const json &params, json &result, std::string &error)
{
	const std::string uuid = OptString(params, "uuid");
	if (uuid.empty()) {
		error = "canvas.remove requires a non-empty 'uuid'";
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
	// Destroy this canvas's preview surface FIRST: its obs_display renders the
	// canvas mix on the render thread, so the display (and its draw callback) must
	// be torn down -- synchronizing with the render thread -- before RemoveCanvas
	// frees the mix, else the next render dereferences freed memory (UAF). A live
	// output is already refused above; an idle-but-previewed canvas is not, which
	// is exactly the case this guards.
	if (PreviewManager *pm = Preview::Instance()) {
		pm->Destroy(uuid);
	}
	// Drop the engine's cached encoder pair for the removed canvas (it is bound to
	// a mix that goes away with the canvas), then destroy the live mix itself.
	ObsBootstrap::Multistream().InvalidateCanvasEncoders(uuid);
	ObsBootstrap::CanvasRuntime().RemoveCanvas(uuid);

	store.Remove(uuid);
	store.Save();
	ObsBootstrap::PruneSceneLinksForCanvas(uuid);
	EmitEvent("canvas.changed", json::object());
	result = json{{"removed", uuid}};
	return true;
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

// Map one StreamProfile to the bridge's profile shape. `platform` is the display
// prefix (e.g. "YouTube"); `service` is the raw service id (rtmp_common etc.).
json StreamProfileToJson(const StreamProfile &p)
{
	return json{
		{"uuid", p.uuid},
		{"label", p.label},
		{"isPrimary", p.isPrimary},
		{"service", p.serviceId},
		{"platform", p.PlatformName()},
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
	if (!params.is_object()) {
		error = "streamProfile.create expects an object";
		return false;
	}
	const std::string label = OptString(params, "label");
	if (label.empty()) {
		error = "streamProfile.create requires a non-empty 'label'";
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
	store.Save();

	EmitEvent("streamProfile.changed", json::object());
	result = json{{"uuid", uuid}};
	return true;
}

bool MethodStreamProfileUpdate(const json &params, json &result, std::string &error)
{
	if (!params.is_object()) {
		error = "streamProfile.update expects an object";
		return false;
	}
	const std::string uuid = OptString(params, "uuid");
	if (uuid.empty()) {
		error = "streamProfile.update requires a non-empty 'uuid'";
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
	store.Save();

	EmitEvent("streamProfile.changed", json::object());
	result = StreamProfileToJson(*p);
	return true;
}

bool MethodStreamProfileRemove(const json &params, json &result, std::string &error)
{
	const std::string uuid = OptString(params, "uuid");
	if (uuid.empty()) {
		error = "streamProfile.remove requires a non-empty 'uuid'";
		return false;
	}
	StreamProfileStore &store = ObsBootstrap::StreamProfiles();
	if (!store.Find(uuid)) {
		error = "no stream profile with uuid '" + uuid + "'";
		return false;
	}
	// Remove re-points the primary internally when the primary was removed.
	store.Remove(uuid);
	store.Save();

	EmitEvent("streamProfile.changed", json::object());
	result = json{{"removed", uuid}};
	return true;
}

bool MethodStreamProfileSetPrimary(const json &params, json &result, std::string &error)
{
	const std::string uuid = OptString(params, "uuid");
	if (uuid.empty()) {
		error = "streamProfile.setPrimary requires a non-empty 'uuid'";
		return false;
	}
	StreamProfileStore &store = ObsBootstrap::StreamProfiles();
	if (!store.SetPrimary(uuid)) {
		error = "no stream profile with uuid '" + uuid + "'";
		return false;
	}
	store.Save();

	EmitEvent("streamProfile.changed", json::object());
	result = json{{"uuid", uuid}, {"isPrimary", true}};
	return true;
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
	if (!params.is_object()) {
		error = "outputBinding.create expects an object";
		return false;
	}
	const std::string canvasUuid = OptString(params, "canvasUuid");
	if (canvasUuid.empty()) {
		error = "outputBinding.create requires a non-empty 'canvasUuid'";
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
	for (const OutputBinding &b : bindings.bindings) {
		if (b.profileUuid == profileUuid && b.canvasUuid == canvasUuid) {
			error = "that profile is already bound to this canvas";
			return false;
		}
	}

	OutputBinding &created = bindings.Add(canvasUuid);
	created.profileUuid = profileUuid;
	const std::string uuid = created.uuid;
	ObsBootstrap::OutputBindings().Save();

	EmitEvent("outputBinding.changed", json::object());
	result = json{{"uuid", uuid}};
	return true;
}

bool MethodOutputBindingUpdate(const json &params, json &result, std::string &error)
{
	if (!params.is_object()) {
		error = "outputBinding.update expects an object";
		return false;
	}
	const std::string uuid = OptString(params, "uuid");
	if (uuid.empty()) {
		error = "outputBinding.update requires a non-empty 'uuid'";
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

	for (const OutputBinding &other : bindings.bindings) {
		if (other.uuid != uuid && other.profileUuid == newProfile && other.canvasUuid == newCanvas) {
			error = "that profile is already bound to this canvas";
			return false;
		}
	}

	b->profileUuid = newProfile;
	b->canvasUuid = newCanvas;
	ObsBootstrap::OutputBindings().Save();

	EmitEvent("outputBinding.changed", json::object());
	result = OutputBindingToJson(*b);
	return true;
}

bool MethodOutputBindingSetEnabled(const json &params, json &result, std::string &error)
{
	const std::string uuid = OptString(params, "uuid");
	if (uuid.empty()) {
		error = "outputBinding.setEnabled requires a non-empty 'uuid'";
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
	b->enabled = enabled;
	ObsBootstrap::OutputBindings().Save();

	// outputBinding.changed is also the hook 4.4.4/4.4.5 use to re-decide whether a
	// canvas renders (AnyEnabledForCanvas may have flipped on this toggle).
	EmitEvent("outputBinding.changed", json::object());
	result = json{{"uuid", uuid}, {"enabled", enabled}};
	return true;
}

bool MethodOutputBindingRemove(const json &params, json &result, std::string &error)
{
	const std::string uuid = OptString(params, "uuid");
	if (uuid.empty()) {
		error = "outputBinding.remove requires a non-empty 'uuid'";
		return false;
	}
	OutputBindings &bindings = ObsBootstrap::OutputBindings().Bindings();
	if (!bindings.Find(uuid)) {
		error = "no output binding with uuid '" + uuid + "'";
		return false;
	}
	bindings.Remove(uuid);
	ObsBootstrap::OutputBindings().Save();

	EmitEvent("outputBinding.changed", json::object());
	result = json{{"removed", uuid}};
	return true;
}

// --- multistream live status + per-row control (4.4.4) ----------------------

bool MethodMultistreamStatus(const json & /*params*/, json &result, std::string & /*error*/)
{
	result = json{{"outputs", BuildStatusArray()}};
	return true;
}

bool MethodMultistreamStartOutput(const json &params, json &result, std::string &error)
{
	const std::string uuid = OptString(params, "uuid");
	if (uuid.empty()) {
		error = "multistream.startOutput requires a non-empty 'uuid'";
		return false;
	}
	result = json{{"ok", ObsBootstrap::Multistream().StartOutput(uuid)}};
	return true;
}

bool MethodMultistreamStopOutput(const json &params, json &result, std::string &error)
{
	const std::string uuid = OptString(params, "uuid");
	if (uuid.empty()) {
		error = "multistream.stopOutput requires a non-empty 'uuid'";
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

	const uint32_t renderLagged = obs_get_lagged_frames();
	const uint32_t renderTotal = obs_get_total_frames();
	const double renderLagPct = renderTotal > 0 ? (static_cast<double>(renderLagged) / renderTotal) * 100.0 : 0.0;

	uint32_t encodeSkipped = 0;
	uint32_t encodeTotal = 0;
	if (video_t *video = obs_get_video()) {
		encodeSkipped = video_output_get_skipped_frames(video);
		encodeTotal = video_output_get_total_frames(video);
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

		const double dropPct =
			s.totalFrames > 0 ? (static_cast<double>(s.droppedFrames) / s.totalFrames) * 100.0 : 0.0;

		outputs.push_back(json{
			{"bindingUuid", s.bindingUuid},
			{"profileLabel", s.profileLabel},
			{"canvasName", s.canvasName},
			{"state", MultistreamEngine::StateName(s.state)},
			{"bitrateKbps", bitrateKbps},
			{"droppedFrames", s.droppedFrames},
			{"totalFrames", s.totalFrames},
			{"dropPct", dropPct},
			{"congestionPct", s.congestion * 100.0},
			{"durationMs", s.connectTimeMs},
		});
	}
	bitrateCache.swap(nextCache);

	result = json{{"general", std::move(general)}, {"outputs", std::move(outputs)}};
	return true;
}

// --- audio mixer (per-source faders + volmeters, levels) --------------------

bool MethodAudioList(const json & /*params*/, json &result, std::string & /*error*/)
{
	json arr = json::array();
	for (const AudioMonitor::SourceInfo &s : ObsBootstrap::AudioMonitor().List()) {
		arr.push_back(json{
			{"uuid", s.uuid},
			{"name", s.name},
			{"deflection", s.deflection},
			{"volumeDb", s.volumeDb},
			{"muted", s.muted},
		});
	}
	result = json{{"sources", std::move(arr)}};
	return true;
}

bool MethodAudioSetDeflection(const json &params, json &result, std::string &error)
{
	const std::string uuid = OptString(params, "uuid");
	if (uuid.empty()) {
		error = "audio.setDeflection requires a non-empty 'uuid'";
		return false;
	}
	if (!params.is_object() || !params.contains("deflection") || !params["deflection"].is_number()) {
		error = "audio.setDeflection requires a number 'deflection'";
		return false;
	}
	const float deflection = params["deflection"].get<float>();
	float appliedDef = 0.0f;
	float appliedDb = 0.0f;
	if (!ObsBootstrap::AudioMonitor().SetDeflection(uuid, deflection, appliedDef, appliedDb)) {
		error = "no active audio source with uuid '" + uuid + "'";
		return false;
	}
	result = json{{"uuid", uuid}, {"deflection", appliedDef}, {"volumeDb", appliedDb}};
	return true;
}

bool MethodAudioSetMuted(const json &params, json &result, std::string &error)
{
	const std::string uuid = OptString(params, "uuid");
	if (uuid.empty()) {
		error = "audio.setMuted requires a non-empty 'uuid'";
		return false;
	}
	if (!params.is_object() || !params.contains("muted") || !params["muted"].is_boolean()) {
		error = "audio.setMuted requires a boolean 'muted'";
		return false;
	}
	obs_source_t *source = obs_get_source_by_uuid(uuid.c_str()); // addref'd
	if (!source) {
		error = "no source with uuid '" + uuid + "'";
		return false;
	}
	const bool muted = params["muted"].get<bool>();
	obs_source_set_muted(source, muted);
	obs_source_release(source);
	result = json{{"uuid", uuid}, {"muted", muted}};
	return true;
}

// --- global audio devices (Desktop Audio / Mic on output channels 1..6) ------
// Stock OBS seeds these on first run; the new frontend never did, so the mixer
// stayed empty. One row per global output channel: a wasapi capture source bound
// to the channel shows in the mixer (AudioMonitor enumerates channels 1..6).

struct GlobalAudioSlot {
	int channel;          // obs output channel 1..6
	bool input;           // true = mic/aux (wasapi_input_capture), false = desktop (wasapi_output_capture)
	const char *sourceId; // "wasapi_input_capture" | "wasapi_output_capture"
	const char *label;    // "Desktop Audio", "Mic/Aux", ...
	const char *role;     // "desktop" | "mic"
};

static const GlobalAudioSlot kGlobalAudioSlots[] = {
	{1, false, "wasapi_output_capture", "Desktop Audio", "desktop"},
	{2, false, "wasapi_output_capture", "Desktop Audio 2", "desktop"},
	{3, true, "wasapi_input_capture", "Mic/Aux", "mic"},
	{4, true, "wasapi_input_capture", "Mic/Aux 2", "mic"},
	{5, true, "wasapi_input_capture", "Mic/Aux 3", "mic"},
	{6, true, "wasapi_input_capture", "Mic/Aux 4", "mic"},
};

const GlobalAudioSlot *SlotForChannel(int ch)
{
	for (const GlobalAudioSlot &slot : kGlobalAudioSlots) {
		if (slot.channel == ch) {
			return &slot;
		}
	}
	return nullptr;
}

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

// Persist the current per-channel device map ({"<channel>":"<deviceId>", ...})
// for every channel that has a slot source bound, as a stringified-JSON blob under
// "state" in audio_devices.json (the theme.json convention).
void PersistGlobalAudio()
{
	json obj = json::object();
	for (const GlobalAudioSlot &slot : kGlobalAudioSlots) {
		OBSSourceAutoRelease cur = obs_get_output_source(slot.channel); // addref'd; may be null
		if (!cur) {
			continue;
		}
		OBSDataAutoRelease settings = obs_source_get_settings(cur);
		const char *deviceId = settings ? obs_data_get_string(settings, "device_id") : nullptr;
		obj[std::to_string(slot.channel)] = deviceId ? std::string(deviceId) : std::string();
	}
	WriteJsonString("audio_devices.json", "state", obj.dump());
}

// Bind/update/disable the device on one global channel. Does NOT persist or
// rebuild -- callers do, so seeding can batch.
bool ApplyGlobalDevice(int ch, const std::string &deviceId, std::string &error)
{
	const GlobalAudioSlot *slot = SlotForChannel(ch);
	if (!slot) {
		error = "channel " + std::to_string(ch) + " is not a global audio slot";
		return false;
	}

	if (deviceId.empty()) {
		// DISABLE: drop the channel's ref so the source is destroyed.
		obs_set_output_source(ch, nullptr);
		return true;
	}

	// If the channel already carries this slot's source type, just update its
	// device_id in place rather than recreating it.
	OBSSourceAutoRelease cur = obs_get_output_source(ch); // addref'd; may be null
	if (cur) {
		const char *curId = obs_source_get_id(cur);
		if (curId && std::string(curId) == slot->sourceId) {
			OBSDataAutoRelease s = obs_source_get_settings(cur);
			obs_data_set_string(s, "device_id", deviceId.c_str());
			obs_source_update(cur, s);
			return true;
		}
	}

	// Otherwise create a fresh source and bind it (the channel takes its own ref).
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "device_id", deviceId.c_str());
	obs_source_t *source = obs_source_create(slot->sourceId, slot->label, settings, nullptr); // create-ref
	if (!source) {
		error = std::string("obs_source_create failed for ") + slot->sourceId;
		return false;
	}
	obs_set_output_source(ch, source); // channel takes its own ref
	obs_source_release(source);        // drop the create-ref
	return true;
}

// First run: seed Desktop Audio (ch1) + Mic/Aux (ch3) to the OS default device and
// persist. Subsequent runs: restore the saved per-channel map. Never throws.
void SeedOrRestoreGlobalAudio()
{
	const std::string state = ReadJsonString("audio_devices.json", "state");

	auto firstRunSeed = []() {
		std::string err;
		ApplyGlobalDevice(1, "default", err);
		ApplyGlobalDevice(3, "default", err);
		PersistGlobalAudio();
		HostLog("[bridge] global audio: first-run seed (Desktop Audio + Mic/Aux -> default)");
	};

	if (state.empty()) {
		firstRunSeed();
		return;
	}

	json parsed;
	try {
		parsed = json::parse(state);
	} catch (...) {
		HostLog("[bridge] global audio: state parse failed; falling back to first-run seed");
		firstRunSeed();
		return;
	}
	if (!parsed.is_object()) {
		firstRunSeed();
		return;
	}

	int restored = 0;
	for (auto it = parsed.begin(); it != parsed.end(); ++it) {
		int ch = 0;
		try {
			ch = std::stoi(it.key());
		} catch (...) {
			continue;
		}
		if (!it.value().is_string()) {
			continue;
		}
		std::string err;
		if (ApplyGlobalDevice(ch, it.value().get<std::string>(), err)) {
			++restored;
		} else {
			HostLog("[bridge] global audio: restore ch" + std::to_string(ch) + " failed: " + err);
		}
	}
	HostLog("[bridge] global audio: restored " + std::to_string(restored) + " channel(s) from audio_devices.json");
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
	json arr = json::array();
	for (const GlobalAudioSlot &slot : kGlobalAudioSlots) {
		OBSSourceAutoRelease cur = obs_get_output_source(slot.channel); // addref'd; may be null
		json deviceId = nullptr;
		if (cur) {
			OBSDataAutoRelease settings = obs_source_get_settings(cur);
			const char *id = settings ? obs_data_get_string(settings, "device_id") : nullptr;
			deviceId = id ? json(std::string(id)) : json(std::string());
		}
		arr.push_back(json{
			{"channel", slot.channel},
			{"role", slot.role},
			{"label", slot.label},
			{"isInput", slot.input},
			{"deviceId", deviceId},
			{"active", !deviceId.is_null()},
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

	if (!ApplyGlobalDevice(channel, deviceId, error)) {
		return false;
	}
	PersistGlobalAudio();
	ObsBootstrap::AudioMonitor().Rebuild();
	EmitEvent("audio.changed", json::object());

	result = json{{"channel", channel}, {"deviceId", deviceId.empty() ? json(nullptr) : json(deviceId)}};
	return true;
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
// to <config>/obs-multistream/basic/{theme,layout}.json, reusing the same
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
	const std::string id = OptString(params, "id");
	if (id.empty()) {
		error = "transitions.setCurrent requires a non-empty 'id'";
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
	EmitEvent("transitions.changed", json::object());
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
	EmitEvent("transitions.changed", json::object());
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
	const std::string dock = OptString(params, "dock");
	if (dock.empty()) {
		error = "window.detach requires a non-empty 'dock'";
		return false;
	}
	const int windowId = wm->Detach(dock);
	if (windowId <= 0) {
		error = "failed to create detached window";
		return false;
	}
	EmitEvent("window.opened", json{{"windowId", windowId}, {"dock", dock}});
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
	EmitEvent("window.closed", json{{"windowId", windowId}, {"dock", dock}});
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
			error = "fullscreen requires a valid 'monitor' index in [0," + std::to_string(monitorCount) + ")";
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

	EmitEvent("projector.changed", json{{"opened", id}});
	HostLog("[bridge] projector.open -> ok id=" + std::to_string(id) + " kind=" + KindToString(kind) + " mode=" +
		mode);
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
	EmitEvent("projector.changed", json{{"closed", id}});
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
		return json{{"enabled", false}, {"port", 47800},      {"token", ""},
			    {"allowMutations", true}, {"allowGoLive", false}, {"listening", false},
			    {"lastError", ""},        {"endpoint", "http://127.0.0.1:47800/mcp"}};
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
	EmitEvent("mcp.changed", json::object());
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
	EmitEvent("mcp.changed", json::object());
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
	if (!params.is_object()) {
		error = "settings.restore expects an object";
		return false;
	}

	if (params.contains("video")) {
		json r;
		std::string e;
		MethodSettingsSetVideo(params["video"], r, e);
	}
	if (params.contains("audio")) {
		json r;
		std::string e;
		MethodSettingsSetAudio(params["audio"], r, e);
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
			MethodAudioSetGlobalDevice(setParams, r, e);
		}
	}

	if (params.contains("streamProfiles")) {
		StreamProfileStore &s = ObsBootstrap::StreamProfiles();
		s.FromJson(params["streamProfiles"]);
		s.Save();
	}
	if (params.contains("outputBindings")) {
		OutputBindingStore &s = ObsBootstrap::OutputBindings();
		s.FromJson(params["outputBindings"]);
		s.Save();
	}

	if (params.contains("canvases")) {
		CanvasStore &cs = ObsBootstrap::Canvases();

		// Snapshot the pre-restore non-Default canvases (uuid + resolution) so we can
		// tell which live mixes were created during the edit (tear them down) and
		// which surviving ones had a resolution change (revert the mix). A live canvas
		// can't be edited, so it can't appear in a revert delta; we still defensively
		// skip touching one.
		struct Prev {
			std::string uuid;
			uint32_t w, h, fpsN, fpsD;
		};
		std::vector<Prev> before;
		for (const CanvasDefinition &d : cs.Definitions()) {
			if (!d.isDefault) {
				before.push_back({d.uuid, d.width, d.height, d.fpsNum, d.fpsDen});
			}
		}

		cs.FromJson(params["canvases"]);
		cs.Save();

		std::unordered_set<std::string> after;
		for (const CanvasDefinition &d : cs.Definitions()) {
			if (!d.isDefault) {
				after.insert(d.uuid);
			}
		}

		CanvasRuntime &rt = ObsBootstrap::CanvasRuntime();

		// 1) Tear down mixes for canvases created during the edit but absent after the
		// revert. Mirror MethodCanvasRemove's teardown order: preview surface first
		// (its obs_display reads the mix on the render thread), then the engine's
		// cached encoders, then the mix. Skip a live canvas.
		for (const Prev &p : before) {
			if (after.count(p.uuid)) {
				continue;
			}
			if (CanvasIsLive(p.uuid)) {
				continue;
			}
			if (PreviewManager *pm = Preview::Instance()) {
				pm->Destroy(p.uuid);
			}
			ObsBootstrap::Multistream().InvalidateCanvasEncoders(p.uuid);
			rt.RemoveCanvas(p.uuid);
		}

		// 2) (Re)create mixes for any restored canvas missing one (removed during the
		// edit, brought back by the revert). EnsureCanvas is idempotent.
		rt.SyncFromDefinitions();

		// 3) Revert the live mix resolution for surviving non-Default canvases whose
		// resolution changed during the edit, invalidating their cached encoders too
		// (mirrors MethodCanvasUpdate). The Default canvas has no runtime mix -- its
		// resolution drives global video, handled below.
		for (const CanvasDefinition &d : cs.Definitions()) {
			if (d.isDefault || CanvasIsLive(d.uuid)) {
				continue;
			}
			for (const Prev &p : before) {
				if (p.uuid != d.uuid) {
					continue;
				}
				if (p.w != d.width || p.h != d.height || p.fpsN != d.fpsNum || p.fpsD != d.fpsDen) {
					ObsBootstrap::Multistream().InvalidateCanvasEncoders(d.uuid);
					rt.ResetVideo(d);
				}
				break;
			}
		}

		// 4) Make global video follow the restored Default canvas def so the main
		// pipeline (and output resolution) reverts too. Reuse Task 1's path; skip
		// while an output is live (a live pipeline can't be reset).
		if (!AnyOutputActive()) {
			const CanvasDefinition &def = cs.Default();
			std::string e;
			ApplyGlobalVideo(def.width, def.height, def.width, def.height, def.fpsNum, def.fpsDen, e);
		}
	}

	if (params.contains("hotkeys")) {
		Hotkeys::RestoreFromSnapshot(params["hotkeys"]);
	}
	if (params.contains("mcp")) {
		json r;
		std::string e;
		MethodMcpSetConfig(params["mcp"], r, e);
	}

	EmitEvent("canvas.changed", json::object());
	EmitEvent("streamProfile.changed", json::object());
	EmitEvent("outputBinding.changed", json::object());
	EmitEvent("multistream.changed", json::object());
	EmitEvent("mcp.changed", json::object());
	result = json{{"ok", true}};
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
	const std::string name = OptString(params, "name");
	if (name.empty()) {
		error = "collections.create requires a non-empty 'name'";
		return false;
	}
	const SceneCollectionRecord &added = ObsBootstrap::SceneCollections().Create(name);
	result = json{{"id", added.id}};
	EmitEvent("collections.changed", json::object());
	return true;
}

bool MethodCollectionsRename(const json &params, json &result, std::string &error)
{
	if (ObsBootstrap::SceneCollections().IndexWasCorrupt()) {
		error = "scene collection index is corrupt; cannot modify collections";
		return false;
	}
	const std::string id = OptString(params, "id");
	const std::string name = OptString(params, "name");
	if (id.empty()) {
		error = "collections.rename requires a non-empty 'id'";
		return false;
	}
	if (name.empty()) {
		error = "collections.rename requires a non-empty 'name'";
		return false;
	}
	if (!ObsBootstrap::SceneCollections().Rename(id, name)) {
		error = "no scene collection with id '" + id + "'";
		return false;
	}
	result = json{{"id", id}, {"name", name}};
	EmitEvent("collections.changed", json::object());
	return true;
}

bool MethodCollectionsDuplicate(const json &params, json &result, std::string &error)
{
	SceneCollections &store = ObsBootstrap::SceneCollections();
	if (store.IndexWasCorrupt()) {
		error = "scene collection index is corrupt; cannot modify collections";
		return false;
	}
	const std::string name = OptString(params, "name");
	if (name.empty()) {
		error = "collections.duplicate requires a non-empty 'name'";
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
	EmitEvent("collections.changed", json::object());
	return true;
}

bool MethodCollectionsSwitch(const json &params, json &result, std::string &error)
{
	if (ObsBootstrap::SceneCollections().IndexWasCorrupt()) {
		error = "scene collection index is corrupt; cannot modify collections";
		return false;
	}
	const std::string id = OptString(params, "id");
	if (id.empty()) {
		error = "collections.switch requires a non-empty 'id'";
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
	const std::string id = OptString(params, "id");
	if (id.empty()) {
		error = "collections.remove requires a non-empty 'id'";
		return false;
	}
	if (!ObsBootstrap::SceneCollections().Remove(id, error)) {
		return false;
	}
	result = json{{"removed", id}};
	EmitEvent("collections.changed", json::object());
	return true;
}

} // namespace

bool WriteJsonString(const char *file, const char *key, const std::string &value)
{
	const std::string path = MultistreamBasicPath(file);
	if (path.empty()) {
		return false;
	}
	std::filesystem::path dir = std::filesystem::u8path(path).parent_path();
	os_mkdirs(dir.u8string().c_str());
	OBSDataAutoRelease root = obs_data_create();
	obs_data_set_string(root, key, value.c_str());
	return obs_data_save_json_pretty_safe(root, path.c_str(), "tmp", "bak");
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
		{"scenes.reorder", MethodScenesReorder},
		{"collections.list", MethodCollectionsList},
		{"collections.create", MethodCollectionsCreate},
		{"collections.rename", MethodCollectionsRename},
		{"collections.duplicate", MethodCollectionsDuplicate},
		{"collections.switch", MethodCollectionsSwitch},
		{"collections.remove", MethodCollectionsRemove},
		{"sceneItems.list", MethodSceneItemsList},
		{"sceneItems.setVisible", MethodSceneItemsSetVisible},
		{"sceneItems.setLocked", MethodSceneItemsSetLocked},
		{"sceneItems.remove", MethodSceneItemsRemove},
		{"sceneItems.reorder", MethodSceneItemsReorder},
		{"sceneItems.getTransform", MethodSceneItemsGetTransform},
		{"sceneItems.setTransform", MethodSceneItemsSetTransform},
		{"sceneItems.transformAction", MethodSceneItemsTransformAction},
		{"sceneItems.setScaleFilter", MethodSceneItemsSetScaleFilter},
		{"sourceTypes.list", MethodSourceTypesList},
		{"sources.create", MethodSourcesCreate},
		{"sources.listExisting", MethodSourcesListExisting},
		{"sources.addExisting", MethodSourcesAddExisting},
		{"sources.rename", MethodSourcesRename},
		{"properties.get", MethodPropertiesGet},
		{"properties.set", MethodPropertiesSet},
		{"properties.button", MethodPropertiesButton},
		{"dialog.openFile", MethodDialogOpenFile},
		{"filterTypes.list", MethodFilterTypesList},
		{"filters.list", MethodFiltersList},
		{"filters.add", MethodFiltersAdd},
		{"filters.remove", MethodFiltersRemove},
		{"filters.setEnabled", MethodFiltersSetEnabled},
		{"filters.reorder", MethodFiltersReorder},
		{"filters.rename", MethodFiltersRename},
		{"settings.getVideo", MethodSettingsGetVideo},
		{"settings.setVideo", MethodSettingsSetVideo},
		{"settings.getAudio", MethodSettingsGetAudio},
		{"settings.setAudio", MethodSettingsSetAudio},
		{"settings.snapshot", MethodSettingsSnapshot},
		{"settings.restore", MethodSettingsRestore},
		{"canvas.list", MethodCanvasList},
		{"canvas.create", MethodCanvasCreate},
		{"canvas.update", MethodCanvasUpdate},
		{"canvas.remove", MethodCanvasRemove},
		{"encoderTypes.list", MethodEncoderTypesList},
		{"streamProfile.list", MethodStreamProfileList},
		{"streamProfile.create", MethodStreamProfileCreate},
		{"streamProfile.update", MethodStreamProfileUpdate},
		{"streamProfile.remove", MethodStreamProfileRemove},
		{"streamProfile.setPrimary", MethodStreamProfileSetPrimary},
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
		{"multistream.startOutput", MethodMultistreamStartOutput},
		{"multistream.stopOutput", MethodMultistreamStopOutput},
		{"undo.undo", MethodUndoUndo},
		{"undo.redo", MethodUndoRedo},
		{"undo.state", MethodUndoState},
		{"stats.get", MethodStatsGet},
		{"audio.list", MethodAudioList},
		{"audio.setDeflection", MethodAudioSetDeflection},
		{"audio.setMuted", MethodAudioSetMuted},
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
		{"display.listMonitors", MethodDisplayListMonitors},
		{"projector.open", MethodProjectorOpen},
		{"projector.close", MethodProjectorClose},
		{"projector.list", MethodProjectorList},
		{"hotkeys.list", Hotkeys::MethodList},
		{"hotkeys.set", Hotkeys::MethodSet},
		{"hotkeys.clear", Hotkeys::MethodClear},
		{"mcp.getConfig", MethodMcpGetConfig},
		{"mcp.setConfig", MethodMcpSetConfig},
		{"mcp.regenerateToken", MethodMcpRegenerateToken},
	};

	// Notify JS whenever the undo stack changes (add/undo/redo/clear) so the UI's
	// undo/redo affordance re-reads canUndo/canRedo + names.
	ObsBootstrap::Undo().onChanged = [] { EmitUndoChanged(); };

	obs_frontend_add_event_callback(OnFrontendEvent, nullptr);
	HostLog("[bridge] init: " + std::to_string(g_methods.size()) + " methods, obs event forwarding armed");
}

void Shutdown()
{
	obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
	// Drop the undo->event hook so the g_undo.Clear() later in Stop() (after the CEF
	// loop has returned) doesn't try to emit through a torn-down bridge.
	ObsBootstrap::Undo().onChanged = nullptr;
	g_methods.clear();
	if (g_cpuInfo) {
		os_cpu_usage_info_destroy(g_cpuInfo);
		g_cpuInfo = nullptr;
	}
	g_bitrateCache.clear();
}

void SeedGlobalAudio()
{
	SeedOrRestoreGlobalAudio();
}

void ClearGlobalAudio()
{
	// Drop each global channel's ref so the wasapi sources die before obs_shutdown.
	for (const GlobalAudioSlot &slot : kGlobalAudioSlots) {
		obs_set_output_source(slot.channel, nullptr);
	}
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
	return it->second(params, result, error);
}

void EmitEvent(const std::string &name, const json &payload)
{
	const std::string payloadDump = payload.dump();
	if (CefCurrentlyOn(TID_UI)) {
		DoEmit(name, payloadDump);
		return;
	}
	CefPostTask(TID_UI, base::BindOnce(&DoEmit, name, payloadDump));
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
	EmitEvent("multistream.changed", json{{"outputs", BuildStatusArray()}});
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
	EmitEvent("audio.levels", json{{"levels", std::move(arr)}});
}

void EmitAudioChanged()
{
	EmitEvent("audio.changed", json::object());
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

	json result;
	std::string error;
	if (Bridge::Dispatch(method, params, result, error)) {
		callback->Success(result.dump());
		HostLog("[bridge] " + method + " -> ok");
	} else {
		callback->Failure(404, error);
		HostLog("[bridge] " + method + " -> FAIL (" + error + ")");
	}
	return true;
}
