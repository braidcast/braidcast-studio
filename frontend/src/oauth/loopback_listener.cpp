// Winsock headers must precede <windows.h>.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "loopback_listener.hpp"

#include <chrono>
#include <map>
#include <string>
#include <utility>

#include "../log.hpp"

namespace OAuth {

namespace {

// ---- Winsock RAII -----------------------------------------------------------

// Per-call WSAStartup/WSACleanup (refcounted, so it coexists with any other
// Winsock user in-process). Ensures Winsock is torn down on EVERY return path.
struct WsaGuard {
	bool ok = false;
	WsaGuard()
	{
		WSADATA data;
		ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}
	~WsaGuard()
	{
		if (ok) {
			WSACleanup();
		}
	}
	WsaGuard(const WsaGuard &) = delete;
	WsaGuard &operator=(const WsaGuard &) = delete;
};

struct SocketGuard {
	SOCKET s = INVALID_SOCKET;
	~SocketGuard()
	{
		if (s != INVALID_SOCKET) {
			closesocket(s);
		}
	}
	SocketGuard() = default;
	SocketGuard(const SocketGuard &) = delete;
	SocketGuard &operator=(const SocketGuard &) = delete;
};

// Percent-decode an application/x-www-form-urlencoded query component.
std::string UrlDecode(const std::string &in)
{
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size(); ++i) {
		const char c = in[i];
		if (c == '+') {
			out.push_back(' ');
		} else if (c == '%' && i + 2 < in.size()) {
			const auto hex = [](char h) -> int {
				if (h >= '0' && h <= '9') {
					return h - '0';
				}
				if (h >= 'a' && h <= 'f') {
					return h - 'a' + 10;
				}
				if (h >= 'A' && h <= 'F') {
					return h - 'A' + 10;
				}
				return -1;
			};
			const int hi = hex(in[i + 1]);
			const int lo = hex(in[i + 2]);
			if (hi >= 0 && lo >= 0) {
				out.push_back(static_cast<char>((hi << 4) | lo));
				i += 2;
			} else {
				out.push_back(c);
			}
		} else {
			out.push_back(c);
		}
	}
	return out;
}

// Parse "a=b&c=d" into a map, url-decoding keys and values.
std::map<std::string, std::string> ParseQuery(const std::string &query)
{
	std::map<std::string, std::string> out;
	size_t pos = 0;
	while (pos < query.size()) {
		size_t amp = query.find('&', pos);
		if (amp == std::string::npos) {
			amp = query.size();
		}
		const std::string pair = query.substr(pos, amp - pos);
		const size_t eq = pair.find('=');
		if (eq != std::string::npos) {
			out[UrlDecode(pair.substr(0, eq))] = UrlDecode(pair.substr(eq + 1));
		} else if (!pair.empty()) {
			out[UrlDecode(pair)] = std::string();
		}
		pos = amp + 1;
	}
	return out;
}

} // namespace

struct LoopbackListener::Impl {
	WsaGuard wsa;
	SocketGuard listener4;
	SocketGuard listener6;
	SocketGuard conn;
	unsigned port = 0;
	std::map<std::string, std::string> params;
};

LoopbackListener::LoopbackListener() : impl_(std::make_unique<Impl>()) {}

LoopbackListener::~LoopbackListener() = default;

bool LoopbackListener::Bind(std::string &err)
{
	// Bind a 127.0.0.1-only ephemeral listener (never INADDR_ANY/0.0.0.0), then a
	// matching [::1] listener on the SAME port below so both loopback families work.
	if (!impl_->wsa.ok) {
		err = "Winsock init failed";
		return false;
	}

	impl_->listener4.s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (impl_->listener4.s == INVALID_SOCKET) {
		err = "failed to create loopback socket";
		return false;
	}

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only
	addr.sin_port = 0;                             // ephemeral port
	if (bind(impl_->listener4.s, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == SOCKET_ERROR) {
		err = "failed to bind loopback listener";
		return false;
	}
	if (listen(impl_->listener4.s, 1) == SOCKET_ERROR) {
		err = "failed to listen on loopback socket";
		return false;
	}

	sockaddr_in bound{};
	int boundLen = sizeof bound;
	if (getsockname(impl_->listener4.s, reinterpret_cast<sockaddr *>(&bound), &boundLen) == SOCKET_ERROR) {
		err = "failed to resolve loopback port";
		return false;
	}
	impl_->port = ntohs(bound.sin_port);

	// Kick advertises the redirect host as "localhost", which resolves to ::1 before
	// 127.0.0.1 on IPv6-preferring hosts; an IPv4-only listener would then never see
	// the redirect. Bind a second listener on [::1] sharing the SAME ephemeral port
	// (IPV6_V6ONLY so it does not collide with the v4 bind). Best-effort: on any
	// failure, log and proceed IPv4-only rather than failing the whole flow.
	impl_->listener6.s = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	if (impl_->listener6.s == INVALID_SOCKET) {
		HostLog("LoopbackListener: IPv6 loopback socket unavailable; listening IPv4-only");
	} else {
		DWORD v6only = 1;
		setsockopt(impl_->listener6.s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&v6only),
			   sizeof v6only);
		sockaddr_in6 addr6{};
		addr6.sin6_family = AF_INET6;
		addr6.sin6_addr = in6addr_loopback; // ::1 only
		addr6.sin6_port = htons(static_cast<unsigned short>(impl_->port));
		if (bind(impl_->listener6.s, reinterpret_cast<sockaddr *>(&addr6), sizeof addr6) == SOCKET_ERROR ||
		    listen(impl_->listener6.s, 1) == SOCKET_ERROR) {
			HostLog("LoopbackListener: IPv6 loopback bind/listen failed; listening IPv4-only");
			closesocket(impl_->listener6.s);
			impl_->listener6.s = INVALID_SOCKET;
		}
	}

	return true;
}

unsigned LoopbackListener::Port() const
{
	return impl_->port;
}

bool LoopbackListener::Await(const std::function<bool()> &canceled, int timeoutSec, std::string &err)
{
	// Accept the redirect connection. Poll with select() so cancel + an overall
	// deadline are honored without blocking forever in accept(). A browser may open
	// stray preconnect/favicon requests to the loopback port first, so keep accepting
	// until a request actually carries the OAuth code/error (or the deadline lapses)
	// rather than committing to the first connection. The deadline is short so a
	// provider that never redirects (e.g. an unverified-app screen) fails visibly
	// instead of hanging; the UI shows a matching countdown.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
	SocketGuard &conn = impl_->conn;
	for (;;) {
		if (canceled()) {
			return false; // user closed the modal / bridge tore down / superseded
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			err = "No response from the browser -- if the sign-in page showed a "
			      "verification/blocked error, close it and try again.";
			return false;
		}

		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(impl_->listener4.s, &readfds);
		if (impl_->listener6.s != INVALID_SOCKET) {
			FD_SET(impl_->listener6.s, &readfds);
		}
		timeval tv{};
		tv.tv_sec = 0;
		tv.tv_usec = 500 * 1000; // 0.5s; re-check cancel/deadline between waits
		const int sel = select(0, &readfds, nullptr, nullptr, &tv);
		if (sel == SOCKET_ERROR) {
			err = "loopback select failed";
			return false;
		}
		if (sel == 0) {
			continue; // nothing yet
		}
		// Accept from whichever loopback family the browser connected on. sel != 0
		// guarantees one of the sockets in the set is readable.
		const SOCKET ready = FD_ISSET(impl_->listener4.s, &readfds) ? impl_->listener4.s : impl_->listener6.s;
		conn.s = accept(ready, nullptr, nullptr);
		if (conn.s == INVALID_SOCKET) {
			continue; // transient; keep waiting within the deadline
		}

		// Bound the read so a half-open client can't hang the worker.
		DWORD recvTimeoutMs = 5000;
		setsockopt(conn.s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&recvTimeoutMs),
			   sizeof recvTimeoutMs);

		std::string request;
		char buf[2048];
		for (int i = 0; i < 16; ++i) {
			const int n = recv(conn.s, buf, sizeof buf, 0);
			if (n <= 0) {
				break;
			}
			request.append(buf, static_cast<size_t>(n));
			if (request.find("\r\n\r\n") != std::string::npos) {
				break; // headers complete; the request line is all we need
			}
			if (request.size() > 16 * 1024) {
				break;
			}
		}

		// Parse the request line: "GET <target> HTTP/1.1".
		const size_t lineEnd = request.find("\r\n");
		const std::string line = request.substr(0, lineEnd == std::string::npos ? request.size() : lineEnd);
		const size_t sp1 = line.find(' ');
		const size_t sp2 = sp1 == std::string::npos ? std::string::npos : line.find(' ', sp1 + 1);
		std::string target;
		if (sp1 != std::string::npos && sp2 != std::string::npos) {
			target = line.substr(sp1 + 1, sp2 - sp1 - 1);
		}
		const size_t qpos = target.find('?');
		const std::string query = qpos == std::string::npos ? std::string() : target.substr(qpos + 1);
		impl_->params = ParseQuery(query);

		// The real redirect carries code= or error=; anything else (preconnect,
		// favicon, ...) gets a minimal answer and we keep waiting on the listener.
		if (impl_->params.count("code") || impl_->params.count("error")) {
			break;
		}
		const std::string ignore = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
		send(conn.s, ignore.data(), static_cast<int>(ignore.size()), 0);
		shutdown(conn.s, SD_SEND);
		closesocket(conn.s);
		conn.s = INVALID_SOCKET;
	}

	return true;
}

const std::map<std::string, std::string> &LoopbackListener::Params() const
{
	return impl_->params;
}

void LoopbackListener::Respond(bool ok)
{
	// Answer the browser before judging the result so the tab never hangs: a success
	// page only when the grant is actually valid, otherwise a visible failure page.
	std::string bodyHtml;
	if (ok) {
		bodyHtml =
			"<p>Authorization complete \xE2\x80\x94 you can close this window and return to Braidcast.</p>";
	} else {
		bodyHtml =
			"<p>Authorization failed \xE2\x80\x94 close this window and return to Braidcast to try again.</p>";
	}
	const std::string html =
		"<!doctype html><html><head><meta charset=\"utf-8\"><title>Braidcast</title></head>"
		"<body style=\"font-family:system-ui,sans-serif;padding:2rem\">" +
		bodyHtml + "</body></html>";
	const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
				     std::to_string(html.size()) + "\r\nConnection: close\r\n\r\n" + html;
	send(impl_->conn.s, response.data(), static_cast<int>(response.size()), 0);
	shutdown(impl_->conn.s, SD_SEND);
	// listeners + conn close via their SocketGuards when the Impl is destroyed.
}

} // namespace OAuth
