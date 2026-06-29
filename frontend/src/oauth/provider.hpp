#ifndef OBS_MULTISTREAM_FRONTEND_OAUTH_PROVIDER_HPP_
#define OBS_MULTISTREAM_FRONTEND_OAUTH_PROVIDER_HPP_

#include <cstdint>
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
};

// The device-code grant's first-leg result: show `userCode` to the user, open
// `verificationUri` in a browser, then poll with `deviceCode` every
// `intervalSec` until `expiresSec` elapses.
struct DeviceCodeStart {
	std::string deviceCode;
	std::string userCode;
	std::string verificationUri;
	int intervalSec = 5;
	int expiresSec = 0;
};

// One platform's auth mechanism (device-code, PKCE-loopback, ...). Strategies are
// a small reusable set providers pick from; the framework holds only the device-
// code one in Task 3.
class AuthStrategy {
public:
	virtual ~AuthStrategy() = default;

	enum class PollResult { Pending, SlowDown, Success, Expired, Error };

	// Begin the grant (e.g. POST the device endpoint). Fills `out`; false + `err`
	// on transport/parse failure.
	virtual bool begin(DeviceCodeStart &out, std::string &err) = 0;

	// Poll the token endpoint once for `deviceCode`. On Success, `acct` is filled
	// with access/refresh/expireTime/scopeVer (identity is a separate provider
	// hook). Pending/SlowDown mean keep polling; Expired/Error are terminal.
	virtual PollResult poll(const std::string &deviceCode, OAuthAccount &acct, std::string &err) = 0;

	// Exchange the refresh token for a fresh access token, updating `acct` in
	// place (and persisting a rotated refresh token if the response carries one).
	// false + `err` on failure; invalid_grant means the caller must force re-auth.
	virtual bool refresh(OAuthAccount &acct, std::string &err) = 0;

	// Lazily refresh `acct` only when it is within the skew window of expiry,
	// single-flight per account so concurrent callers don't double-refresh. true
	// when the token is usable afterward.
	virtual bool ensureFresh(OAuthAccount &acct, std::string &err) = 0;
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
};

} // namespace OAuth

#endif // OBS_MULTISTREAM_FRONTEND_OAUTH_PROVIDER_HPP_
