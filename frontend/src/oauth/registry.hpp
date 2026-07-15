#ifndef OBS_MULTISTREAM_FRONTEND_OAUTH_REGISTRY_HPP_
#define OBS_MULTISTREAM_FRONTEND_OAUTH_REGISTRY_HPP_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "provider.hpp"

// The provider registry: a map<id, StreamProvider> populated once at boot. The
// generic `oauth.*` / `streamMeta.*` bridge methods dispatch through it so adding
// a platform is one module + one registry line, with no per-platform branches in
// the bridge. Task 3 ships the empty framework; Task 4 registers Twitch.
namespace OAuth {

class ProviderRegistry {
public:
	// Register `provider` under its id(). Replaces any existing entry with the
	// same id (last registration wins).
	void Register(std::unique_ptr<StreamProvider> provider);

	// The provider with `id`, or nullptr if none is registered.
	StreamProvider *Get(const std::string &id) const;

	// All registered providers (registration is at boot, before any reads).
	std::vector<StreamProvider *> All() const;

private:
	std::map<std::string, std::unique_ptr<StreamProvider>> providers_;
};

// Process-wide registry accessor (Meyers singleton), mirroring the other
// frontend stores' free-accessor shape.
ProviderRegistry &Registry();

// Populate the registry at boot. Called once from ObsBootstrap::Start(). Task 3
// registers nothing (the framework only); Task 4 adds the Twitch provider here.
void BootProviders();

// --- Account connection state (single source of truth) ---------------------
//
// "Logged in" for an account means all three hold: its provider is registered,
// its token scope is current, and it carries a refresh token ("valid credential
// = refresh token present", see OAuthAccount). A record left by a partial/aborted
// connect (identity fetched, no refresh token) is NOT connected. Every consumer
// -- the oauth.status chip gate, the chat/events hubs, the audience/viewer
// pollers, the search-token lookup -- must funnel through these so the definition
// can't drift per call site (the drift was exactly the "shows a platform I never
// signed into" bug).

// Is this specific account currently usable for authenticated calls?
bool IsAccountConnected(const OAuthAccount &acct);

// Account has a credential that cannot be used as-is and only a fresh interactive
// grant recovers: its token was issued under an older scope set, or the broker
// rejected its refresh with invalid_grant (revoked/expired). One signal for both, so
// the UI has a single relink state to render. (Connected and needs-reconnect are
// mutually exclusive; a no-credential partial record is neither.)
bool AccountNeedsReconnect(const OAuthAccount &acct);

// Is at least one account for `providerId` connected? (e.g. IsProviderConnected("twitch").)
bool IsProviderConnected(const std::string &providerId);

// Distinct provider ids with >=1 connected account (e.g. {"twitch","kick"}).
std::vector<std::string> ConnectedProviders();

} // namespace OAuth

#endif // OBS_MULTISTREAM_FRONTEND_OAUTH_REGISTRY_HPP_
