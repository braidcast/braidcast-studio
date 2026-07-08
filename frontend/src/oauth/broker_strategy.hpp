#ifndef OBS_MULTISTREAM_FRONTEND_OAUTH_BROKER_STRATEGY_HPP_
#define OBS_MULTISTREAM_FRONTEND_OAUTH_BROKER_STRATEGY_HPP_

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "provider.hpp"

// Authorization-code grant driven through the Braidcast OAuth broker
// (auth.braidcast.com). The app opens the system browser at the broker's /start,
// the broker redirects to the provider and (after consent) back to this app's
// loopback listener, then the app POSTs the code to the broker's /token, which
// injects the client secret. No client_id or client_secret ships for the OAuth
// flow. PKCE is always generated client-side; the broker forwards it only for
// providers configured with pkce=true. The only per-provider knob is the platform
// slug + scope version.
namespace OAuth {

class BrokerStrategy : public AuthStrategy {
public:
	struct Config {
		std::string brokerBaseUrl; // e.g. "https://auth.braidcast.com" (no trailing slash)
		std::string platform;      // "twitch" | "kick" | "youtube"
		int scopeVer = 0;          // stamped into issued accounts
	};

	explicit BrokerStrategy(Config config);

	bool authorize(const AuthContext &ctx, OAuthAccount &acct, std::string &err) override;
	bool refresh(OAuthAccount &acct, std::string &err) override;
	bool ensureFresh(OAuthAccount &acct, std::string &err, bool force = false) override;
	int scopeVer() const override { return config_.scopeVer; }

private:
	std::shared_ptr<std::mutex> FlightLock(const std::string &key);

	Config config_;
	std::mutex flightMapMutex_;
	std::map<std::string, std::shared_ptr<std::mutex>> flightLocks_;
};

} // namespace OAuth

#endif // OBS_MULTISTREAM_FRONTEND_OAUTH_BROKER_STRATEGY_HPP_
