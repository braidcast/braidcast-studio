#ifndef OBS_MULTISTREAM_FRONTEND_OAUTH_PROVIDER_HPP_
#define OBS_MULTISTREAM_FRONTEND_OAUTH_PROVIDER_HPP_

#include <cstdint>
#include <functional>
#include <memory>
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

// Forward declaration: a provider optionally owns a chat transport (Phase 9.0) and
// an event transport (Phase 9.2a). The full interfaces live in
// chat/chat_transport.hpp and events/event_transport.hpp, which include THIS header
// for OAuthAccount -- so we only forward-declare here to avoid a header cycle.
namespace Chat {
class ChatTransport;
}
namespace Events {
class EventTransport;
}

// SendAuthed takes an Http::HttpReq by value and fills an Http::HttpResponse; the full
// definitions live in http_client.hpp, which provider.cpp includes. By-value + reference
// parameters only need the incomplete types at the declaration, so forward-declare here.
namespace Http {
struct HttpReq;
struct HttpResponse;
} // namespace Http

namespace OAuth {

using json = nlohmann::json;

// Which audience total a platform reports for an account. Drives the panel label.
enum class AudienceKind { Unknown, Followers, Subscribers };

inline const char *AudienceKindName(AudienceKind k)
{
	switch (k) {
	case AudienceKind::Followers:
		return "followers";
	case AudienceKind::Subscribers:
		return "subscribers";
	default:
		return "";
	}
}

inline AudienceKind AudienceKindFromName(const std::string &s)
{
	if (s == "followers") {
		return AudienceKind::Followers;
	}
	if (s == "subscribers") {
		return AudienceKind::Subscribers;
	}
	return AudienceKind::Unknown;
}

// Result of an audience-total read. `available` is false when the platform has no
// REST total (Kick) or the read failed / is unsupported; the poller then leaves the
// account's cached count untouched.
struct AudienceResult {
	int64_t count = -1;
	AudienceKind kind = AudienceKind::Unknown;
	bool hidden = false;
	bool available = false;
};

// The persisted OAuth record, keyed in the account store by AccountId
// (providerId:userId). `expireTime` is absolute epoch seconds (the "valid
// credential = refresh token present" model from the legacy OAuth port).
// `scopeVer` lets a provider force re-auth on installs holding tokens issued
// under an older scope set.
struct OAuthAccount {
	std::string providerId;
	std::string access;
	std::string refresh;
	std::string userId;
	std::string login;
	std::string displayName;
	int64_t expireTime = 0;
	int scopeVer = 0;
	// Identity + audience (Channel identity feature). Persisted so the panel
	// shows cached avatar/count instantly on launch. audienceCount == -1 means
	// "not yet known"; audienceHidden reflects YouTube's hiddenSubscriberCount.
	std::string avatarUrl;
	int64_t audienceCount = -1;
	AudienceKind audienceKind = AudienceKind::Unknown;
	bool audienceHidden = false;
	int64_t audienceUpdatedNs = 0;
};

// The account's stable identity: providerId + ":" + userId. Pure function of the
// record (userId is always populated for a connected account), so no field stores
// it. This is the single key used by the account store and every live hub. Returns
// "<providerId>:" (empty-user tail) for a record whose identity fetch never
// completed -- such a record must not be persisted (see the connect flow).
inline std::string AccountId(const OAuthAccount &a)
{
	return a.providerId + ":" + a.userId;
}

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

	// Lazily refresh `acct` only when within the skew window (or always, when
	// `force`). Single-flight per account so concurrent callers don't double-refresh.
	// Store-coherent: the account is re-read from the account store (keyed by
	// AccountId(acct)) inside the flight lock and the rotated token written back, so
	// concurrent callers with stale copies never invalidate each other's one-time-use
	// refresh token. true when the token is usable afterward. `force` bypasses the
	// skew/expiry check (a reactive 401 means the access token is dead) but keeps the
	// same single-flight + re-read + write-back path.
	virtual bool ensureFresh(OAuthAccount &acct, std::string &err, bool force = false) = 0;

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

	// Ensure `acct` carries a resolved userId, self-healing a record whose identity
	// fetch never completed: if userId is empty, fetch it via fetchIdentity and fail
	// only if that also fails. Read paths (metadata / stream key / viewer + audience
	// counts) call this instead of hard-aborting on a transiently-empty identity, so
	// every platform self-heals the same way. Non-virtual: the recovery policy is
	// uniform; only the fetch it delegates to is per-platform.
	bool ensureIdentity(OAuthAccount &acct, std::string &err);

	// Send an authenticated platform request: proactively ensureFresh, stamp the auth
	// headers via stampAuth, then on a 401 force one refresh + retry with the new bearer.
	// `req` is taken by value so the headers are re-applied cleanly on the retry (the
	// bearer changes after a refresh). false + `err` only on a transport failure or an
	// unrecoverable 401 ("re-authentication required"); an HTTP error otherwise returns
	// true with the status/body left for the caller to interpret. Non-virtual: the
	// proactive-refresh + reactive-401 policy is uniform; only the header stamp is
	// per-platform (see stampAuth).
	bool SendAuthed(OAuthAccount &acct, Http::HttpReq req, Http::HttpResponse &resp, std::string &err);

	// Fetch the channel's current stream metadata (title/category/...) into `out`
	// for prefill. `acct` is non-const so a reactive token refresh (proactive skew
	// or a 401 retry) propagates back for the caller to persist. false + `err` on
	// failure.
	virtual bool getMetadata(OAuthAccount &acct, json &out, std::string &err) = 0;

	// Resolve a category/game typeahead `query` into `out` (a list of matches).
	// `acct` is non-const for the same refresh-propagation reason as getMetadata.
	virtual bool searchCategories(OAuthAccount &acct, const std::string &query, json &out, std::string &err) = 0;

	// Push the resolved metadata `fields` to the platform. `profileUuid` is the
	// stream profile this apply targets (distinct from the shared account identity):
	// a provider that writes a per-go-live ingest endpoint back into the profile
	// (YouTube) needs it; Twitch/Kick ignore it. false + `err` on failure (a
	// per-platform failure warns but must not block going live).
	virtual bool applyMetadata(OAuthAccount &acct, const std::string &profileUuid, const json &fields,
				   std::string &err) = 0;

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

	// Construct a FRESH chat transport for `acct` (Phase 9.0), or nullptr when the
	// provider has no chat stream. Ownership transfers to the caller (the ChatHub,
	// which holds it for the account's live session). One instance per account: each
	// owns its own socket + state, so two accounts on one platform never share a
	// connection or a mutex. The base default returns null (defined out-of-line in
	// provider.cpp, where Chat::ChatTransport is complete -- unique_ptr's deleter needs
	// the full type) so a provider without chat needs no override.
	virtual std::unique_ptr<Chat::ChatTransport> makeChat(const OAuthAccount &acct);

	// Construct a FRESH event transport for `acct` (Phase 9.2a), or nullptr. Same
	// per-account ownership contract as makeChat; base default returns null (out-of-line
	// for the same completeness reason).
	virtual std::unique_ptr<Events::EventTransport> makeEvents(const OAuthAccount &acct);

	// Report the platform's current concurrent viewer count for `acct` into `out`
	// (Phase 9.0 aggregate viewer poller). `acct` is non-const so a reactive token
	// refresh propagates back. Returns true with `out` set (0 when the channel is
	// offline) on a usable read; false when unsupported or not currently live -- the
	// poller then omits this platform from the aggregate. Default: unsupported.
	virtual bool viewerCount(OAuthAccount &acct, int &out, std::string &err)
	{
		(void)acct;
		(void)out;
		(void)err;
		return false;
	}

	// Report the account's follower/subscriber TOTAL (distinct from concurrent
	// viewers). `acct` is non-const so a reactive token refresh propagates back.
	// Returns true with `out.available` set on a usable read; false / available=false
	// when unsupported or not currently obtainable. Default: unsupported.
	virtual bool audienceCount(OAuthAccount &acct, AudienceResult &out, std::string &err)
	{
		(void)acct;
		(void)out;
		(void)err;
		return false;
	}

	// The platform-specific channel reference the hub passes into the chat transport's
	// connect() for `acct`: the channel login/slug for Twitch IRC / Kick Pusher, the per-broadcast
	// liveChatId for YouTube. Default = the account login; providers whose chat keys
	// off something else override it, keeping the hub free of per-platform
	// channel-resolution branches.
	virtual std::string chatChannelRef(const OAuthAccount &acct) { return acct.login; }

	// Drop the active-broadcast chat/viewer target for ONE account on stream stop
	// (Phase 9.0). Providers editing a persistent channel (Twitch/Kick) have nothing
	// to clear; YouTube overrides to zero the account's cached liveChatId/broadcastId
	// so a later go-live without a fresh applyMetadata can't poll a stale broadcast.
	virtual void clearActiveBroadcast(const std::string &accountId) { (void)accountId; }

protected:
	// Stamp the per-request auth headers onto `r`, called by SendAuthed for each attempt.
	// Base default: `Authorization: Bearer <access>`. Twitch overrides to prepend its
	// `Client-Id` header; Kick/YouTube authenticate with the bearer alone and keep this.
	virtual void stampAuth(Http::HttpReq &r, const OAuthAccount &acct) const;
};

} // namespace OAuth

#endif // OBS_MULTISTREAM_FRONTEND_OAUTH_PROVIDER_HPP_
