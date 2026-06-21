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
