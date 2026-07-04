#include "pkce_loopback.hpp"

// Winsock headers must precede <windows.h>.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <optional>
#include <utility>

#include "../http_client.hpp"
#include "../log.hpp"
#include "account_store.hpp"

namespace OAuth {

namespace {

// ---- JSON / form helpers (mirrors device_code.cpp's local set) --------------

std::string JsonStr(const json &j, const char *key)
{
	if (!j.is_object()) {
		return std::string();
	}
	auto it = j.find(key);
	if (it == j.end() || !it->is_string()) {
		return std::string();
	}
	return it->get<std::string>();
}

int JsonInt(const json &j, const char *key, int fallback)
{
	if (!j.is_object()) {
		return fallback;
	}
	auto it = j.find(key);
	if (it == j.end()) {
		return fallback;
	}
	if (it->is_number_integer()) {
		return it->get<int>();
	}
	if (it->is_number()) {
		return static_cast<int>(it->get<double>());
	}
	if (it->is_string()) {
		try {
			return std::stoi(it->get<std::string>());
		} catch (const std::exception &) {
			return fallback;
		}
	}
	return fallback;
}

void AppendForm(std::string &body, const char *key, const std::string &value)
{
	if (!body.empty()) {
		body += "&";
	}
	body += key;
	body += "=";
	body += Http::UrlEncode(value);
}

void AppendQuery(std::string &url, const char *key, const std::string &value)
{
	url += url.find('?') == std::string::npos ? "?" : "&";
	url += key;
	url += "=";
	url += Http::UrlEncode(value);
}

json ParseJson(const std::string &body)
{
	return json::parse(body, nullptr, false);
}

// ---- crypto (Windows CNG; no extra deps) ------------------------------------

// base64url without padding (RFC 4648 §5), the encoding PKCE + the CSRF state use.
std::string Base64Url(const unsigned char *data, size_t len)
{
	static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	std::string out;
	out.reserve(((len + 2) / 3) * 4);
	size_t i = 0;
	for (; i + 3 <= len; i += 3) {
		const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) |
				   static_cast<uint32_t>(data[i + 2]);
		out.push_back(tbl[(n >> 18) & 63]);
		out.push_back(tbl[(n >> 12) & 63]);
		out.push_back(tbl[(n >> 6) & 63]);
		out.push_back(tbl[n & 63]);
	}
	const size_t rem = len - i;
	if (rem == 1) {
		const uint32_t n = static_cast<uint32_t>(data[i]) << 16;
		out.push_back(tbl[(n >> 18) & 63]);
		out.push_back(tbl[(n >> 12) & 63]);
	} else if (rem == 2) {
		const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
		out.push_back(tbl[(n >> 18) & 63]);
		out.push_back(tbl[(n >> 12) & 63]);
		out.push_back(tbl[(n >> 6) & 63]);
	}
	return out;
}

// Cryptographically-strong random bytes from the system-preferred RNG.
bool RandomBytes(unsigned char *buf, size_t len)
{
	return BCRYPT_SUCCESS(
		BCryptGenRandom(nullptr, buf, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

// SHA-256 of `in` into `out` (32 bytes).
bool Sha256(const std::string &in, unsigned char out[32])
{
	BCRYPT_ALG_HANDLE alg = nullptr;
	if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
		return false;
	}
	const bool ok = BCRYPT_SUCCESS(BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(in.data())),
						  static_cast<ULONG>(in.size()), out, 32));
	BCryptCloseAlgorithmProvider(alg, 0);
	return ok;
}

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

PkceLoopbackStrategy::PkceLoopbackStrategy(Config config) : config_(std::move(config)) {}

bool PkceLoopbackStrategy::authorize(const AuthContext &ctx, OAuthAccount &acct, std::string &err)
{
	// PKCE verifier (64 base64url chars from 48 random bytes) + S256 challenge, and
	// a CSRF state. Never logged.
	std::array<unsigned char, 48> verifierBytes{};
	std::array<unsigned char, 32> stateBytes{};
	if (!RandomBytes(verifierBytes.data(), verifierBytes.size()) ||
	    !RandomBytes(stateBytes.data(), stateBytes.size())) {
		err = "failed to generate PKCE entropy";
		return false;
	}
	const std::string verifier = Base64Url(verifierBytes.data(), verifierBytes.size());
	const std::string state = Base64Url(stateBytes.data(), stateBytes.size());

	unsigned char digest[32];
	if (!Sha256(verifier, digest)) {
		err = "failed to compute PKCE challenge";
		return false;
	}
	const std::string challenge = Base64Url(digest, sizeof digest);

	// Bind a 127.0.0.1-only ephemeral listener (never INADDR_ANY/0.0.0.0), then a
	// matching [::1] listener on the SAME port below so both loopback families work.
	WsaGuard wsa;
	if (!wsa.ok) {
		err = "Winsock init failed";
		return false;
	}

	SocketGuard listener4;
	listener4.s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener4.s == INVALID_SOCKET) {
		err = "failed to create loopback socket";
		return false;
	}

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only
	addr.sin_port = 0;                             // ephemeral port
	if (bind(listener4.s, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == SOCKET_ERROR) {
		err = "failed to bind loopback listener";
		return false;
	}
	if (listen(listener4.s, 1) == SOCKET_ERROR) {
		err = "failed to listen on loopback socket";
		return false;
	}

	sockaddr_in bound{};
	int boundLen = sizeof bound;
	if (getsockname(listener4.s, reinterpret_cast<sockaddr *>(&bound), &boundLen) == SOCKET_ERROR) {
		err = "failed to resolve loopback port";
		return false;
	}
	const unsigned port = ntohs(bound.sin_port);

	// Kick advertises the redirect host as "localhost", which resolves to ::1 before
	// 127.0.0.1 on IPv6-preferring hosts; an IPv4-only listener would then never see
	// the redirect. Bind a second listener on [::1] sharing the SAME ephemeral port
	// (IPV6_V6ONLY so it does not collide with the v4 bind). Best-effort: on any
	// failure, log and proceed IPv4-only rather than failing the whole flow.
	SocketGuard listener6;
	listener6.s = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	if (listener6.s == INVALID_SOCKET) {
		HostLog("PkceLoopback: IPv6 loopback socket unavailable; listening IPv4-only");
	} else {
		DWORD v6only = 1;
		setsockopt(listener6.s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&v6only),
			   sizeof v6only);
		sockaddr_in6 addr6{};
		addr6.sin6_family = AF_INET6;
		addr6.sin6_addr = in6addr_loopback; // ::1 only
		addr6.sin6_port = htons(static_cast<unsigned short>(port));
		if (bind(listener6.s, reinterpret_cast<sockaddr *>(&addr6), sizeof addr6) == SOCKET_ERROR ||
		    listen(listener6.s, 1) == SOCKET_ERROR) {
			HostLog("PkceLoopback: IPv6 loopback bind/listen failed; listening IPv4-only");
			closesocket(listener6.s);
			listener6.s = INVALID_SOCKET;
		}
	}

	// redirectHost is the advertised host string only (the listeners above cover both
	// loopback families); Kick uses "localhost", others "127.0.0.1".
	const std::string redirectUri =
		"http://" + config_.redirectHost + ":" + std::to_string(port) + config_.redirectPath;

	std::string scopesJoined;
	for (const std::string &scope : config_.scopes) {
		if (!scopesJoined.empty()) {
			scopesJoined += " ";
		}
		scopesJoined += scope;
	}

	std::string authUrl = config_.authorizeUrl;
	AppendQuery(authUrl, "response_type", "code");
	AppendQuery(authUrl, "client_id", config_.clientId);
	AppendQuery(authUrl, "redirect_uri", redirectUri);
	if (!scopesJoined.empty()) {
		AppendQuery(authUrl, "scope", scopesJoined);
	}
	AppendQuery(authUrl, "state", state);
	AppendQuery(authUrl, "code_challenge", challenge);
	AppendQuery(authUrl, "code_challenge_method", "S256");
	for (const std::pair<std::string, std::string> &param : config_.extraAuthParams) {
		AppendQuery(authUrl, param.first.c_str(), param.second);
	}

	ctx.emitProgress(json{{"phase", "browser"},
			      {"message", "Waiting for browser authorization\xE2\x80\xA6"},
			      {"timeoutSec", 180},
			      {"openUrl", authUrl}});

	// Accept the redirect connection. Poll with select() so cancel + an overall
	// deadline are honored without blocking forever in accept(). A browser may open
	// stray preconnect/favicon requests to the loopback port first, so keep accepting
	// until a request actually carries the OAuth code/error (or the deadline lapses)
	// rather than committing to the first connection. The deadline is short (180s) so
	// a provider that never redirects (e.g. an unverified-app screen) fails visibly
	// instead of hanging; the UI shows a matching countdown (timeoutSec above).
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
	SocketGuard conn;
	std::map<std::string, std::string> params;
	for (;;) {
		if (ctx.canceled()) {
			return false; // user closed the modal / bridge tore down / superseded
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			err = "No response from the browser -- if the sign-in page showed a "
			      "verification/blocked error, close it and try again.";
			return false;
		}

		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(listener4.s, &readfds);
		if (listener6.s != INVALID_SOCKET) {
			FD_SET(listener6.s, &readfds);
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
		const SOCKET ready = FD_ISSET(listener4.s, &readfds) ? listener4.s : listener6.s;
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
		params = ParseQuery(query);

		// The real redirect carries code= or error=; anything else (preconnect,
		// favicon, ...) gets a minimal answer and we keep waiting on the listener.
		if (params.count("code") || params.count("error")) {
			break;
		}
		const std::string ignore = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
		send(conn.s, ignore.data(), static_cast<int>(ignore.size()), 0);
		shutdown(conn.s, SD_SEND);
		closesocket(conn.s);
		conn.s = INVALID_SOCKET;
	}

	const auto getParam = [&params](const char *key) -> std::string {
		auto it = params.find(key);
		return it == params.end() ? std::string() : it->second;
	};
	const std::string oauthError = getParam("error");
	const std::string code = getParam("code");
	const std::string returnedState = getParam("state");
	// A genuine success needs a code, a matching CSRF state, and no error= param.
	const bool ok = oauthError.empty() && !code.empty() && returnedState == state;

	// Answer the browser before judging the result so the tab never hangs: a success
	// page only when the grant is actually valid, otherwise a visible failure page.
	std::string bodyHtml;
	if (ok) {
		bodyHtml =
			"<p>Authorization complete \xE2\x80\x94 you can close this window and return to OBS MultiStream.</p>";
	} else {
		bodyHtml =
			"<p>Authorization failed \xE2\x80\x94 close this window and return to OBS MultiStream to try again.</p>";
	}
	const std::string html =
		"<!doctype html><html><head><meta charset=\"utf-8\"><title>OBS MultiStream</title></head>"
		"<body style=\"font-family:system-ui,sans-serif;padding:2rem\">" +
		bodyHtml + "</body></html>";
	const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
				     std::to_string(html.size()) + "\r\nConnection: close\r\n\r\n" + html;
	send(conn.s, response.data(), static_cast<int>(response.size()), 0);
	shutdown(conn.s, SD_SEND);
	// listeners + conn close via their SocketGuards on every return below.

	if (!oauthError.empty()) {
		err = "authorization denied: " + oauthError;
		return false;
	}
	if (code.empty()) {
		err = "authorization response missing code";
		return false;
	}
	// CSRF: the redirect's state must match the one we sent.
	if (returnedState != state) {
		err = "state mismatch on authorization redirect";
		return false;
	}

	// The exchange below spends the one-time code. If the attempt was canceled or
	// superseded while the browser was redirecting, bail before burning it so a retry
	// gets a fresh grant. SocketGuards close the listeners + conn on return.
	if (ctx.canceled()) {
		return false; // user closed the modal / bridge tore down / superseded
	}

	// Exchange the code for tokens (PKCE verifier proves possession; the secret is
	// added only when the provider is a confidential client).
	std::string body;
	AppendForm(body, "grant_type", "authorization_code");
	AppendForm(body, "code", code);
	AppendForm(body, "redirect_uri", redirectUri);
	AppendForm(body, "client_id", config_.clientId);
	AppendForm(body, "code_verifier", verifier);
	if (!config_.clientSecret.empty()) {
		AppendForm(body, "client_secret", config_.clientSecret);
	}

	Http::HttpReq req;
	req.method = "POST";
	req.url = config_.tokenUrl;
	req.contentType = "application/x-www-form-urlencoded";
	req.body = body;

	const Http::HttpResponse resp = Http::HttpRequest(req);
	if (resp.status == 0) {
		err = "token exchange failed: " + resp.error;
		return false;
	}

	const json j = ParseJson(resp.body);
	if (j.is_discarded() || !j.is_object()) {
		err = "token exchange returned an unparseable body (HTTP " + std::to_string(resp.status) + ")";
		return false;
	}

	const std::string access = JsonStr(j, "access_token");
	if (access.empty()) {
		std::string code2 = JsonStr(j, "error");
		if (code2.empty()) {
			code2 = JsonStr(j, "message");
		}
		err = code2.empty() ? ("token exchange rejected (HTTP " + std::to_string(resp.status) + ")") : code2;
		return false;
	}

	acct.access = access;
	const std::string rotated = JsonStr(j, "refresh_token");
	if (!rotated.empty()) {
		acct.refresh = rotated;
	}
	acct.expireTime = static_cast<int64_t>(time(nullptr)) + JsonInt(j, "expires_in", 0);
	acct.scopeVer = config_.scopeVer;
	return true;
}

bool PkceLoopbackStrategy::refresh(OAuthAccount &acct, std::string &err)
{
	if (acct.refresh.empty()) {
		err = "no refresh token";
		return false;
	}

	std::string body;
	AppendForm(body, "grant_type", "refresh_token");
	AppendForm(body, "refresh_token", acct.refresh);
	AppendForm(body, "client_id", config_.clientId);
	if (!config_.clientSecret.empty()) {
		AppendForm(body, "client_secret", config_.clientSecret);
	}

	Http::HttpReq req;
	req.method = "POST";
	req.url = config_.tokenUrl;
	req.contentType = "application/x-www-form-urlencoded";
	req.body = body;

	const Http::HttpResponse resp = Http::HttpRequest(req);
	if (resp.status == 0) {
		err = "token refresh failed: " + resp.error;
		return false;
	}

	const json j = ParseJson(resp.body);
	if (j.is_discarded() || !j.is_object()) {
		err = "token refresh returned an unparseable body (HTTP " + std::to_string(resp.status) + ")";
		return false;
	}

	const std::string access = JsonStr(j, "access_token");
	if (access.empty()) {
		std::string code = JsonStr(j, "error");
		if (code.empty()) {
			code = JsonStr(j, "message");
		}
		// invalid_grant => the refresh token is dead; the caller must re-auth.
		err = code.empty() ? ("token refresh rejected (HTTP " + std::to_string(resp.status) + ")") : code;
		return false;
	}

	acct.access = access;
	acct.expireTime = static_cast<int64_t>(time(nullptr)) + JsonInt(j, "expires_in", 0);
	const std::string rotated = JsonStr(j, "refresh_token");
	if (!rotated.empty()) {
		acct.refresh = rotated;
	}
	return true;
}

bool PkceLoopbackStrategy::ensureFresh(OAuthAccount &acct, std::string &err, bool force)
{
	if (acct.refresh.empty()) {
		err = "no refresh token";
		return false;
	}

	const int64_t skew = 5;
	if (!force && static_cast<int64_t>(time(nullptr)) <= acct.expireTime - skew) {
		return true; // still fresh; no network
	}

	// Single-flight: serialize concurrent refreshes for the same account, keyed by
	// the account's stable identity (the account store key).
	const std::string accountId = AccountId(acct);
	const std::shared_ptr<std::mutex> lock = FlightLock(accountId);
	const std::lock_guard<std::mutex> guard(*lock);

	// Re-read the authoritative copy from the store inside the lock so a prior
	// holder's rotated one-time-use refresh token is the one we use. When the
	// account isn't stored yet, Get is a no-op and we keep the caller's copy.
	if (const std::optional<OAuthAccount> stored = Accounts().Get(accountId)) {
		acct = *stored;
	}

	// Re-check after the store re-read: a prior holder may have just refreshed.
	// A forced refresh (reactive 401) skips this -- the access token is dead
	// regardless of the stored expiry, so always refresh under the lock. This
	// matters for Kick, whose refresh tokens rotate and must be persisted.
	if (!force && static_cast<int64_t>(time(nullptr)) <= acct.expireTime - skew) {
		return true;
	}

	if (!refresh(acct, err)) {
		return false;
	}

	// Persist the rotated token inside the lock so the next single-flight holder
	// re-reads the new refresh token, not the spent one.
	Accounts().Put(accountId, acct);
	return true;
}

std::shared_ptr<std::mutex> PkceLoopbackStrategy::FlightLock(const std::string &key)
{
	const std::lock_guard<std::mutex> guard(flightMapMutex_);
	std::shared_ptr<std::mutex> &slot = flightLocks_[key];
	if (!slot) {
		slot = std::make_shared<std::mutex>();
	}
	return slot;
}

} // namespace OAuth
