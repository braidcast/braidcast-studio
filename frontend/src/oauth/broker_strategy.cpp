#include "broker_strategy.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <optional>
#include <utility>

#include <nlohmann/json.hpp>

#include "util/http_client.hpp"
#include "util/json_util.hpp"
#include "../bridge.hpp"
#include "../log.hpp"
#include "account_store.hpp"
#include "loopback_listener.hpp"

namespace OAuth {

using json = nlohmann::json;

namespace {

using JsonUtil::NumLoose;
using JsonUtil::ParseJson;
using JsonUtil::Str;

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

// base64url without padding (RFC 4648 §5) -- PKCE + the nonce use it.
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

bool RandomBytes(unsigned char *buf, size_t len)
{
	return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, buf, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

bool Sha256(const std::string &in, unsigned char out[32])
{
	BCRYPT_ALG_HANDLE alg = nullptr;
	if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
		return false;
	}
	const bool ok =
		BCRYPT_SUCCESS(BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(in.data())),
					  static_cast<ULONG>(in.size()), out, 32));
	BCryptCloseAlgorithmProvider(alg, 0);
	return ok;
}

} // namespace

BrokerStrategy::BrokerStrategy(Config config) : config_(std::move(config)) {}

bool BrokerStrategy::authorize(const AuthContext &ctx, OAuthAccount &acct, std::string &err)
{
	// PKCE verifier + S256 challenge, and a CSRF nonce (echoed back through the broker).
	std::array<unsigned char, 48> verifierBytes{};
	std::array<unsigned char, 32> nonceBytes{};
	if (!RandomBytes(verifierBytes.data(), verifierBytes.size()) ||
	    !RandomBytes(nonceBytes.data(), nonceBytes.size())) {
		err = "failed to generate OAuth entropy";
		return false;
	}
	const std::string verifier = Base64Url(verifierBytes.data(), verifierBytes.size());
	const std::string nonce = Base64Url(nonceBytes.data(), nonceBytes.size());

	unsigned char digest[32];
	if (!Sha256(verifier, digest)) {
		err = "failed to compute PKCE challenge";
		return false;
	}
	const std::string challenge = Base64Url(digest, sizeof digest);

	LoopbackListener listener;
	if (!listener.Bind(err)) {
		return false;
	}

	// Open the broker's /start in the system browser: it 302s to the provider and,
	// after consent, 302s back to this loopback with ?code=&nonce=.
	std::string startUrl = config_.brokerBaseUrl + "/v1/" + config_.platform + "/start";
	AppendQuery(startUrl, "port", std::to_string(listener.Port()));
	AppendQuery(startUrl, "nonce", nonce);
	AppendQuery(startUrl, "code_challenge", challenge);

	DBG(LogCat::OAuth, "broker authorize begin (platform %s, port %u)", config_.platform.c_str(), listener.Port());
	ctx.emitProgress(json{{"phase", "browser"},
			      {"message", "Waiting for browser authorization\xE2\x80\xA6"},
			      {"timeoutSec", 180},
			      {"openUrl", startUrl}});

	if (!listener.Await(ctx.canceled, 180, err)) {
		return false; // cancel (err empty) or timeout/socket error (err set)
	}

	const auto &params = listener.Params();
	const auto getParam = [&params](const char *key) -> std::string {
		auto it = params.find(key);
		return it == params.end() ? std::string() : it->second;
	};
	const std::string oauthError = getParam("error");
	const std::string code = getParam("code");
	const std::string returnedNonce = getParam("nonce");
	const bool ok = oauthError.empty() && !code.empty() && returnedNonce == nonce;
	DBG(LogCat::OAuth, "broker authorize returned (platform %s, ok=%d, error='%s')", config_.platform.c_str(),
	    (int)ok, oauthError.c_str());

	listener.Respond(ok);

	if (!oauthError.empty()) {
		err = "authorization denied: " + oauthError;
		return false;
	}
	if (code.empty()) {
		err = "authorization response missing code";
		return false;
	}
	if (returnedNonce != nonce) {
		err = "nonce mismatch on authorization redirect";
		return false;
	}
	if (ctx.canceled()) {
		return false; // superseded while redirecting; don't spend the one-time code
	}

	// Exchange the code at the broker /token (broker injects client_id + secret +
	// redirect_uri; forwards code_verifier only for PKCE providers).
	std::string body;
	AppendForm(body, "grant_type", "authorization_code");
	AppendForm(body, "code", code);
	AppendForm(body, "code_verifier", verifier);

	Http::HttpReq req;
	req.method = "POST";
	req.url = config_.brokerBaseUrl + "/v1/" + config_.platform + "/token";
	req.contentType = "application/x-www-form-urlencoded";
	req.body = body;

	const Http::HttpResponse resp = Http::HttpRequest(req);
	DBG(LogCat::OAuth, "broker token exchange (platform %s, http=%d)", config_.platform.c_str(), resp.status);
	if (resp.status == 0) {
		err = "token exchange failed: " + resp.error;
		return false;
	}

	const json j = ParseJson(resp.body);
	if (j.is_discarded() || !j.is_object()) {
		err = "token exchange returned an unparseable body (HTTP " + std::to_string(resp.status) + ")";
		return false;
	}

	const std::string access = Str(j, "access_token");
	if (access.empty()) {
		std::string code2 = Str(j, "error");
		if (code2.empty()) {
			code2 = Str(j, "message");
		}
		err = code2.empty() ? ("token exchange rejected (HTTP " + std::to_string(resp.status) + ")") : code2;
		return false;
	}

	acct.access = access;
	const std::string rotated = Str(j, "refresh_token");
	if (!rotated.empty()) {
		acct.refresh = rotated;
	}
	acct.expireTime = static_cast<int64_t>(time(nullptr)) + NumLoose(j, "expires_in", 0);
	acct.scopeVer = config_.scopeVer;
	return true;
}

bool BrokerStrategy::refresh(OAuthAccount &acct, std::string &err)
{
	RefreshFailureKind kind = RefreshFailureKind::Transient;
	return RefreshOnce(acct, err, kind);
}

bool BrokerStrategy::RefreshOnce(OAuthAccount &acct, std::string &err, RefreshFailureKind &kind)
{
	kind = RefreshFailureKind::Transient;
	if (acct.refresh.empty()) {
		err = "no refresh token";
		return false;
	}

	std::string body;
	AppendForm(body, "grant_type", "refresh_token");
	AppendForm(body, "refresh_token", acct.refresh);

	Http::HttpReq req;
	req.method = "POST";
	req.url = config_.brokerBaseUrl + "/v1/" + config_.platform + "/token";
	req.contentType = "application/x-www-form-urlencoded";
	req.body = body;

	const Http::HttpResponse resp = Http::HttpRequest(req);
	if (resp.status == 0) {
		kind = ClassifyRefreshFailure(0, std::string());
		err = "token refresh failed: " + resp.error;
		return false;
	}

	const json j = ParseJson(resp.body);
	if (j.is_discarded() || !j.is_object()) {
		kind = ClassifyRefreshFailure(resp.status, std::string());
		err = "token refresh returned an unparseable body (HTTP " + std::to_string(resp.status) + ")";
		return false;
	}

	const std::string access = Str(j, "access_token");
	if (access.empty()) {
		// Classify on the RFC 6749 `error` code alone. `message` is a human-readable
		// string some brokers send instead, fine for the error text but never a verdict
		// on the credential -- feeding it to the classifier would let arbitrary prose
		// decide whether the account gets signed out.
		const std::string code = Str(j, "error");
		kind = ClassifyRefreshFailure(resp.status, code);
		std::string text = code.empty() ? Str(j, "message") : code;
		err = text.empty() ? ("token refresh rejected (HTTP " + std::to_string(resp.status) + ")") : text;
		DBG(LogCat::OAuth, "refresh rejected (platform %s, http=%d, error='%s') -> %s",
		    config_.platform.c_str(), resp.status, code.c_str(), RefreshFailureKindName(kind));
		return false;
	}

	acct.access = access;
	acct.expireTime = static_cast<int64_t>(time(nullptr)) + NumLoose(j, "expires_in", 0);
	const std::string rotated = Str(j, "refresh_token");
	if (!rotated.empty()) {
		acct.refresh = rotated;
	}
	// A refresh that succeeds proves the credential is alive, so it retires any earlier
	// dead verdict (e.g. the token was revoked, then the user relinked out of band).
	acct.refreshDead = false;
	return true;
}

bool BrokerStrategy::ensureFresh(OAuthAccount &acct, std::string &err, bool force)
{
	if (acct.refresh.empty()) {
		err = "no refresh token";
		return false;
	}

	const int64_t skew = 5;
	if (!force && static_cast<int64_t>(time(nullptr)) <= acct.expireTime - skew) {
		return true;
	}

	const std::string accountId = AccountId(acct);
	const std::shared_ptr<std::mutex> lock = FlightLock(accountId);
	const std::lock_guard<std::mutex> guard(*lock);

	const std::string priorAccess = acct.access;
	if (const std::optional<OAuthAccount> stored = Accounts().Get(accountId)) {
		acct = *stored;
	}
	if (!force && static_cast<int64_t>(time(nullptr)) <= acct.expireTime - skew) {
		return true;
	}
	// Forced (reactive-401) path: if a peer rotated the token while we waited on the
	// flight lock, the re-read access token differs from the one the caller held when
	// it 401'd. Use that fresh token instead of refreshing again -- a redundant
	// rotation could invalidate the peer's in-flight access token (and burns an extra
	// rotation on providers that rotate their refresh token per refresh, e.g. Kick).
	if (force && !priorAccess.empty() && acct.access != priorAccess) {
		return true;
	}
	RefreshFailureKind kind = RefreshFailureKind::Transient;
	if (!RefreshOnce(acct, err, kind)) {
		if (kind == RefreshFailureKind::Dead) {
			// The contract provider.hpp declares for refresh(): invalid_grant means
			// re-auth, and no silent recovery exists. Record the verdict so the shared
			// connected/needs-reconnect gates (registry.hpp) flip this account to the
			// amber relink state instead of leaving it green until it dies mid-stream.
			// Field-scoped: `acct` is a pre-HTTP snapshot, so writing it whole could
			// clobber a token a concurrent flight rotated.
			acct.refreshDead = true;
			if (Accounts().SetRefreshDead(accountId, true)) {
				DBG(LogCat::OAuth, "account %s marked needs-reconnect (refresh token dead)",
				    accountId.c_str());
				Bridge::EmitOAuthStatus();
			}
		} else {
			// Transient: the credential is unproven, not dead. Leave the account
			// connected and let the next call retry -- flagging here would sign a user
			// out over a broker hiccup.
			DBG(LogCat::OAuth, "refresh failed transiently for %s (retryable): %s", accountId.c_str(),
			    err.c_str());
		}
		return false;
	}
	Accounts().Put(accountId, acct);
	return true;
}

std::shared_ptr<std::mutex> BrokerStrategy::FlightLock(const std::string &key)
{
	const std::lock_guard<std::mutex> guard(flightMapMutex_);
	std::shared_ptr<std::mutex> &slot = flightLocks_[key];
	if (!slot) {
		slot = std::make_shared<std::mutex>();
	}
	return slot;
}

void BrokerStrategy::ForgetAccount(const std::string &accountId)
{
	// Called only on explicit account removal (UI thread), where the just-disconnected
	// account has no refresh in flight -- so no caller is holding the erased mutex. Even
	// if one were, FlightLock hands out a shared_ptr copy, so the mutex object outlives
	// the map slot until that copy drops; erasing the slot only forgets the key.
	const std::lock_guard<std::mutex> guard(flightMapMutex_);
	flightLocks_.erase(accountId);
}

} // namespace OAuth
