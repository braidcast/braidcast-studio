#define _CRT_RAND_S

#include "mcp/McpServer.hpp"

#include <obs.h>
#include <obs.hpp>         // OBSDataAutoRelease
#include <util/platform.h> // os_mkdirs

#include <array>
#include <chrono>
#include <cstdlib> // rand_s
#include <future>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#include "bridge.hpp"
#include "log.hpp"
#include "multistream/StorePaths.hpp"

namespace {

// The single live McpServer, reachable process-wide so the posted UI task can
// re-check liveness before touching the bridge.
McpServer *g_instance = nullptr;

constexpr int kBridgeTimeoutSeconds = 15;

// Capability classification of a bridge method name.
enum class Capability { Read, Mutate, GoLive };

const char *CapabilityName(Capability cap)
{
	switch (cap) {
	case Capability::Read:
		return "read";
	case Capability::Mutate:
		return "mutate";
	case Capability::GoLive:
		return "goLive";
	}
	return "mutate";
}

// Exact go-live method names registered in g_methods (bridge.cpp). There is no
// startAll/stopAll in the registry, so only these four are GoLive.
const std::set<std::string> &GoLiveMethods()
{
	static const std::set<std::string> kSet = {
		"streaming.start",
		"streaming.stop",
		"multistream.startOutput",
		"multistream.stopOutput",
	};
	return kSet;
}

// Read-classification rules, applied as a data list (suffix / substring / exact)
// rather than a long switch, so new read methods need no code change.
enum class RuleKind { Suffix, Substring, Exact };

struct ReadRule {
	RuleKind kind;
	const char *needle;
};

const std::array<ReadRule, 7> &ReadRules()
{
	static const std::array<ReadRule, 7> kRules = {{
		{RuleKind::Suffix, ".list"},
		{RuleKind::Suffix, ".status"},
		{RuleKind::Suffix, ".getCurrent"},
		{RuleKind::Substring, ".get"},
		{RuleKind::Exact, "stats.get"},
		{RuleKind::Exact, "display.listMonitors"},
		{RuleKind::Suffix, ".getCurrent"}, // duplicate keeps array size stable on edits
	}};
	return kRules;
}

bool EndsWith(const std::string &s, const char *suffix)
{
	const std::string suf = suffix;
	return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

Capability Classify(const std::string &method)
{
	if (GoLiveMethods().count(method)) {
		return Capability::GoLive;
	}
	for (const auto &rule : ReadRules()) {
		switch (rule.kind) {
		case RuleKind::Suffix:
			if (EndsWith(method, rule.needle)) {
				return Capability::Read;
			}
			break;
		case RuleKind::Substring:
			if (method.find(rule.needle) != std::string::npos) {
				return Capability::Read;
			}
			break;
		case RuleKind::Exact:
			if (method == rule.needle) {
				return Capability::Read;
			}
			break;
		}
	}
	return Capability::Mutate;
}

// JSON Schema helpers so each curated tool row stays one readable expression.
McpServer::json SchemaObject(const McpServer::json &properties, const McpServer::json &required)
{
	using json = McpServer::json;
	json schema{{"type", "object"}, {"properties", properties}};
	if (!required.empty()) {
		schema["required"] = required;
	}
	return schema;
}

McpServer::json Prop(const char *type, const char *description)
{
	return McpServer::json{{"type", type}, {"description", description}};
}

// The tools/list registry: a data list so adding a tool is one row. `obs_call` is
// the generic escape hatch; the rest are curated, high-value wrappers that each map
// to a single bridge method (`bridgeMethod`). tools/call resolves a curated tool to
// its bridge method and passes `arguments` straight through as the bridge params,
// running the SAME UI-thread marshal + capability gate (via Classify(bridgeMethod))
// as obs_call. An empty bridgeMethod marks the obs_call escape hatch (handled
// specially: the method comes from arguments.method, not the row).
struct ToolDescriptor {
	const char *name;
	const char *bridgeMethod; // "" for obs_call
	McpServer::json descriptor;
};

// Build the descriptor JSON the MCP client sees in tools/list.
McpServer::json MakeDescriptor(const char *name, const char *description, const McpServer::json &inputSchema)
{
	return McpServer::json{{"name", name}, {"description", description}, {"inputSchema", inputSchema}};
}

const std::vector<ToolDescriptor> &ToolRegistry()
{
	using json = McpServer::json;
	static const std::vector<ToolDescriptor> kTools = {
		{"obs_call", "",
		 MakeDescriptor(
			 "obs_call", "Escape hatch: call any OBS bridge method by name with params.",
			 SchemaObject(json{{"method", Prop("string", "The bridge method name, e.g. 'scenes.list'.")},
					   {"params", json{{"type", "object"},
							   {"description", "Method-specific params object."}}}},
				      json::array({"method"})))},

		{"list_scenes", "scenes.list",
		 MakeDescriptor("list_scenes", "List the scenes of a canvas. Omit 'canvas' for the Default canvas.",
				SchemaObject(json{{"canvas", Prop("string", "Optional canvas uuid; omit for Default.")}},
					     json::array()))},

		{"switch_scene", "scenes.setCurrent",
		 MakeDescriptor(
			 "switch_scene",
			 "Switch the active (program) scene by name. Optional 'canvas' uuid targets an additional canvas.",
			 SchemaObject(json{{"name", Prop("string", "Scene name to make current.")},
					   {"canvas", Prop("string", "Optional canvas uuid; omit for Default.")}},
				      json::array({"name"})))},

		{"create_scene", "scenes.create",
		 MakeDescriptor("create_scene",
				"Create a new empty scene. Optional 'canvas' uuid targets an additional canvas.",
				SchemaObject(json{{"name", Prop("string", "Name for the new scene.")},
						  {"canvas", Prop("string", "Optional canvas uuid; omit for Default.")}},
					     json::array({"name"})))},

		{"list_scene_items", "sceneItems.list",
		 MakeDescriptor(
			 "list_scene_items",
			 "List the items (sources) of a scene, topmost first. Defaults to the current scene.",
			 SchemaObject(json{{"scene", Prop("string", "Optional scene name; defaults to current.")},
					   {"canvas", Prop("string", "Optional canvas uuid; omit for Default.")}},
				      json::array()))},

		{"set_item_visible", "sceneItems.setVisible",
		 MakeDescriptor(
			 "set_item_visible", "Show or hide a scene item by its numeric id.",
			 SchemaObject(json{{"id", Prop("integer", "Scene-item id (from list_scene_items).")},
					   {"visible", Prop("boolean", "True to show, false to hide.")},
					   {"scene", Prop("string", "Optional scene name; defaults to current.")},
					   {"canvas", Prop("string", "Optional canvas uuid; omit for Default.")}},
				      json::array({"id", "visible"})))},

		{"get_item_transform", "sceneItems.getTransform",
		 MakeDescriptor(
			 "get_item_transform", "Read a scene item's full geometry (position, scale, rotation, crop).",
			 SchemaObject(json{{"id", Prop("integer", "Scene-item id (from list_scene_items).")},
					   {"scene", Prop("string", "Optional scene name; defaults to current.")},
					   {"canvas", Prop("string", "Optional canvas uuid; omit for Default.")}},
				      json::array({"id"})))},

		{"set_item_transform", "sceneItems.setTransform",
		 MakeDescriptor(
			 "set_item_transform",
			 "Apply a partial transform to a scene item (send only the fields that change).",
			 SchemaObject(json{{"id", Prop("integer", "Scene-item id (from list_scene_items).")},
					   {"transform",
					    json{{"type", "object"},
						 {"description", "Partial geometry: pos {x,y}, scale {x,y}, "
								 "rotation, crop {left,top,right,bottom}, etc."}}},
					   {"scene", Prop("string", "Optional scene name; defaults to current.")},
					   {"canvas", Prop("string", "Optional canvas uuid; omit for Default.")}},
				      json::array({"id", "transform"})))},

		{"create_source", "sources.create",
		 MakeDescriptor(
			 "create_source", "Create a new source of the given type and add it to a scene.",
			 SchemaObject(json{{"type",
					    Prop("string", "Source kind id, e.g. 'color_source' or 'browser_source'.")},
					   {"name", Prop("string",
							 "Optional source name; defaults to the type's display name.")},
					   {"scene", Prop("string", "Optional scene name; defaults to current.")},
					   {"canvas", Prop("string", "Optional canvas uuid; omit for Default.")}},
				      json::array({"type"})))},

		{"rename_source", "sources.rename",
		 MakeDescriptor(
			 "rename_source", "Rename the source backing a scene item.",
			 SchemaObject(json{{"id", Prop("integer", "Scene-item id (from list_scene_items).")},
					   {"name", Prop("string", "New source name.")},
					   {"scene", Prop("string", "Optional scene name; defaults to current.")},
					   {"canvas", Prop("string", "Optional canvas uuid; omit for Default.")}},
				      json::array({"id", "name"})))},

		{"remove_source", "sceneItems.remove",
		 MakeDescriptor(
			 "remove_source", "Remove a scene item (the source) from its scene by numeric id.",
			 SchemaObject(json{{"id", Prop("integer", "Scene-item id (from list_scene_items).")},
					   {"scene", Prop("string", "Optional scene name; defaults to current.")},
					   {"canvas", Prop("string", "Optional canvas uuid; omit for Default.")}},
				      json::array({"id"})))},

		{"get_current_transition", "transitions.getCurrent",
		 MakeDescriptor("get_current_transition", "Get the active scene transition type, name, and duration.",
				SchemaObject(json::object(), json::array()))},

		{"set_transition", "transitions.setCurrent",
		 MakeDescriptor(
			 "set_transition", "Set the active scene transition by its type id.",
			 SchemaObject(json{{"id", Prop("string", "Transition type id (from transitionTypes.list).")}},
				      json::array({"id"})))},

		{"list_canvases", "canvas.list",
		 MakeDescriptor("list_canvases", "List all canvases (encode targets) with their resolution and FPS.",
				SchemaObject(json::object(), json::array()))},

		{"list_stream_profiles", "streamProfile.list",
		 MakeDescriptor("list_stream_profiles",
				"List the saved stream destination profiles (platform + credential).",
				SchemaObject(json::object(), json::array()))},

		{"list_outputs", "outputBinding.list",
		 MakeDescriptor("list_outputs",
				"List the output bindings (stream-profile x canvas pairings) and their enabled state.",
				SchemaObject(json::object(), json::array()))},

		{"multistream_status", "multistream.status",
		 MakeDescriptor("multistream_status",
				"Get live status of every enabled output binding (idle/connecting/live/error).",
				SchemaObject(json::object(), json::array()))},

		{"start_output", "multistream.startOutput",
		 MakeDescriptor("start_output", "Start streaming one output binding by its uuid (go-live).",
				SchemaObject(json{{"uuid", Prop("string", "Output binding uuid (from list_outputs).")}},
					     json::array({"uuid"})))},

		{"stop_output", "multistream.stopOutput",
		 MakeDescriptor("stop_output", "Stop streaming one output binding by its uuid.",
				SchemaObject(json{{"uuid", Prop("string", "Output binding uuid (from list_outputs).")}},
					     json::array({"uuid"})))},

		{"get_stats", "stats.get",
		 MakeDescriptor(
			 "get_stats",
			 "Get a performance + per-output streaming stats snapshot (fps, cpu, bitrate, dropped frames).",
			 SchemaObject(json::object(), json::array()))},
	};
	return kTools;
}

std::string ServerVersion()
{
	const char *v = obs_get_version_string();
	return v ? std::string(v) : std::string("0.0.0");
}

// Constant-time-ish compare: walk both fully, accumulate the diff.
bool TokensEqual(const std::string &a, const std::string &b)
{
	if (a.size() != b.size()) {
		// Length itself is not secret here; still avoid an early-return on content.
		return false;
	}
	unsigned char diff = 0;
	for (size_t i = 0; i < a.size(); ++i) {
		diff |= (unsigned char)(a[i] ^ b[i]);
	}
	return diff == 0;
}

std::string GenerateToken()
{
	static const char *kHex = "0123456789abcdef";
	std::string token;
	token.reserve(32);
	for (int i = 0; i < 32; ++i) {
		unsigned int r = 0;
		if (rand_s(&r) != 0) {
			r = (unsigned int)i * 2654435761u; // fallback; should never hit
		}
		token.push_back(kHex[r & 0xF]);
	}
	return token;
}

// Build a JSON-RPC success envelope.
McpServer::json RpcResult(const McpServer::json &id, const McpServer::json &result)
{
	return McpServer::json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

// Build a JSON-RPC error envelope.
McpServer::json RpcError(const McpServer::json &id, int code, const std::string &message)
{
	return McpServer::json{{"jsonrpc", "2.0"},
			       {"id", id},
			       {"error", McpServer::json{{"code", code}, {"message", message}}}};
}

// A tools/call content result (text block).
McpServer::json ToolText(const std::string &text, bool isError)
{
	return McpServer::json{{"content", McpServer::json::array({McpServer::json{{"type", "text"}, {"text", text}}})},
			       {"isError", isError}};
}

Mcp::HttpResponse JsonResponse(int status, const McpServer::json &body)
{
	Mcp::HttpResponse resp;
	resp.status = status;
	resp.body = body.dump();
	return resp;
}

} // namespace

McpServer::McpServer()
{
	Load();
}

McpServer::~McpServer()
{
	Stop();
}

void McpServer::Load()
{
	const std::string path = MultistreamBasicPath("mcp.json");
	OBSDataAutoRelease root = path.empty() ? nullptr : obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!root) {
		// First run / unreadable: keep the struct defaults, generate a token, save.
		config_ = Config{};
		config_.token = GenerateToken();
		Save();
		return;
	}

	obs_data_set_default_bool(root, "enabled", false);
	obs_data_set_default_int(root, "port", 47800);
	obs_data_set_default_bool(root, "allowMutations", true);
	obs_data_set_default_bool(root, "allowGoLive", false);

	config_.enabled = obs_data_get_bool(root, "enabled");
	config_.port = (int)obs_data_get_int(root, "port");
	config_.token = obs_data_get_string(root, "token");
	config_.allowMutations = obs_data_get_bool(root, "allowMutations");
	config_.allowGoLive = obs_data_get_bool(root, "allowGoLive");

	if (config_.token.empty()) {
		config_.token = GenerateToken();
		Save();
	}
}

void McpServer::Save() const
{
	const std::string path = MultistreamBasicPath("mcp.json");
	if (path.empty()) {
		return;
	}

	OBSDataAutoRelease root = obs_data_create();
	obs_data_set_bool(root, "enabled", config_.enabled);
	obs_data_set_int(root, "port", config_.port);
	obs_data_set_string(root, "token", config_.token.c_str());
	obs_data_set_bool(root, "allowMutations", config_.allowMutations);
	obs_data_set_bool(root, "allowGoLive", config_.allowGoLive);
	ReportSaveResult(SaveJsonAtomic(root, path), path);
}

void McpServer::Start()
{
	shutdown_.store(false);
	if (!config_.enabled) {
		HostLog("[mcp] server disabled (mcp.json enabled=false); not listening");
		return;
	}
	const bool ok =
		httpServer_.Start(config_.port, [this](const Mcp::HttpRequest &req) { return HandleRequest(req); });
	if (ok) {
		HostLog("[mcp] server listening on http://127.0.0.1:" + std::to_string(config_.port) +
			"/mcp (token required)");
	} else {
		HostLog("[mcp] server failed to listen: " + httpServer_.LastError());
	}
}

void McpServer::Stop()
{
	shutdown_.store(true);
	httpServer_.Stop();
}

bool McpServer::StartForTest(int port, const std::string &token, bool allowMutations, bool allowGoLive)
{
	shutdown_.store(false);
	config_.enabled = true;
	config_.port = port;
	config_.token = token;
	config_.allowMutations = allowMutations;
	config_.allowGoLive = allowGoLive;
	// Deliberately no Save(): must not touch the user's mcp.json.
	httpServer_.Start(port, [this](const Mcp::HttpRequest &req) { return HandleRequest(req); });
	return httpServer_.IsListening();
}

McpServer::ConfigView McpServer::ViewLocked() const
{
	ConfigView v;
	v.enabled = config_.enabled;
	v.port = config_.port;
	v.token = config_.token;
	v.allowMutations = config_.allowMutations;
	v.allowGoLive = config_.allowGoLive;
	v.listening = httpServer_.IsListening();
	v.lastError = httpServer_.LastError();
	v.endpoint = "http://127.0.0.1:" + std::to_string(config_.port) + "/mcp";
	return v;
}

McpServer::ConfigView McpServer::GetConfigView() const
{
	std::lock_guard<std::mutex> lock(configMutex_);
	return ViewLocked();
}

void McpServer::RestartListener()
{
	// Stop joins the accept thread (we must NOT hold configMutex_ here; HandleRequest
	// takes it on that thread). After stopping, re-read enabled/port under the lock.
	httpServer_.Stop();

	bool enabled = false;
	int port = 0;
	{
		std::lock_guard<std::mutex> lock(configMutex_);
		enabled = config_.enabled;
		port = config_.port;
	}
	if (!enabled) {
		HostLog("[mcp] server disabled; listener stopped");
		return;
	}
	shutdown_.store(false);
	const bool ok = httpServer_.Start(port, [this](const Mcp::HttpRequest &req) { return HandleRequest(req); });
	if (ok) {
		HostLog("[mcp] server listening on http://127.0.0.1:" + std::to_string(port) + "/mcp (token required)");
	} else {
		HostLog("[mcp] server failed to listen: " + httpServer_.LastError());
	}
}

McpServer::ConfigView McpServer::ApplyConfigPatch(const ConfigPatch &patch)
{
	bool restart = false;
	{
		std::lock_guard<std::mutex> lock(configMutex_);
		if (patch.hasEnabled && patch.enabled != config_.enabled) {
			config_.enabled = patch.enabled;
			restart = true;
		}
		if (patch.hasPort) {
			// Validate the range; ignore an out-of-range port (keeps the old one).
			if (patch.port >= 1024 && patch.port <= 65535) {
				if (patch.port != config_.port) {
					config_.port = patch.port;
					restart = true;
				}
			} else {
				HostLog("[mcp] setConfig: ignoring out-of-range port " + std::to_string(patch.port));
			}
		}
		if (patch.hasAllowMutations) {
			config_.allowMutations = patch.allowMutations;
		}
		if (patch.hasAllowGoLive) {
			config_.allowGoLive = patch.allowGoLive;
		}
		Save();
	}

	if (restart) {
		RestartListener();
	}

	return GetConfigView();
}

std::string McpServer::RegenerateToken()
{
	std::lock_guard<std::mutex> lock(configMutex_);
	config_.token = GenerateToken();
	Save();
	return config_.token;
}

bool McpServer::RunBridge(const std::string &method, const json &params, json &result, std::string &error) const
{
	// On the UI thread already (the in-process self-test): call directly. Posting
	// and blocking here would deadlock -- the task can only run on this thread.
	if (CefCurrentlyOn(TID_UI)) {
		return Bridge::Dispatch(method, params, result, error);
	}

	struct DispatchResult {
		bool ok = false;
		json result;
		std::string error;
	};

	// shared_ptr so the promise outlives both sides regardless of timeout/teardown.
	auto prom = std::make_shared<std::promise<DispatchResult>>();
	std::future<DispatchResult> fut = prom->get_future();

	// Capture method/params by value; the promise by shared_ptr value.
	CefPostTask(TID_UI, base::BindOnce(
				    [](std::shared_ptr<std::promise<DispatchResult>> p, std::string m, json ps) {
					    DispatchResult dr;
					    McpServer *inst = Mcp::Instance();
					    if (!inst || inst->IsShuttingDown()) {
						    dr.ok = false;
						    dr.error = "server shutting down";
						    p->set_value(std::move(dr));
						    return;
					    }
					    dr.ok = Bridge::Dispatch(m, ps, dr.result, dr.error);
					    p->set_value(std::move(dr));
				    },
				    prom, method, params));

	if (fut.wait_for(std::chrono::seconds(kBridgeTimeoutSeconds)) != std::future_status::ready) {
		error = "timed out";
		return false;
	}
	DispatchResult dr = fut.get();
	result = std::move(dr.result);
	error = std::move(dr.error);
	return dr.ok;
}

McpServer::json McpServer::BuildToolsList() const
{
	json tools = json::array();
	for (const auto &tool : ToolRegistry()) {
		tools.push_back(tool.descriptor);
	}
	return json{{"tools", tools}};
}

McpServer::json McpServer::GateAndRun(const std::string &method, const json &callParams) const
{
	// The mcp.* bridge namespace configures THIS server (enable, port, token,
	// allowMutations/allowGoLive). Reaching it from the network endpoint the server
	// itself authorizes would let a client escalate its own capabilities (e.g.
	// obs_call mcp.setConfig {allowGoLive:true}), rotate the token, or stop the
	// server. Those methods are reachable only from the in-process Settings UI; deny
	// them here at the single chokepoint every tool (obs_call + curated) passes
	// through.
	if (method.rfind("mcp.", 0) == 0) {
		return ToolText("method 'mcp.*' is not callable from MCP", true);
	}

	// Classify + enforce capability before executing. Read the flags under the lock
	// (the Settings UI may flip them on the UI thread while we run on the HTTP thread).
	const Capability cap = Classify(method);
	bool allowed = true;
	{
		std::lock_guard<std::mutex> lock(configMutex_);
		if (cap == Capability::Mutate) {
			allowed = config_.allowMutations;
		} else if (cap == Capability::GoLive) {
			allowed = config_.allowGoLive;
		}
	}
	if (!allowed) {
		return ToolText(std::string("capability '") + CapabilityName(cap) + "' disabled", true);
	}

	json result;
	std::string error;
	if (RunBridge(method, callParams, result, error)) {
		return ToolText(result.dump(), false);
	}
	return ToolText(error, true);
}

McpServer::json McpServer::HandleToolsCall(const json &params) const
{
	const std::string name = params.value("name", std::string());

	// Resolve the tool by name from the registry.
	const ToolDescriptor *found = nullptr;
	for (const auto &tool : ToolRegistry()) {
		if (name == tool.name) {
			found = &tool;
			break;
		}
	}
	if (!found) {
		// Signaled via a sentinel the caller turns into a JSON-RPC -32602 error.
		return json{{"__rpcError", json{{"code", -32602}, {"message", "unknown tool: " + name}}}};
	}

	const json arguments = params.contains("arguments") && params["arguments"].is_object() ? params["arguments"]
											       : json::object();

	// obs_call (empty bridgeMethod): the method + params come from the arguments.
	if (found->bridgeMethod[0] == '\0') {
		if (!arguments.contains("method") || !arguments["method"].is_string()) {
			return json{{"__rpcError",
				     json{{"code", -32602}, {"message", "obs_call requires arguments.method"}}}};
		}
		const std::string method = arguments["method"].get<std::string>();
		const json callParams = arguments.contains("params") && arguments["params"].is_object()
						? arguments["params"]
						: json::object();
		return GateAndRun(method, callParams);
	}

	// Curated tool: map to its bridge method, pass arguments straight through as the
	// bridge params, and run through the same gate + UI-thread marshal as obs_call.
	return GateAndRun(found->bridgeMethod, arguments);
}

Mcp::HttpResponse McpServer::HandleRequest(const Mcp::HttpRequest &req)
{
	// Auth first: require "Authorization: Bearer <token>" matching the current token.
	// Snapshot the token under the lock (the Settings UI may regenerate it live).
	const std::string prefix = "Bearer ";
	std::string presented;
	if (req.authorization.size() >= prefix.size() && req.authorization.compare(0, prefix.size(), prefix) == 0) {
		presented = req.authorization.substr(prefix.size());
	}
	std::string expectedToken;
	{
		std::lock_guard<std::mutex> lock(configMutex_);
		expectedToken = config_.token;
	}
	if (expectedToken.empty() || !TokensEqual(presented, expectedToken)) {
		return JsonResponse(401, json{{"error", "unauthorized"}});
	}

	// Routing: only POST /mcp.
	if (req.path != "/mcp") {
		return JsonResponse(404, json{{"error", "not found"}});
	}

	// Parse the JSON-RPC body.
	json rpc;
	try {
		rpc = json::parse(req.body);
	} catch (...) {
		return JsonResponse(200, RpcError(json(nullptr), -32700, "parse error"));
	}
	if (!rpc.is_object()) {
		return JsonResponse(200, RpcError(json(nullptr), -32600, "invalid request"));
	}

	const json id = rpc.contains("id") ? rpc["id"] : json(nullptr);
	const std::string method = rpc.value("method", std::string());
	const json params = rpc.contains("params") && rpc["params"].is_object() ? rpc["params"] : json::object();

	if (method == "initialize") {
		json result = json{{"protocolVersion", "2024-11-05"},
				   {"capabilities", json{{"tools", json::object()}}},
				   {"serverInfo", json{{"name", "braidcast"}, {"version", ServerVersion()}}}};
		return JsonResponse(200, RpcResult(id, result));
	}
	if (method == "notifications/initialized") {
		// A notification (no id): acknowledge with 202, no JSON-RPC body.
		Mcp::HttpResponse resp;
		resp.status = 202;
		resp.body.clear();
		return resp;
	}
	if (method == "ping") {
		return JsonResponse(200, RpcResult(id, json::object()));
	}
	if (method == "tools/list") {
		return JsonResponse(200, RpcResult(id, BuildToolsList()));
	}
	if (method == "tools/call") {
		json toolResult = HandleToolsCall(params);
		if (toolResult.is_object() && toolResult.contains("__rpcError")) {
			const json &err = toolResult["__rpcError"];
			return JsonResponse(200, RpcError(id, err.value("code", -32602),
							  err.value("message", std::string("invalid params"))));
		}
		return JsonResponse(200, RpcResult(id, toolResult));
	}

	return JsonResponse(200, RpcError(id, -32601, "method not found: " + method));
}

namespace Mcp {

void SetInstance(McpServer *server)
{
	g_instance = server;
}

McpServer *Instance()
{
	return g_instance;
}

} // namespace Mcp
