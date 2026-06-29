#include "registry.hpp"

#include <memory>
#include <utility>

#include "../log.hpp"
#include "../provider_creds.hpp"
#include "kick_provider.hpp"
#include "twitch_provider.hpp"

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

void BootProviders()
{
	if (TwitchConfigured()) {
		Registry().Register(std::make_unique<TwitchProvider>());
	}
	if (KickConfigured()) {
		Registry().Register(std::make_unique<KickProvider>());
	}
	HostLog("[oauth] provider registry booted: " + std::to_string(Registry().All().size()) + " provider(s)");
}

} // namespace OAuth
