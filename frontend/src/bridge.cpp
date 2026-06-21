#include "bridge.hpp"

#include <algorithm>
#include <functional>
#include <mutex>
#include <unordered_map>

#include <obs.h>
#include <obs-frontend-api.h>

#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#include "log.hpp"
#include "obs_bootstrap.hpp"
#include "preview_window.hpp"
#include "properties_serializer.hpp"

#include "multistream/CanvasRuntime.hpp"
#include "multistream/CanvasStore.hpp"
#include "multistream/MultistreamEngine.hpp"
#include "multistream/OutputBindingStore.hpp"
#include "multistream/StreamProfileStore.hpp"
#include <util/dstr.h>
#include <util/platform.h>

namespace Bridge {

namespace {

// One method: (params) -> (result | error). Returns false and fills `error` on
// failure. Runs on the browser UI thread.
using MethodFn = std::function<bool(const json &params, json &result, std::string &error)>;

std::unordered_map<std::string, MethodFn> g_methods;

// The UI browser EmitEvent targets. Set/cleared on the UI thread; read on the UI
// thread (EmitEvent posts there first). guarded for paranoia across the post.
std::mutex g_browser_mutex;
CefRefPtr<CefBrowser> g_ui_browser;

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

	HostLog("[bridge] preview.setRect dev-px x=" + std::to_string(x) + " y=" + std::to_string(y) +
		" w=" + std::to_string(w) + " h=" + std::to_string(h) + " (dpr=" + std::to_string(dpr) +
		(canvas.empty() ? ")" : ", canvas=" + canvas + ")"));
	pm->SetRect(canvas, x, y, w, h);
	return true;
}

bool MethodPreviewHide(const json &params, json & /*result*/, std::string &error)
{
	PreviewManager *pm = Preview::Instance();
	if (!pm) {
		error = "preview not ready";
		return false;
	}
	pm->Hide(PreviewCanvasParam(params));
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

// Rebuild the current obs_video_info, override only the passed fields, and
// obs_reset_video. Preserves graphics_module/colorspace/range/format/adapter/
// gpu_conversion/scale_type so only resolution+FPS change. Refuses while an
// output is live and on a failed reset restores the prior config so video is
// never left broken. Emits settings.videoChanged + re-validates the preview.
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
	const obs_video_info previous = ovi; // for rollback

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

	const int rv = obs_reset_video(&ovi);
	if (rv != OBS_VIDEO_SUCCESS) {
		// Restore the prior config so we never leave video in a broken state.
		obs_video_info restore = previous;
		const int rb = obs_reset_video(&restore);
		HostLog("[bridge] settings.setVideo reset FAILED code=" + std::to_string(rv) +
			", rollback code=" + std::to_string(rb));
		error = "obs_reset_video failed (code " + std::to_string(rv) + ")";
		return false;
	}

	// The obs_display swapchain survives a video reset; just invalidate the cached
	// letterbox transform so the next frame re-letterboxes to the new base size.
	Preview::OnVideoReset();

	obs_video_info applied = {};
	obs_get_video_info(&applied);
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
		return obs_get_output_source(0); // already addref'd; null if unbound
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

	if (!Preview::SelectFromBridge(PreviewCanvasParam(params), scene, id, hasId)) {
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
	// to output channel 0.
	obs_source_t *current = obs_get_output_source(0); // addref'd; may be null
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
	result = json{{"name", name}};
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
		// Remove from the additional canvas's own scenes; the runtime refuses the
		// last scene and switches channel 0 off the target first.
		if (!ObsBootstrap::CanvasRuntime().RemoveScene(canvasTarget.uuid, name)) {
			error = "could not remove scene '" + name + "' from that canvas";
			return false;
		}
		EmitScenesChanged(canvasTarget.uuid);
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

	// If the target is the current scene, switch output 0 to the fallback first.
	obs_source_t *current = obs_get_output_source(0); // addref'd
	const bool removingCurrent = current && current == target;
	if (current) {
		obs_source_release(current);
	}
	if (removingCurrent && ctx.fallback) {
		obs_set_output_source(0, ctx.fallback);
	}

	obs_source_remove(target);
	obs_source_release(target);
	if (ctx.fallback) {
		obs_source_release(ctx.fallback);
	}

	EmitScenesChanged(std::string());
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
	obs_set_output_source(0, source);
	obs_source_release(source);

	EmitScenesChanged(std::string());
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
	result = json{{"name", to}};
	return true;
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
	obs_sceneitem_set_visible(item, visible);
	EmitSceneItemsChanged(sceneSource, ResolveCanvasTarget(params).uuid);
	obs_source_release(sceneSource);
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
	obs_sceneitem_set_locked(item, locked);
	EmitSceneItemsChanged(sceneSource, ResolveCanvasTarget(params).uuid);
	obs_source_release(sceneSource);
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
	obs_sceneitem_remove(item);
	EmitSceneItemsChanged(sceneSource, ResolveCanvasTarget(params).uuid);
	obs_source_release(sceneSource);
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
	obs_sceneitem_set_order(item, *movement);
	EmitSceneItemsChanged(sceneSource, ResolveCanvasTarget(params).uuid);
	obs_source_release(sceneSource);
	result = json{{"id", id}, {"direction", direction}};
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
	obs_source_release(source); // drop the create-ref; scene holds the source

	EmitSceneItemsChanged(sceneSource, ResolveCanvasTarget(params).uuid);
	obs_source_release(sceneSource);

	if (!item) {
		error = "obs_scene_add failed";
		return false;
	}
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
	obs_source_release(source); // drop our lookup ref

	EmitSceneItemsChanged(sceneSource, ResolveCanvasTarget(params).uuid);
	obs_source_release(sceneSource);

	if (!item) {
		error = "obs_scene_add failed";
		return false;
	}
	result = json{{"id", itemId}, {"source", name}};
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
bool BuildPropertiesResult(const PropertyKind *kind, void *obj, json &result, std::string &error)
{
	obs_properties_t *props = kind->get_props(obj);
	obs_data_t *settings = kind->get_settings(obj); // addref'd

	json descriptors = PropertiesSerializer::SerializeProperties(props, settings);

	json values = json::object();
	if (settings) {
		const char *raw = obs_data_get_json(settings);
		if (raw) {
			values = json::parse(raw, nullptr, false);
			if (values.is_discarded()) {
				values = json::object();
			}
		}
	}

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
	// changes don't touch the mix.) No-op for the Default canvas, which has no
	// runtime mix.
	if (resChanged) {
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

// Post the actual ExecuteJavaScript on TID_UI. Built from JSON dumps so the name
// and payload are correctly quoted/escaped.
void DoEmit(const std::string &name, const std::string &payloadDump)
{
	CEF_REQUIRE_UI_THREAD();

	CefRefPtr<CefBrowser> browser;
	{
		std::lock_guard<std::mutex> lock(g_browser_mutex);
		browser = g_ui_browser;
	}
	if (!browser) {
		return; // UI browser not yet created or already gone
	}
	CefRefPtr<CefFrame> frame = browser->GetMainFrame();
	if (!frame) {
		return;
	}

	// window.__obsEmit("<name>", <payload>); guarded so it no-ops before the
	// bridge client script has defined the sink.
	const std::string nameDump = json(name).dump();
	std::string script = "if(window.__obsEmit){window.__obsEmit(" + nameDump + "," + payloadDump + ");}";
	frame->ExecuteJavaScript(script, frame->GetURL(), 0);
}

} // namespace

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
		{"preview.select", MethodPreviewSelect},
		{"scenes.list", MethodScenesList},
		{"scenes.create", MethodScenesCreate},
		{"scenes.remove", MethodScenesRemove},
		{"scenes.setCurrent", MethodScenesSetCurrent},
		{"scenes.rename", MethodScenesRename},
		{"sceneItems.list", MethodSceneItemsList},
		{"sceneItems.setVisible", MethodSceneItemsSetVisible},
		{"sceneItems.setLocked", MethodSceneItemsSetLocked},
		{"sceneItems.remove", MethodSceneItemsRemove},
		{"sceneItems.reorder", MethodSceneItemsReorder},
		{"sourceTypes.list", MethodSourceTypesList},
		{"sources.create", MethodSourcesCreate},
		{"sources.listExisting", MethodSourcesListExisting},
		{"sources.addExisting", MethodSourcesAddExisting},
		{"properties.get", MethodPropertiesGet},
		{"properties.set", MethodPropertiesSet},
		{"properties.button", MethodPropertiesButton},
		{"settings.getVideo", MethodSettingsGetVideo},
		{"settings.setVideo", MethodSettingsSetVideo},
		{"settings.getAudio", MethodSettingsGetAudio},
		{"settings.setAudio", MethodSettingsSetAudio},
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
		{"multistream.status", MethodMultistreamStatus},
		{"multistream.startOutput", MethodMultistreamStartOutput},
		{"multistream.stopOutput", MethodMultistreamStopOutput},
	};

	obs_frontend_add_event_callback(OnFrontendEvent, nullptr);
	HostLog("[bridge] init: " + std::to_string(g_methods.size()) + " methods, obs event forwarding armed");
}

void Shutdown()
{
	obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
	g_methods.clear();
}

void SetUiBrowser(CefRefPtr<CefBrowser> browser)
{
	std::lock_guard<std::mutex> lock(g_browser_mutex);
	g_ui_browser = browser;
}

void ClearUiBrowser()
{
	std::lock_guard<std::mutex> lock(g_browser_mutex);
	g_ui_browser = nullptr;
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
