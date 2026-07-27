#ifndef OBS_MULTISTREAM_FRONTEND_MCP_MCPSERVER_HPP_
#define OBS_MULTISTREAM_FRONTEND_MCP_MCPSERVER_HPP_

#include <atomic>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "mcp/HttpServer.hpp"

// The embedded MCP (Model Context Protocol) server. Hosts a localhost HTTP/1.1
// endpoint (POST /mcp) speaking the JSON-RPC 2.0 subset MCP uses (initialize,
// notifications/initialized, ping, tools/list, tools/call) and exposes a single
// generic `obs_call` tool that proxies any registered bridge method through
// Bridge::Dispatch.
//
// Threading: HandleRequest runs on the HttpServer accept thread (or, for the
// in-process self-test, on the CEF UI thread). Bridge::Dispatch must run on the
// CEF UI thread; HandleRequest marshals to TID_UI with a promise/future + a 15s
// timeout, EXCEPT when already on TID_UI (the self-test), where it calls Dispatch
// directly to avoid deadlocking the UI thread on a task only it can service.
//
// Disabled by default (mcp.json `enabled=false`), so nothing listens unless the
// user opts in.

class McpServer {
public:
	using json = nlohmann::json;

	McpServer();
	~McpServer();

	McpServer(const McpServer &) = delete;
	McpServer &operator=(const McpServer &) = delete;

	// Start listening if config_.enabled; no-op (logged) otherwise.
	void Start();

	// Set the shutdown flag (so any pending/blocked marshalled call bails) then
	// stop the HTTP server (joins the accept thread). Idempotent.
	void Stop();

	// Test-only: start on an explicit port/token/config WITHOUT touching mcp.json
	// (does not Save). Returns whether the socket is listening. The self-test does
	// not need the socket -- it drives HandleRequest in-process -- but this also
	// sets the config the in-process path reads.
	bool StartForTest(int port, const std::string &token, bool allowMutations, bool allowGoLive);

	// The core request handler, public so the self-test can invoke it in-process
	// (avoids socket timing flakiness). The HttpServer lambda forwards to this.
	Mcp::HttpResponse HandleRequest(const Mcp::HttpRequest &req);

	// Config snapshot handed to the Settings UI (bridge `mcp.getConfig`). Mirrors the
	// frontend's McpConfig type. `listening`/`lastError`/`endpoint` are runtime, the
	// rest are the persisted config.
	struct ConfigView {
		bool enabled = false;
		int port = 47800;
		std::string token;
		bool allowMutations = true;
		bool allowGoLive = false;
		bool listening = false;
		std::string lastError;
		std::string endpoint;
	};

	// Fields the Settings UI may change via `mcp.setConfig`. A nullopt-style "unset"
	// is encoded by the has* flags so partial updates (only changed fields) work.
	struct ConfigPatch {
		bool hasEnabled = false;
		bool enabled = false;
		bool hasPort = false;
		int port = 0;
		bool hasAllowMutations = false;
		bool allowMutations = false;
		bool hasAllowGoLive = false;
		bool allowGoLive = false;
	};

	// Snapshot the current config + runtime state for the Settings UI. Thread-safe.
	ConfigView GetConfigView() const;

	// Apply a partial config change (Settings UI). Persists mcp.json and, if
	// enabled/port changed, restarts the listener (Stop then Start when enabled,
	// Stop when disabled). Validates the port range; on a bind failure leaves
	// listening=false + lastError set. Returns the resulting view. UI-thread only
	// (it may join the accept thread); thread-safe wrt the running HTTP thread.
	ConfigView ApplyConfigPatch(const ConfigPatch &patch);

	// Generate + persist a new token; the new token applies to subsequent requests.
	// Returns it. Thread-safe (swaps the in-memory token under the config mutex).
	std::string RegenerateToken();

	bool IsListening() const { return httpServer_.IsListening(); }

	// True once Stop() has been requested. Read by the posted UI task so it bails
	// instead of touching the bridge during teardown.
	bool IsShuttingDown() const { return shutdown_.load(); }

private:
	struct Config {
		bool enabled = false;
		int port = 47800;
		std::string token;
		bool allowMutations = true;
		bool allowGoLive = false;
	};

	void Load();
	void Save() const;

	// Build the runtime view from the current config (caller holds configMutex_).
	ConfigView ViewLocked() const;
	// Restart the HTTP listener to match config_.enabled/port (caller holds NO lock;
	// joins the accept thread). Used by ApplyConfigPatch.
	void RestartListener();

	// JSON-RPC dispatch helpers (all run wherever HandleRequest runs).
	json BuildToolsList() const;
	json HandleToolsCall(const json &params) const;

	// Capability-gate then run a bridge method (shared by obs_call + curated tools).
	json GateAndRun(const std::string &method, const json &callParams) const;

	// Run a bridge method, marshalling to TID_UI when needed (see threading note).
	// Fills ok/result/error; returns false on timeout/shutdown (error set).
	bool RunBridge(const std::string &method, const json &params, json &result, std::string &error) const;

	// Guards config_ (token/flags/port/enabled). Read on the HTTP accept thread per
	// request (snapshotted at the top of HandleRequest), written on the UI thread by
	// the Settings setters. mutable so const read paths can lock it.
	mutable std::mutex configMutex_;
	Config config_;
	Mcp::HttpServer httpServer_;
	std::atomic<bool> shutdown_{false};
};

// Process-wide accessor to the single live McpServer (mirrors Preview/Projector).
// Set by ObsBootstrap::Start after construction, cleared in Stop before reset.
namespace Mcp {
void SetInstance(McpServer *server);
McpServer *Instance();
} // namespace Mcp

#endif // OBS_MULTISTREAM_FRONTEND_MCP_MCPSERVER_HPP_
