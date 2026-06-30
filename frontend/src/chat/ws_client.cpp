#include "ws_client.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

// curl.h pulls <winsock2.h> on Windows; include it BEFORE log.hpp (which pulls
// <windows.h>) so winsock2 wins over the legacy winsock.h windows.h would drag
// in. Also gives us select()/fd_set for the recv poll.
#include <curl/curl.h>

#include "../log.hpp"

// The vendored curl.h (8.12.1-DEV) ships the WebSocket API (websockets.h) but its
// curl_version_info feature-flag list stops at CURL_VERSION_THREADSAFE (1<<30) and
// omits the named CURL_VERSION_WEBSOCKETS macro (1<<31). The runtime libcurl still
// sets bit 31 when built with WebSocket support, so define the macro to curl's
// upstream value if the header lacks it.
#ifndef CURL_VERSION_WEBSOCKETS
#define CURL_VERSION_WEBSOCKETS (1u << 31)
#endif

namespace Chat {

namespace {

// Cached once by EnsureInit: whether the linked libcurl advertises WebSocket
// support. Read by WebSocketsSupported() after EnsureInit has run.
std::atomic<bool> g_wsSupported{false};

// libcurl version_num for 7.86.0, the first release with the curl_ws_* API.
constexpr unsigned int kCurlWsMinVersion = 0x075600;

void DoInit()
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
	const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
	const std::string version = (info && info->version) ? info->version : "?";
	// The WebSocket API (curl_ws_send/recv) exists since libcurl 7.86.0. Some builds
	// export the functions WITHOUT advertising CURL_VERSION_WEBSOCKETS in the feature
	// bitmask (the API was long flagged experimental), so treat a recent-enough
	// version OR the feature bit as supported -- we link the WS symbols directly, so a
	// version this new guarantees they resolve.
	const bool featureBit = info && (info->features & CURL_VERSION_WEBSOCKETS) != 0;
	const bool recentEnough = info && info->version_num >= kCurlWsMinVersion;
	const bool ws = featureBit || recentEnough;
	g_wsSupported.store(ws, std::memory_order_release);
	if (ws) {
		HostLog("[chat] libcurl WebSocket API available (" + version +
			", feature flag: " + (featureBit ? "advertised" : "unadvertised, version >= 7.86") + ")");
	} else {
		HostLog("[chat] WARNING: linked libcurl (" + version +
			") predates the WebSocket API -- Twitch/Kick chat transports cannot connect. "
			"Rebuild against libcurl >= 7.86 built with --enable-websockets.");
	}
}

// 250 ms socket poll: short enough that a recv loop re-checks its cancel flag
// well within the ~0.5s the ChatHub expects for a prompt Stop().
constexpr long kPollMicros = 250 * 1000;

} // namespace

void WsClient::EnsureInit()
{
	static std::once_flag once;
	std::call_once(once, DoInit);
}

bool WsClient::WebSocketsSupported()
{
	EnsureInit();
	return g_wsSupported.load(std::memory_order_acquire);
}

WsClient::~WsClient()
{
	close();
}

bool WsClient::connect(const std::string &url, std::string &err)
{
	EnsureInit();
	close();

	CURL *curl = curl_easy_init();
	if (!curl) {
		err = "curl_easy_init failed";
		return false;
	}

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	// 2 = perform the WebSocket upgrade and leave the socket open for curl_ws_*.
	curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
	// NOSIGNAL is mandatory off the main thread (libcurl's DNS-timeout path uses
	// SIGALRM otherwise, unsafe in a multithreaded process).
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);

	const CURLcode code = curl_easy_perform(curl);
	if (code != CURLE_OK) {
		err = std::string("websocket connect failed: ") + curl_easy_strerror(code);
		curl_easy_cleanup(curl);
		return false;
	}

	curl_socket_t sock = CURL_SOCKET_BAD;
	curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sock);

	easy_ = curl;
	sock_ = (sock == CURL_SOCKET_BAD) ? -1 : static_cast<long long>(sock);
	accum_.clear();
	accumText_ = false;
	return true;
}

bool WsClient::sendText(const std::string &text)
{
	if (!easy_) {
		return false;
	}
	size_t sent = 0;
	const CURLcode code =
		curl_ws_send(static_cast<CURL *>(easy_), text.data(), text.size(), &sent, 0, CURLWS_TEXT);
	return code == CURLE_OK;
}

bool WsClient::recv(std::string &outFrame, bool &isText, std::string &err)
{
	outFrame.clear();
	if (!easy_) {
		err = "not connected";
		return false;
	}
	CURL *curl = static_cast<CURL *>(easy_);

	// Wait briefly for readability so a caller loop can re-check cancellation
	// instead of blocking indefinitely inside curl_ws_recv.
	if (sock_ >= 0) {
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(static_cast<curl_socket_t>(sock_), &rfds);
		timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = kPollMicros;
		// nfds is ignored on Windows; pass 0.
		const int sel = select(0, &rfds, nullptr, nullptr, &tv);
		if (sel == 0) {
			return true; // timeout, no data -- caller loops and re-checks canceled()
		}
		// sel < 0 falls through and lets curl_ws_recv surface the error/again.
	}

	char buf[65536];
	size_t n = 0;
	const struct curl_ws_frame *meta = nullptr;
	const CURLcode code = curl_ws_recv(curl, buf, sizeof(buf), &n, &meta);
	if (code == CURLE_AGAIN) {
		return true; // spurious wakeup, no complete frame yet
	}
	if (code != CURLE_OK) {
		err = std::string("websocket recv failed: ") + curl_easy_strerror(code);
		return false;
	}
	if (!meta) {
		return true;
	}
	if (meta->flags & CURLWS_CLOSE) {
		err = "websocket closed by peer";
		return false;
	}
	if (meta->flags & CURLWS_PING) {
		// Echo the ping payload back as a pong to keep the connection alive.
		size_t sent = 0;
		curl_ws_send(curl, buf, n, &sent, 0, CURLWS_PONG);
		return true;
	}

	// Data frame. curl may deliver a frame larger than `buf` across multiple
	// recv calls (meta->bytesleft > 0 until the last chunk); accumulate until the
	// payload is complete, then hand the whole frame to the caller. The chat
	// protocols in scope (Pusher, Twitch IRC) use small single frames, so this is
	// the only fragmentation that occurs in practice.
	if (accum_.empty()) {
		accumText_ = (meta->flags & CURLWS_TEXT) != 0;
	}
	accum_.append(buf, n);
	if (meta->bytesleft > 0) {
		return true; // more chunks of this frame still to come
	}

	outFrame = std::move(accum_);
	accum_.clear();
	isText = accumText_;
	accumText_ = false;
	return true;
}

void WsClient::close()
{
	if (easy_) {
		curl_easy_cleanup(static_cast<CURL *>(easy_)); // also closes the socket
		easy_ = nullptr;
	}
	sock_ = -1;
	accum_.clear();
	accumText_ = false;
}

std::chrono::milliseconds Backoff::next()
{
	long long ms = base_.count();
	for (int i = 0; i < attempt_ && ms < cap_.count(); ++i) {
		ms *= 2;
	}
	if (ms > cap_.count()) {
		ms = cap_.count();
	}
	++attempt_;
	return std::chrono::milliseconds(ms);
}

bool CancelableSleep(std::chrono::milliseconds total, const std::function<bool()> &canceled)
{
	const auto slice = std::chrono::milliseconds(100);
	std::chrono::milliseconds elapsed{0};
	while (elapsed < total) {
		if (canceled && canceled()) {
			return true;
		}
		const auto step = std::min(slice, total - elapsed);
		std::this_thread::sleep_for(step);
		elapsed += step;
	}
	return canceled && canceled();
}

} // namespace Chat
