#include "registry.hpp"

#include <algorithm>
#include <memory>
#include <utility>

#include "../log.hpp"
#include "provider_creds.hpp"
#include "account_store.hpp"
#include "kick_provider.hpp"
#include "twitch_provider.hpp"
#include "youtube_provider.hpp"

namespace OAuth {

void ProviderRegistry::Register(std::unique_ptr<StreamProvider> provider)
{
	if (!provider) {
		return;
	}
	const std::string id = provider->id();
	providers_[id] = std::move(provider);
}

StreamProvider *ProviderRegistry::Get(const std::string &id) const
{
	auto it = providers_.find(id);
	return it == providers_.end() ? nullptr : it->second.get();
}

std::vector<StreamProvider *> ProviderRegistry::All() const
{
	std::vector<StreamProvider *> out;
	out.reserve(providers_.size());
	for (const auto &entry : providers_) {
		out.push_back(entry.second.get());
	}
	return out;
}

ProviderRegistry &Registry()
{
	static ProviderRegistry registry;
	return registry;
}

bool IsAccountConnected(const OAuthAccount &acct)
{
	StreamProvider *provider = Registry().Get(acct.providerId);
	// An unregistered provider (e.g. Twitch without baked creds) can't be used, so
	// it isn't connected. Scope must be current, a refresh token must exist ("valid
	// credential = refresh token present"), and that token must not have been rejected
	// as dead -- a revoked token is still a non-empty string, so presence alone read
	// green right up until the account died mid-stream.
	return provider && provider->isTokenScopeCurrent(acct) && !acct.refresh.empty() && !acct.refreshDead;
}

bool AccountNeedsReconnect(const OAuthAccount &acct)
{
	StreamProvider *provider = Registry().Get(acct.providerId);
	// Has a credential the user established, but it can no longer be used as-is:
	// either the token predates the current scope set, or the broker rejected its
	// refresh with invalid_grant. Both recover only through a fresh interactive grant,
	// so both surface on the one relink signal. Distinct from a partial no-refresh
	// record (which is just ignored).
	return provider && !acct.refresh.empty() && (!provider->isTokenScopeCurrent(acct) || acct.refreshDead);
}

bool IsProviderConnected(const std::string &providerId)
{
	for (const auto &entry : Accounts().All()) {
		if (entry.second.providerId == providerId && IsAccountConnected(entry.second)) {
			return true;
		}
	}
	return false;
}

std::vector<std::string> ConnectedProviders()
{
	std::vector<std::string> out;
	for (const auto &entry : Accounts().All()) {
		const std::string &providerId = entry.second.providerId;
		if (IsAccountConnected(entry.second) && std::find(out.begin(), out.end(), providerId) == out.end()) {
			out.push_back(providerId);
		}
	}
	return out;
}

void BootProviders()
{
	if (TwitchConfigured()) {
		Registry().Register(std::make_unique<TwitchProvider>());
	}
	// Kick and YouTube carry no baked credentials -- their OAuth runs entirely through
	// the broker -- so they always register.
	Registry().Register(std::make_unique<KickProvider>());
	Registry().Register(std::make_unique<YouTubeProvider>());
	HostLog("[oauth] provider registry booted: " + std::to_string(Registry().All().size()) + " provider(s)");
}

} // namespace OAuth
