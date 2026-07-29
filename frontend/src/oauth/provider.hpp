#ifndef OBS_MULTISTREAM_FRONTEND_OAUTH_PROVIDER_HPP_
#define OBS_MULTISTREAM_FRONTEND_OAUTH_PROVIDER_HPP_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

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
	// The broker rejected this record's refresh token with invalid_grant: it is
	// revoked/expired and only a fresh interactive grant can recover it. Persisted so
	// the verdict survives a relaunch, and cleared by any refresh that succeeds (and
	// naturally by a reconnect, which stores a brand-new record). Kept distinct from an
	// empty `refresh` -- the dead token is still a non-empty string, which is exactly
	// why the connected gate could not see it.
	bool refreshDead = false;
	// Identity + audience (Channel identity feature). Persisted so the panel
	// shows cached avatar/count instantly on launch. audienceCount == -1 means
	// "not yet known"; audienceHidden reflects YouTube's hiddenSubscriberCount.
	std::string avatarUrl;
	int64_t audienceCount = -1;
	AudienceKind audienceKind = AudienceKind::Unknown;
	bool audienceHidden = false;
	int64_t audienceUpdatedNs = 0;
	// The provider's reusable ingest stream ids, for platforms that model the RTMP
	// endpoint as a create-once resource (YouTube liveStreams; empty elsewhere).
	// Remembered so each go-live re-verifies and re-binds the existing stream (1
	// quota unit) instead of inserting a fresh one (50 units). An entry is dropped
	// whenever its remembered stream no longer verifies server-side.
	//
	// Keyed by profileUuid -- ONE STREAM PER DESTINATION, not per account. A stream
	// carries a single video feed and binds to one broadcast at a time, so an account
	// streaming two orientations needs two ingest endpoints: sharing one would hand both
	// stream profiles the same RTMP key and let the second go-live's bind detach the
	// first broadcast. The profileUuid alone is the key because the record is already
	// scoped to one account -- this is the profile half of a DestinationId, not a second
	// keying scheme. An empty key is the account-wide destination.
	std::map<std::string /*profileUuid*/, std::string /*streamId*/> reusableStreamIds;
	// Absolute epoch seconds when this provider's exhausted daily API quota next resets
	// (0 = no exhaustion recorded). Persisted so a relaunch does not have to re-learn the
	// verdict by spending a request that is guaranteed to fail -- with the gate held only in
	// memory, every launch burned one doomed request per account per source.
	//
	// The value is PROVIDER-WIDE, not per-account: quota is spent per API project, so one
	// exhausted account means every account on that provider is dark. It is stored on each
	// account record only because that is where per-provider state already lives; the
	// provider writes the same instant to all of its accounts and takes the MAXIMUM back on
	// load, so any single surviving record restores the verdict. Absolute rather than a
	// duration, which is what makes it self-clearing: once the instant passes it simply
	// reads as not-exhausted, with no reset step and no relaunch.
	int64_t quotaResetEpoch = 0;
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

// One live DESTINATION: an account streaming under one stream profile. The account
// alone is not an identity on a platform that creates a broadcast per go-live --
// YouTube gives each stream profile its own broadcast, with its own liveChatId and its
// own concurrent-viewer figure, so two orientations on one channel are two distinct
// destinations sharing one accountId. Per-broadcast state keys off this pair.
//
// An EMPTY profileUuid is the account-wide destination, used for platforms that edit
// one persistent channel (Twitch/Kick: one chat, one viewer figure per account, so
// every profile pointing at that account resolves to the same single destination) and
// for a cache-recovery entry that could not be attributed to a specific profile.
struct DestinationId {
	std::string accountId;
	std::string profileUuid;

	bool operator<(const DestinationId &o) const
	{
		return accountId != o.accountId ? accountId < o.accountId : profileUuid < o.profileUuid;
	}
	bool operator==(const DestinationId &o) const
	{
		return accountId == o.accountId && profileUuid == o.profileUuid;
	}
};

// The single flat rendering of a DestinationId, for JSON keys, list keys and log lines:
// "<providerId>:<userId>@<profileUuid>", or just the accountId for the account-wide
// destination. '@' is unambiguous here -- neither an accountId (providerId ':' userId)
// nor a uuid contains one. The ONE formatter; never inline the concatenation.
inline std::string DestinationKey(const DestinationId &d)
{
	return d.profileUuid.empty() ? d.accountId : d.accountId + "@" + d.profileUuid;
}

// The account-wide destination for `accountId` (empty profileUuid). Named so an
// account-scoped surface that has to hand a DestinationId to a destination-keyed API
// states that intent, instead of spelling the empty profile half out per call site.
inline DestinationId AccountDestination(const std::string &accountId)
{
	return DestinationId{accountId, std::string()};
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

	// Drop any per-account bookkeeping the strategy holds (e.g. the single-flight
	// refresh lock) when an account is removed, so a disconnected account leaves
	// nothing behind. Default no-op; strategies with per-account state override.
	virtual void ForgetAccount(const std::string &accountId) { (void)accountId; }

	// Best-effort revocation of `acct`'s grant at the provider, called once from
	// TeardownAccount right after the account is dropped from the local store.
	// Runs off the calling thread and must never block it or report failure back
	// -- a disconnect has already succeeded locally by the time this is called and
	// stays succeeded regardless of what happens here (offline, provider error, or
	// an already-dead token are all fine outcomes). Default no-op; strategies
	// backed by a revocable provider (BrokerStrategy) override.
	virtual void Revoke(const OAuthAccount &acct) { (void)acct; }

	// The scope version this strategy currently requests. Tokens stored with a
	// lower scopeVer were issued under an older permission set and must reconnect.
	virtual int scopeVer() const { return 0; }

	// Does this strategy's grant carry a refresh token? True for the OAuth 2.0 shape
	// every provider here uses: a short-lived access token plus the long-lived refresh
	// token that renews it, which is therefore the credential "is this account usable"
	// asks about (see AccountHasCredential in registry.hpp).
	//
	// False for a platform that issues ONE long-lived token and no refresh token at all
	// (Meta: a Page access token does not expire). There the access token is the whole
	// credential, there is nothing to renew, and every refresh path is a no-op rather
	// than a failure -- so the answer has to come from the strategy rather than from a
	// per-provider branch at each of the call sites that ask.
	virtual bool usesRefreshToken() const { return true; }
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

	// The scope version the provider currently requests (mirrors the auth
	// strategy's scopeVer). Default 0; providers bump it when their scope set
	// changes so older tokens are treated as needing reconnect.
	virtual int scopeVer() const { return 0; }

	// True when `acct`'s stored scope version covers the provider's current
	// scopes. A behind-scope token lacks permissions the app now needs, so the
	// status path reports it as needing reconnect and metadata calls refuse it.
	bool isTokenScopeCurrent(const OAuthAccount &acct) const { return acct.scopeVer >= scopeVer(); }

	// The capability descriptor: { id, displayName, auth{...}, fields[...] } the
	// Svelte modal renders fields from. Brand color is the web's (platformColors.ts),
	// so it is not carried here.
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
	// true with the status/body left for the caller to interpret. The proactive-refresh +
	// reactive-401 policy stays uniform in this base implementation; virtual so a provider
	// can wrap the send path itself (YouTube gates both paths behind its shared
	// daily-quota state -- see YouTubeProvider).
	virtual bool SendAuthed(OAuthAccount &acct, Http::HttpReq req, Http::HttpResponse &resp, std::string &err);

	// Streaming sibling of SendAuthed for a long-lived server-push response (YouTube
	// liveChatMessages.streamList): same proactive-refresh + reactive-401 policy, but the
	// 2xx body is delivered to `onChunk` as it arrives (returning false aborts the stream)
	// rather than buffered. A non-2xx body is captured into `errorBody` for the caller to
	// interpret (quota/rate-limit reason), and since nothing was streamed on a 401 the
	// forced refresh + retry is clean. Returns the HTTP status: 0 on a transport failure
	// (with `err` set), 401 on an unrecoverable re-auth ("re-authentication required"),
	// otherwise the status with the body already streamed or captured. `req` is by value
	// so the auth header is re-stamped cleanly on the retry (the bearer changes).
	// Virtual for the same provider-wrap seam as SendAuthed.
	virtual long SendAuthedStreaming(OAuthAccount &acct, Http::HttpReq req,
					 const std::function<bool(std::string_view chunk)> &onChunk,
					 std::string &errorBody, std::string &err);

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
	// (YouTube) needs it; Twitch/Kick ignore it. `goingLive` is true ONLY when this
	// apply is the immediate prelude to streaming.start; a create-per-go-live provider
	// (YouTube) uses it to avoid creating a broadcast for a standalone pre-live edit,
	// while persistent-channel providers (Twitch/Kick) ignore it and edit regardless of
	// intent. false + `err` on failure (a per-platform failure warns but must not block
	// going live).
	virtual bool applyMetadata(OAuthAccount &acct, const std::string &profileUuid, const json &fields,
				   bool goingLive, std::string &err) = 0;

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

	// True when the platform creates a distinct live broadcast PER DESTINATION (one per
	// stream profile) instead of editing one persistent channel. This single predicate
	// decides every per-broadcast keying question -- how many chat transports the hub
	// runs for an account, and how many viewer figures the poller asks for -- so the
	// rule lives in one place rather than being re-decided per subsystem.
	//
	// false (Twitch/Kick): one channel, one chat, one viewer figure per account. Every
	// profile bound to that account collapses onto the account-wide destination, so N
	// profiles still cost exactly one transport and one poll, unchanged.
	// true (YouTube): each profile's broadcast has its own liveChatId and its own
	// concurrentViewers, so each needs its own transport and its own read.
	virtual bool broadcastPerDestination() const { return false; }

	// Report the platform's current concurrent viewer count for `acct` into `out`
	// (Phase 9.0 aggregate viewer poller). `acct` is non-const so a reactive token
	// refresh propagates back. Returns true with `out` set (0 when the channel is
	// offline) on a usable read; false when unsupported or not currently live -- the
	// poller then omits this platform from the aggregate. Default: unsupported.
	//
	// This is the per-CHANNEL primitive. The poller calls viewerCounts below instead, so
	// a platform with one broadcast per destination can report each one separately.
	virtual bool viewerCount(OAuthAccount &acct, int &out, std::string &err)
	{
		(void)acct;
		(void)out;
		(void)err;
		return false;
	}

	// Report concurrent viewers for every live destination of `acct`, so an account with
	// several concurrent broadcasts contributes all of them to the aggregate instead of
	// silently dropping every one but the first. Returns true when at least one row was
	// filled; false (leaving `out` untouched) when unsupported or not live.
	//
	// The default is the whole per-channel implementation: exactly ONE viewerCount call,
	// reported under the account-wide destination. A platform with one channel per account
	// therefore keeps its previous cost and shape without overriding anything.
	virtual bool viewerCounts(OAuthAccount &acct, std::map<DestinationId, int> &out, std::string &err)
	{
		int count = 0;
		if (!viewerCount(acct, count, err)) {
			return false;
		}
		out[AccountDestination(AccountId(acct))] = count;
		return true;
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
	// connect() for one destination: the channel login/slug for Twitch IRC / Kick Pusher,
	// the per-broadcast liveChatId for YouTube. `profileUuid` names which of the account's
	// broadcasts is being connected and is empty for the account-wide destination; a
	// per-channel platform ignores it. Default = the account login; providers whose chat
	// keys off something else override it, keeping the hub free of per-platform
	// channel-resolution branches.
	virtual std::string chatChannelRef(const OAuthAccount &acct, const std::string &profileUuid)
	{
		(void)profileUuid;
		return acct.login;
	}

	// Drop EVERY active-broadcast chat/viewer target belonging to one account on stream
	// stop (Phase 9.0). Providers editing a persistent channel (Twitch/Kick) have nothing
	// to clear; YouTube overrides to zero the account's cached liveChatId/broadcastId so a
	// later go-live without a fresh applyMetadata can't poll a stale broadcast. Account-
	// scoped rather than destination-scoped because a stop tears down all of the account's
	// destinations at once, and the caller iterates the account store.
	virtual void clearActiveBroadcast(const std::string &accountId) { (void)accountId; }

	// Drop ONE destination's active-broadcast state, for when that destination's own output
	// ends while the account's other destinations keep streaming. Distinct from the
	// account-wide sibling above rather than folded into it: an empty profileUuid is already
	// a real address (the account-wide destination), so it cannot double as a wildcard.
	// Default no-op; YouTube overrides. Must be idempotent -- an output can report its end
	// twice (a deliberate stop whose libobs stop signal also fires).
	//
	// Leaving a dead destination cached is not merely stale: the viewer poller bills one
	// videos.list per cached broadcast every cycle, so an uncleared entry spends quota on a
	// stream that has ended for as long as the session lasts.
	virtual void clearActiveBroadcastDestination(const DestinationId &dest) { (void)dest; }

protected:
	// Stamp the per-request auth headers onto `r`, called by SendAuthed for each attempt.
	// Base default: `Authorization: Bearer <access>`. Twitch overrides to prepend its
	// `Client-Id` header; Kick/YouTube authenticate with the bearer alone and keep this.
	virtual void stampAuth(Http::HttpReq &r, const OAuthAccount &acct) const;
};

} // namespace OAuth

#endif // OBS_MULTISTREAM_FRONTEND_OAUTH_PROVIDER_HPP_
