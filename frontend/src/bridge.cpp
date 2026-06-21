#include "bridge.hpp"

#include <functional>
#include <mutex>
#include <unordered_map>

#include <obs.h>
#include <obs-frontend-api.h>

#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#include "log.hpp"
#include "preview_window.hpp"

namespace Bridge {

namespace {

// One method: (params) -> (result | error). Returns false and fills `error` on
// failure. Runs on the browser UI thread.
using MethodFn = std::function<bool(const json &params, json &result, std::string &error)>;

std::unordered_map<std::string, MethodFn> g_methods;

// Streaming is a stub for 4.1.4: a flag toggled by streaming.start/stop, read by
// getStreamingState, and announced via the streaming.changed push event. Touched
// only on the UI thread (router callbacks), so no lock needed.
bool g_streaming = false;

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
	result = json{{"active", g_streaming}};
	return true;
}

// Shared toggle for streaming.start / streaming.stop. Sets the flag, emits the
// streaming.changed push (proving server->client events end-to-end), and returns
// {active}. No real streaming yet (4.1.4 stub).
bool SetStreaming(bool active, json &result)
{
	g_streaming = active;
	json payload = json{{"active", g_streaming}};
	EmitEvent("streaming.changed", payload);
	result = payload;
	return true;
}

bool MethodStreamingStart(const json & /*params*/, json &result, std::string & /*error*/)
{
	return SetStreaming(true, result);
}

bool MethodStreamingStop(const json & /*params*/, json &result, std::string & /*error*/)
{
	return SetStreaming(false, result);
}

// Position the native preview overlay over the UI's reported region. params:
// {x,y,w,h} in CSS pixels (host-client space) + dpr (devicePixelRatio). Convert
// CSS px -> device px so the HWND lands exactly on the UI region under any DPI
// scale. Runs on the UI thread (router callback), the same thread that owns the
// HWND, so the SetWindowPos is direct. Lazily creates the overlay on first call.
bool MethodPreviewSetRect(const json &params, json & /*result*/, std::string &error)
{
	PreviewWindow *pw = Preview::Instance();
	if (!pw) {
		error = "preview not ready";
		return false;
	}
	if (!params.is_object()) {
		error = "setRect expects an object {x,y,w,h,dpr}";
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

	HostLog("[bridge] preview.setRect dev-px x=" + std::to_string(x) + " y=" + std::to_string(y) +
		" w=" + std::to_string(w) + " h=" + std::to_string(h) + " (dpr=" + std::to_string(dpr) + ")");
	pw->SetRect(x, y, w, h);
	return true;
}

bool MethodPreviewHide(const json & /*params*/, json & /*result*/, std::string &error)
{
	PreviewWindow *pw = Preview::Instance();
	if (!pw) {
		error = "preview not ready";
		return false;
	}
	pw->Hide();
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

// --- scenes -----------------------------------------------------------------

bool MethodScenesList(const json & /*params*/, json &result, std::string & /*error*/)
{
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

	EmitEvent("scenes.changed", json::object());
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

	EmitEvent("scenes.changed", json::object());
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

	EmitEvent("scenes.changed", json::object());
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

	EmitEvent("scenes.changed", json::object());
	result = json{{"name", to}};
	return true;
}

// --- scene items ------------------------------------------------------------

// Emit sceneItems.changed for the resolved scene. Passes the scene's actual name
// so the UI can decide whether the change applies to what it is showing.
void EmitSceneItemsChanged(obs_source_t *sceneSource)
{
	const char *name = sceneSource ? obs_source_get_name(sceneSource) : nullptr;
	EmitEvent("sceneItems.changed", json{{"scene", name ? json(name) : json(nullptr)}});
}

bool MethodSceneItemsList(const json &params, json &result, std::string &error)
{
	obs_source_t *sceneSource = ResolveSceneSource(OptString(params, "scene")); // addref'd
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
	obs_source_t *sceneSource = ResolveSceneSource(OptString(params, "scene"));
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
	EmitSceneItemsChanged(sceneSource);
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
	obs_source_t *sceneSource = ResolveSceneSource(OptString(params, "scene"));
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
	EmitSceneItemsChanged(sceneSource);
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
	obs_source_t *sceneSource = ResolveSceneSource(OptString(params, "scene"));
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
	EmitSceneItemsChanged(sceneSource);
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

	obs_source_t *sceneSource = ResolveSceneSource(OptString(params, "scene"));
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
	EmitSceneItemsChanged(sceneSource);
	obs_source_release(sceneSource);
	result = json{{"id", id}, {"direction", direction}};
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
