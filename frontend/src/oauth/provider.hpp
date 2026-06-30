#ifndef OBS_MULTISTREAM_FRONTEND_OAUTH_PROVIDER_HPP_
#define OBS_MULTISTREAM_FRONTEND_OAUTH_PROVIDER_HPP_

#include <cstdint>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

// The provider framework for platform OAuth + stream metadata (Phase 8a). A
// `StreamProvider` describes one platform (Twitch, Kick, YouTube, ...): its
// identity, a capability descriptor the Svelte modal renders from, an auth
// strategy, and the metadata read/write hooks the Go Live flow calls. Concrete
// providers (Twitch lands in Task 4) live in their own modules and register one
// instance into the ProviderRegistry at boot. The bridge dispatches generic
// `oauth.*` / `streamMeta.*` methods through the registry -- no per-platform
// branches in the bridge surface.

// Forward declaration: a provider optionally owns a chat transport (Phase 9.0).
// The full interface lives in chat/chat_transport.hpp, which includes THIS header
// for OAuthAccount -- so we only forward-declare here to avoid a header cycle.
namespace Chat {
class ChatTransport;
}

namespace OAuth {

using json = nlohmann::json;

// The persisted OAuth record, keyed in the token store by stream-profile uuid.
// `expireTime` is absolute epoch seconds (the "valid credential = refresh token
// present" model from the legacy OAuth port). `scopeVer` lets a provider force
// re-auth on installs holding tokens issued under an older scope set.
struct OAuthAccount {
	std::string providerId;
	std::string access;
	std::string refresh;
	std::string userId;
	std::string login;
	std::string displayName;
	int64_t expireTime = 0;
	int scopeVer = 0;

	// Transient (never serialized): the stream-profile uuid this record is stored
	// under. The store stamps it on read so a provider's deep call chain can hand
	// it to ensureFresh for store-coherent single-flight refresh without threading
	// the uuid through every metadata hook. AccountToJson/FromJson ignore it.
	std::string profileUuid;
};

// The runtime context the framework hands an AuthStrategy for one interactive
// grant. `emitProgress` reports a phase payload to JS (wired to the
// `oauth.connectProgress` event on the UI thread); a top-level `openUrl` key in a
// payload is opened in the system browser by the connect flow and stripped before
// the event reaches JS. `canceled` returns true once the user cancels or the
// bridge tears down, so the strategy can bail promptly from any wait.
struct AuthContext {
	std::function<void(const json &payload)> emitProgress;
	std::function<bool()> canceled;
};

// One platform's auth mechanism (device-code, PKCE-loopback, ...). Strategies are
// a small reusable set providers pick from. `authorize` runs the WHOLE interactive
// grant on a worker thread; strategy-specific wire details (device endpoints, the
// PKCE loopback listener) stay inside the concrete strategy so the connect flow
// has no per-strategy branches.
class AuthStrategy {
public:
	virtual ~AuthStrategy() = default;

	// Run the entire interactive grant on the calling worker thread: drive the
	// platform-specific authorization, reporting progress via `ctx.emitProgress`
	// and bailing promptly when `ctx.canceled()` turns true, then fill `acct`
	// (access/refresh/expireTime/scopeVer; identity is a separate provider hook).
	// Returns false on failure (with `err` set) or on cancellation (where `err`
	// may be empty) -- the connect flow suppresses the error emit when
	// `ctx.canceled()` is true.
	virtual bool authorize(const AuthContext &ctx, OAuthAccount &acct, std::string &err) = 0;

	// Exchange the refresh token for a fresh access token, updating `acct` in
	// place (and persisting a rotated refresh token if the response carries one).
	// false + `err` on failure; invalid_grant means the caller must force re-auth.
	virtual bool refresh(OAuthAccount &acct, std::string &err) = 0;

	// Lazily refresh `acct` only when it is within the skew window of expiry,
	// single-flight per account so concurrent callers don't double-refresh. When
	// `profileUuid` is non-empty the refresh is store-coherent: the account is
	// re-read from the token store inside the flight lock and the rotated token is
	// written back, so concurrent callers with stale copies never invalidate each
	// other's one-time-use refresh token. true when the token is usable afterward.
	// `force` bypasses the skew/expiry check (a reactive 401 means the access token
	// is dead regardless of its stored expiry), but keeps the SAME single-flight +
	// store-coherent re-read + write-back path so a rotated refresh token is still
	// persisted rather than dropped.
	virtual bool ensureFresh(OAuthAccount &acct, const std::string &profileUuid, std::string &err,
				 bool force = false) = 0;

	// The scope version this strategy currently requests. Tokens stored with a
	// lower scopeVer were issued under an older permission set and must reconnect.
	virtual int scopeVer() const { return 0; }
};

// One streaming platform. `capabilityJson()` is the descriptor the modal renders
// from (see the Phase 8 spec schema). The metadata hooks are pure virtual and
// implemented by concrete providers (Twitch in Task 4); the framework declares
// them here so the registry + bridge can call through one interface.
class StreamProvider {
public:
	virtual ~StreamProvider() = default;

	virtual std::string id() const = 0;
	virtual std::string displayName() const = 0;
	virtual std::string brandColor() const = 0;

	// The scope version the provider currently requests (mirrors the auth
	// strategy's scopeVer). Default 0; providers bump it when their scope set
	// changes so older tokens are treated as needing reconnect.
	virtual int scopeVer() const { return 0; }

	// True when `acct`'s stored scope version covers the provider's current
	// scopes. A behind-scope token lacks permissions the app now needs, so the
	// status path reports it as needing reconnect and metadata calls refuse it.
	bool isTokenScopeCurrent(const OAuthAccount &acct) const { return acct.scopeVer >= scopeVer(); }

	// The capability descriptor: { id, displayName, brandColor, auth{...},
	// fields[...] } the Svelte modal renders fields from.
	virtual json capabilityJson() const = 0;

	// The provider's auth strategy (owned by the provider; never null for a
	// connectable provider).
	virtual AuthStrategy *auth() = 0;

	// Read the account's identity (login / display name / user id) into `acct`
	// after a successful grant. false + `err` on failure.
	virtual bool fetchIdentity(OAuthAccount &acct, std::string &err) = 0;

	// Fetch the channel's current stream metadata (title/category/...) into `out`
	// for prefill. `acct` is non-const so a reactive token refresh (proactive skew
	// or a 401 retry) propagates back for the caller to persist. false + `err` on
	// failure.
	virtual bool getMetadata(OAuthAccount &acct, json &out, std::string &err) = 0;

	// Resolve a category/game typeahead `query` into `out` (a list of matches).
	// `acct` is non-const for the same refresh-propagation reason as getMetadata.
	virtual bool searchCategories(OAuthAccount &acct, const std::string &query, json &out, std::string &err) = 0;

	// Push the resolved metadata `fields` to the platform. false + `err` on
	// failure (a per-platform failure warns but must not block going live).
	virtual bool applyMetadata(OAuthAccount &acct, const json &fields, std::string &err) = 0;

	// Optionally fetch the platform stream key for `acct` (Twitch exposes one via
	// /helix/streams/key; most providers do not). Default: unsupported. On success
	// fills `key`; false + `err` (or false with empty `key`) when unavailable.
	virtual bool fetchStreamKey(OAuthAccount &acct, std::string &key, std::string &err)
	{
		(void)acct;
		(void)err;
		key.clear();
		return false;
	}

	// The provider's chat transport (Phase 9.0), or nullptr when the provider has no
	// chat stream. Owned by the provider (like auth()); the ChatHub runs it on a
	// worker thread between go-live and stop. Default null so a provider without chat
	// needs no override.
	virtual Chat::ChatTransport *chat() { return nullptr; }

	// The platform-specific channel reference the hub passes into chat()->connect for
	// `acct`: the channel login/slug for Twitch IRC / Kick Pusher, the per-broadcast
	// liveChatId for YouTube. Default = the account login; providers whose chat keys
	// off something else override it, keeping the hub free of per-platform
	// channel-resolution branches.
	virtual std::string chatChannelRef(const OAuthAccount &acct) { return acct.login; }
};

} // namespace OAuth

#endif // OBS_MULTISTREAM_FRONTEND_OAUTH_PROVIDER_HPP_
