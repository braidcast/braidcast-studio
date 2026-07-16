#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_WS_CLIENT_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_WS_CLIENT_HPP_

#include <chrono>
#include <functional>
#include <string>

// A reusable libcurl-WebSocket client (Phase 9.0), the shared transport the
// Twitch (IRC-over-WS) and Kick (Pusher) chat transports sit on. Backed by the
// vendored libcurl 8.x WebSocket API (CURLOPT_CONNECT_ONLY=2 + curl_ws_recv /
// curl_ws_send); TLS is handled by libcurl's bundled backend, no extra wiring.
//
// Single-threaded by contract: connect / recv / sendText / close all run on ONE
// worker thread. To interrupt a blocking recv from another thread, flip a flag
// the calling loop checks -- recv polls the socket with a short timeout and
// returns within ~250ms so the loop can re-check its cancel flag promptly. curl
// is kept out of this header (the handle is an opaque void*) so callers don't
// pull <curl/curl.h> (and its winsock2.h ordering constraint).
namespace Chat {

class WsClient {
public:
	WsClient() = default;
	~WsClient();
	WsClient(const WsClient &) = delete;
	WsClient &operator=(const WsClient &) = delete;

	// Transfer `other`'s live connection into this client, closing any prior
	// connection this side held. Lets a worker run the blocking connect() on a
	// stack-local client outside its mutex, then hand the socket to the shared,
	// mutex-guarded instance in O(1) under the lock.
	WsClient &operator=(WsClient &&other) noexcept;

	// Global libcurl init plus a one-time CURL_VERSION_WEBSOCKETS feature check
	// that HostLogs a clear diagnostic when the linked libcurl lacks WebSocket
	// support, so a future dep swap fails loudly rather than silently. Idempotent;
	// called by connect() and once at startup from Bridge::Init.
	static void EnsureInit();

	// Whether the linked libcurl reports WebSocket support (runs EnsureInit).
	static bool WebSocketsSupported();

	// Open `url` (ws:// or wss://) via the WebSocket upgrade handshake. Closes any
	// prior connection first. false + `err` on failure.
	bool connect(const std::string &url, std::string &err);

	// Send `text` as one WebSocket text frame. false on failure.
	bool sendText(const std::string &text);

	// Receive one complete WebSocket frame, polling the socket with a short
	// timeout so the caller can check a cancel flag between calls:
	//   - true with a non-empty `outFrame` when a full data frame arrived
	//     (`isText` distinguishes text vs binary);
	//   - true with an EMPTY `outFrame` when the poll timed out, a PING was
	//     auto-answered with a PONG, or only a partial chunk was consumed (the
	//     caller should loop and re-check cancellation);
	//   - false with `err` set when the peer closed the connection or a transport
	//     error occurred (the caller should reconnect or stop).
	bool recv(std::string &outFrame, bool &isText, std::string &err);

	// Close the connection and free the curl handle. Safe to call repeatedly. Must
	// run on the same worker thread that called recv (curl easy handles are not
	// safe for concurrent use).
	void close();

	bool connected() const { return easy_ != nullptr; }

private:
	void *easy_ = nullptr;   // CURL* (opaque to keep curl out of the header)
	long long sock_ = -1;    // curl_socket_t cached from CURLINFO_ACTIVESOCKET
	std::string accum_;      // reassembly buffer for a chunked (large) frame
	bool accumText_ = false; // whether the in-progress frame is text
};

// Exponential backoff with a cap, for transports reconnecting after a drop.
class Backoff {
public:
	explicit Backoff(std::chrono::milliseconds base = std::chrono::milliseconds(1000),
			 std::chrono::milliseconds cap = std::chrono::milliseconds(30000))
		: base_(base),
		  cap_(cap)
	{
	}

	// The next delay (doubling each call, clamped to the cap) then advance.
	std::chrono::milliseconds next();

	// Reset to the base delay after a successful (re)connect.
	void reset() { attempt_ = 0; }

private:
	std::chrono::milliseconds base_;
	std::chrono::milliseconds cap_;
	int attempt_ = 0;
};

// Sleep `total` in small slices, returning early (true) as soon as canceled()
// turns true, else false once the full duration elapsed. Lets a transport wait
// out a backoff delay while still honoring a prompt cancel.
bool CancelableSleep(std::chrono::milliseconds total, const std::function<bool()> &canceled);

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_WS_CLIENT_HPP_
