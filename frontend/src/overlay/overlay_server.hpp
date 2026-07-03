#ifndef OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SERVER_HPP_
#define OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SERVER_HPP_

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "../events/event_model.hpp" // Events::NormalizedEvent

namespace Overlay {

// Loopback-only HTTP/1.1 server for overlay widgets. GET routing + static file
// serving + long-lived SSE. 127.0.0.1 only; per-widget token on every route.
// Distinct from mcp/HttpServer (which is POST-only, single-connection).
class OverlayServer {
public:
	OverlayServer() = default;
	~OverlayServer();
	OverlayServer(const OverlayServer &) = delete;
	OverlayServer &operator=(const OverlayServer &) = delete;

	// Bind 127.0.0.1: try the store's persisted port, else scan 43000..43019 for a
	// free one; persist the bound port via Store().SetPort. Spawns the accept thread.
	// Idempotent. Returns false (logged) if no port in range binds.
	bool Start();

	// Test entry point: bind an explicit port (0 => OS-ephemeral) WITHOUT touching
	// the store's persisted port; returns the actually-bound port via *boundPort.
	bool StartForTest(int port, int *boundPort);

	// Close the listen socket + every SSE socket (unblocking their recv loops), then
	// join all connection threads. Called from Bridge::Shutdown before CEF teardown.
	void Stop();

	bool IsListening() const { return running_.load(); }
	int Port() const { return port_; }
	bool PortChanged() const { return portChanged_; }
	std::string LastError() const { return lastError_; }

	// Push a NormalizedEvent to EVERY open widget socket (the EventHub::Ingest sink).
	void Broadcast(const Events::NormalizedEvent &ev);
	// Push to ONE widget's sockets (overlays.test -- never goes through the store).
	void BroadcastTo(const std::string &widgetId, const Events::NormalizedEvent &ev);
	// Push a chat message to EVERY open widget socket as a named `chat` SSE event
	// (distinct from the default `message` event alert boxes consume). The chat-box
	// widget subscribes to it; alert boxes ignore it.
	void BroadcastChat(const nlohmann::json &chatMsg);

private:
	void AcceptLoop();
	void HandleConnection(uintptr_t clientSocket); // runs on its own thread; closes the socket
	void ServeRuntime(uintptr_t sock, const std::string &path, const std::string &token);
	void ServeWidget(uintptr_t sock, const std::string &path, const std::string &token);
	// Send a prebuilt SSE frame to every open widget socket; shared by Broadcast/BroadcastChat.
	void BroadcastFrame(const std::string &frame);
	void RunSse(uintptr_t sock, const std::string &widgetId); // owns the socket for its lifetime
	void CloseClient(uintptr_t sock); // the OWNING thread's sole close point: erase from clientSockets_ + close
	void ReapFinishedThreads();       // join+erase threads whose done flag is set

	bool BindOn(int port); // low-level bind+listen helper; sets listenSocket_ + port_

	std::atomic<bool> running_{false};
	std::thread acceptThread_;
	uintptr_t listenSocket_ = ~uintptr_t(0);
	bool wsaUp_ = false;
	int port_ = 0;
	bool portChanged_ = false;
	std::string lastError_;

	std::mutex sseMutex_;                                 // guards sockets_ + the fields below
	std::map<std::string, std::set<uintptr_t>> sockets_; // widgetId -> SSE sockets

	// BroadcastFrame snapshots handles then sends OUTSIDE sseMutex_ so one slow client
	// can't stall the others; broadcastDepth_ counts those in-flight sends. While it is
	// non-zero an owning RunSse thread that tears down must NOT closesocket() its fd (a
	// concurrent send could then land on a recycled fd) -- it parks the fd in
	// deferredCloseSse_ instead, and the last broadcast to finish closes them.
	int broadcastDepth_ = 0;
	std::set<uintptr_t> deferredCloseSse_;

	// Every accepted client fd (SSE and plain), so Stop() can shutdown() them all to
	// unblock parked recv/send loops without closing (the owning thread closes). The
	// fd is inserted synchronously in AcceptLoop (before its thread spawns) so a Stop()
	// after the accept thread joins sees every live connection.
	std::mutex clientsMutex_;
	std::set<uintptr_t> clientSockets_;

	struct Conn {
		std::thread thread;
		std::shared_ptr<std::atomic<bool>> done;
	};
	std::mutex threadsMutex_;
	std::vector<Conn> threads_;
};

} // namespace Overlay

#endif // OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_SERVER_HPP_
