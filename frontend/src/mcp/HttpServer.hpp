#ifndef OBS_MULTISTREAM_FRONTEND_MCP_HTTPSERVER_HPP_
#define OBS_MULTISTREAM_FRONTEND_MCP_HTTPSERVER_HPP_

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// A minimal single-client HTTP/1.1 server bound to 127.0.0.1 only, used to host
// the embedded MCP endpoint. Deliberately libobs-free (no obs.h) so it stays out
// of the /wd4201 set; logging goes through HostLog.
//
// The accept loop runs on its own thread and handles one connection at a time
// (accept -> read -> handler -> write -> close). For the MVP a single in-flight
// request is acceptable: MCP clients are sequential. Stop() unblocks accept by
// closing the listen socket, unblocks a parked recv by shutdown()ing the current
// client socket, and joins the thread -- so Stop() can never hang on a client that
// connected but never sent data.

namespace Mcp {

struct HttpRequest {
	std::string method;
	std::string path;
	std::string authorization;
	std::string body;
};

struct HttpResponse {
	int status = 200;
	std::string contentType = "application/json";
	std::string body;
};

using HttpHandler = std::function<HttpResponse(const HttpRequest &)>;

class HttpServer {
public:
	HttpServer() = default;
	~HttpServer();

	HttpServer(const HttpServer &) = delete;
	HttpServer &operator=(const HttpServer &) = delete;

	// Bind to 127.0.0.1:port and spawn the accept thread. Returns false (and logs
	// + records LastError) on any failure (e.g. port in use) without throwing or
	// crashing; the server is left stopped. The handler is invoked on the accept
	// thread for each request.
	bool Start(int port, HttpHandler handler);

	// Stop the accept thread (closes the listen socket to unblock accept, shuts down
	// any parked client socket, joins), and balance the WSAStartup. Idempotent.
	void Stop();

	bool IsListening() const { return running_.load(); }
	std::string LastError() const { return lastError_; }

private:
	void AcceptLoop();
	void HandleConnection(uintptr_t clientSocket);

	std::atomic<bool> running_{false};
	std::thread acceptThread_;
	uintptr_t listenSocket_ = ~uintptr_t(0); // INVALID_SOCKET, stored type-erased
	bool wsaUp_ = false;
	HttpHandler handler_;
	std::string lastError_;

	// The one client socket HandleConnection may currently be parked in recv() on (this
	// server handles a single connection at a time). Stop() shutdown()s it so a parked
	// recv unblocks immediately instead of waiting out kHeaderRecvTimeoutMs.
	std::mutex clientMutex_;
	uintptr_t clientSocket_ = ~uintptr_t(0);
};

} // namespace Mcp

#endif // OBS_MULTISTREAM_FRONTEND_MCP_HTTPSERVER_HPP_
