#include "device_code.hpp"

#include <chrono>
#include <ctime>
#include <optional>
#include <thread>
#include <utility>

#include "../http_client.hpp"
#include "token_store.hpp"

namespace OAuth {

namespace {

// Read a string field tolerantly: missing/non-string -> "".
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

// Read an integer field tolerantly: a number, or a numeric string, else `fallback`.
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

// Form-encode "key=value", percent-encoding the value.
void AppendForm(std::string &body, const char *key, const std::string &value)
{
	if (!body.empty()) {
		body += "&";
	}
	body += key;
	body += "=";
	body += Http::UrlEncode(value);
}

// Parse a JSON body, tolerating garbage (returns a null json on failure).
json ParseJson(const std::string &body)
{
	return json::parse(body, nullptr, false);
}

} // namespace

DeviceCodeStrategy::DeviceCodeStrategy(Config config) : config_(std::move(config)) {}

bool DeviceCodeStrategy::authorize(const AuthContext &ctx, OAuthAccount &acct, std::string &err)
{
	DeviceCodeStart start;
	if (!begin(start, err)) {
		return false;
	}

	// Surface the user code and open the verification page. `openUrl` is opened by
	// the connect flow and stripped before the event reaches JS, so the emitted
	// payload carries only {phase, userCode, verificationUri, expiresSec}.
	ctx.emitProgress(json{{"phase", "deviceCode"},
			      {"userCode", start.userCode},
			      {"verificationUri", start.verificationUri},
			      {"expiresSec", start.expiresSec},
			      {"openUrl", start.verificationUri}});

	int intervalSec = start.intervalSec > 0 ? start.intervalSec : 5;
	const int lifeSec = start.expiresSec > 0 ? start.expiresSec : 300;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(lifeSec);

	for (;;) {
		if (ctx.canceled()) {
			return false; // bridge tore down, or the user canceled, mid-flow
		}
		std::this_thread::sleep_for(std::chrono::seconds(intervalSec));
		if (ctx.canceled()) {
			return false;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			err = "device code expired before authorization";
			return false;
		}

		std::string pollErr;
		const PollResult result = poll(start.deviceCode, acct, pollErr);
		if (result == PollResult::Pending) {
			continue;
		}
		if (result == PollResult::SlowDown) {
			intervalSec += 5; // RFC 8628: back off on slow_down
			continue;
		}
		if (result == PollResult::Expired) {
			err = "device code expired before authorization";
			return false;
		}
		if (result == PollResult::Error) {
			err = pollErr;
			return false;
		}
		break; // Success
	}
	return true;
}

bool DeviceCodeStrategy::begin(DeviceCodeStart &out, std::string &err)
{
	std::string scopesJoined;
	for (const std::string &scope : config_.scopes) {
		if (!scopesJoined.empty()) {
			scopesJoined += " ";
		}
		scopesJoined += scope;
	}

	std::string body;
	AppendForm(body, "client_id", config_.clientId);
	AppendForm(body, config_.scopeParam.c_str(), scopesJoined);

	Http::HttpReq req;
	req.method = "POST";
	req.url = config_.deviceUrl;
	req.contentType = "application/x-www-form-urlencoded";
	req.body = body;

	const Http::HttpResponse resp = Http::HttpRequest(req);
	if (resp.status == 0) {
		err = "device authorization request failed: " + resp.error;
		return false;
	}

	const json j = ParseJson(resp.body);
	if (resp.status >= 400 || j.is_discarded() || !j.is_object()) {
		err = "device authorization rejected (HTTP " + std::to_string(resp.status) + "): " + resp.body;
		return false;
	}

	out.deviceCode = JsonStr(j, "device_code");
	out.userCode = JsonStr(j, "user_code");
	// Twitch returns "verification_uri"; some providers send "verification_uri_complete".
	out.verificationUri = JsonStr(j, "verification_uri");
	if (out.verificationUri.empty()) {
		out.verificationUri = JsonStr(j, "verification_uri_complete");
	}
	out.intervalSec = JsonInt(j, "interval", 5);
	out.expiresSec = JsonInt(j, "expires_in", 0);

	if (out.deviceCode.empty() || out.userCode.empty()) {
		err = "device authorization response missing device_code/user_code";
		return false;
	}
	return true;
}

DeviceCodeStrategy::PollResult DeviceCodeStrategy::poll(const std::string &deviceCode, OAuthAccount &acct,
							std::string &err)
{
	std::string body;
	AppendForm(body, "client_id", config_.clientId);
	AppendForm(body, "device_code", deviceCode);
	AppendForm(body, "grant_type", "urn:ietf:params:oauth:grant-type:device_code");

	Http::HttpReq req;
	req.method = "POST";
	req.url = config_.tokenUrl;
	req.contentType = "application/x-www-form-urlencoded";
	req.body = body;

	const Http::HttpResponse resp = Http::HttpRequest(req);
	if (resp.status == 0) {
		err = "token poll failed: " + resp.error;
		return PollResult::Error;
	}

	const json j = ParseJson(resp.body);
	if (j.is_discarded() || !j.is_object()) {
		err = "token poll returned an unparseable body (HTTP " + std::to_string(resp.status) + ")";
		return PollResult::Error;
	}

	const std::string access = JsonStr(j, "access_token");
	if (!access.empty()) {
		acct.access = access;
		const std::string rotated = JsonStr(j, "refresh_token");
		if (!rotated.empty()) {
			acct.refresh = rotated;
		}
		acct.expireTime = static_cast<int64_t>(time(nullptr)) + JsonInt(j, "expires_in", 0);
		acct.scopeVer = config_.scopeVer;
		return PollResult::Success;
	}

	// Pending errors arrive as `error` (RFC 8628) or `message` (Twitch's shape).
	std::string code = JsonStr(j, "error");
	if (code.empty()) {
		code = JsonStr(j, "message");
	}
	if (code == "authorization_pending") {
		return PollResult::Pending;
	}
	if (code == "slow_down") {
		return PollResult::SlowDown;
	}
	if (code == "expired_token") {
		err = "device code expired";
		return PollResult::Expired;
	}
	err = code.empty() ? ("token poll error (HTTP " + std::to_string(resp.status) + ")") : code;
	return PollResult::Error;
}

bool DeviceCodeStrategy::refresh(OAuthAccount &acct, std::string &err)
{
	if (acct.refresh.empty()) {
		err = "no refresh token";
		return false;
	}

	std::string body;
	AppendForm(body, "grant_type", "refresh_token");
	AppendForm(body, "refresh_token", acct.refresh);
	AppendForm(body, "client_id", config_.clientId);

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
	// Persist a rotated refresh token whenever one is returned (Twitch rotates).
	const std::string rotated = JsonStr(j, "refresh_token");
	if (!rotated.empty()) {
		acct.refresh = rotated;
	}
	return true;
}

bool DeviceCodeStrategy::ensureFresh(OAuthAccount &acct, const std::string &profileUuid, std::string &err, bool force)
{
	if (acct.refresh.empty()) {
		err = "no refresh token";
		return false;
	}

	const int64_t skew = 5;
	if (!force && static_cast<int64_t>(time(nullptr)) <= acct.expireTime - skew) {
		return true; // still fresh; no network
	}

	// Single-flight: serialize concurrent refreshes for the same account. Key on
	// the stream-profile uuid (the store key) when known, else the user/provider id.
	const std::string key = !profileUuid.empty() ? profileUuid : (acct.userId.empty() ? acct.providerId : acct.userId);
	const std::shared_ptr<std::mutex> lock = FlightLock(key);
	const std::lock_guard<std::mutex> guard(*lock);

	// Re-read the authoritative copy from the store inside the lock: a prior holder
	// may have just refreshed and rotated this account's one-time-use refresh token.
	// Operating on the caller's stale copy here would refresh the spent token and
	// kill the account. Without a uuid (the connect flow's not-yet-stored account)
	// fall back to the caller's copy.
	if (!profileUuid.empty()) {
		const std::optional<OAuthAccount> stored = Tokens().Get(profileUuid);
		if (stored) {
			acct = *stored;
		}
	}

	// Re-check after the store re-read: a prior holder may have just refreshed.
	// A forced refresh (reactive 401) skips this -- the access token is dead
	// regardless of the stored expiry, so always refresh under the lock.
	if (!force && static_cast<int64_t>(time(nullptr)) <= acct.expireTime - skew) {
		return true;
	}

	if (!refresh(acct, err)) {
		return false;
	}

	// Persist the rotated token inside the lock so the next single-flight holder
	// re-reads the new refresh token, not the spent one.
	if (!profileUuid.empty()) {
		Tokens().Put(profileUuid, acct);
	}
	return true;
}

std::shared_ptr<std::mutex> DeviceCodeStrategy::FlightLock(const std::string &key)
{
	const std::lock_guard<std::mutex> guard(flightMapMutex_);
	std::shared_ptr<std::mutex> &slot = flightLocks_[key];
	if (!slot) {
		slot = std::make_shared<std::mutex>();
	}
	return slot;
}

} // namespace OAuth
