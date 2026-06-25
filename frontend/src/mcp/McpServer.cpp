#define _CRT_RAND_S

#include "mcp/McpServer.hpp"

#include <obs.h>
#include <obs.hpp>          // OBSDataAutoRelease
#include <util/platform.h>  // os_mkdirs

#include <array>
#include <chrono>
#include <cstdlib> // rand_s
#include <filesystem>
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

// The tools/list registry: a data list so curated tools slot in later. Stage 1 is
// just obs_call.
struct ToolDescriptor {
	const char *name;
	McpServer::json descriptor;
};

const std::vector<ToolDescriptor> &ToolRegistry()
{
	using json = McpServer::json;
	static const std::vector<ToolDescriptor> kTools = {
		{"obs_call",
		 json{{"name", "obs_call"},
		      {"description", "Call any OBS bridge method by name with params."},
		      {"inputSchema",
		       json{{"type", "object"},
			    {"properties",
			     json{{"method", json{{"type", "string"}}}, {"params", json{{"type", "object"}}}}},
			    {"required", json::array({"method"})}}}}},
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
	std::filesystem::path dir = std::filesystem::u8path(path).parent_path();
	os_mkdirs(dir.u8string().c_str());

	OBSDataAutoRelease root = obs_data_create();
	obs_data_set_bool(root, "enabled", config_.enabled);
	obs_data_set_int(root, "port", config_.port);
	obs_data_set_string(root, "token", config_.token.c_str());
	obs_data_set_bool(root, "allowMutations", config_.allowMutations);
	obs_data_set_bool(root, "allowGoLive", config_.allowGoLive);
	obs_data_save_json_pretty_safe(root, path.c_str(), "tmp", "bak");
}

void McpServer::Start()
{
	shutdown_.store(false);
	if (!config_.enabled) {
		HostLog("[mcp] server disabled (mcp.json enabled=false); not listening");
		return;
	}
	const bool ok = httpServer_.Start(config_.port, [this](const Mcp::HttpRequest &req) { return HandleRequest(req); });
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

void McpServer::SetEnabled(bool v)
{
	config_.enabled = v;
	Save();
}

void McpServer::SetPort(int v)
{
	config_.port = v;
	Save();
}

void McpServer::RegenerateToken()
{
	config_.token = GenerateToken();
	Save();
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

	if (name == "obs_call") {
		if (!arguments.contains("method") || !arguments["method"].is_string()) {
			return json{
				{"__rpcError", json{{"code", -32602}, {"message", "obs_call requires arguments.method"}}}};
		}
		const std::string method = arguments["method"].get<std::string>();
		const json callParams = arguments.contains("params") && arguments["params"].is_object()
						? arguments["params"]
						: json::object();

		// Classify + enforce capability before executing.
		const Capability cap = Classify(method);
		bool allowed = true;
		if (cap == Capability::Mutate) {
			allowed = config_.allowMutations;
		} else if (cap == Capability::GoLive) {
			allowed = config_.allowGoLive;
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

	return json{{"__rpcError", json{{"code", -32602}, {"message", "unhandled tool: " + name}}}};
}

Mcp::HttpResponse McpServer::HandleRequest(const Mcp::HttpRequest &req)
{
	// Auth first: require "Authorization: Bearer <token>" matching config_.token.
	const std::string prefix = "Bearer ";
	std::string presented;
	if (req.authorization.size() >= prefix.size() && req.authorization.compare(0, prefix.size(), prefix) == 0) {
		presented = req.authorization.substr(prefix.size());
	}
	if (config_.token.empty() || !TokensEqual(presented, config_.token)) {
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
				   {"serverInfo", json{{"name", "obs-multistreamer"}, {"version", ServerVersion()}}}};
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
