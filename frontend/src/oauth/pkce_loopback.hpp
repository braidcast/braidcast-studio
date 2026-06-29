#ifndef OBS_MULTISTREAM_FRONTEND_OAUTH_PKCE_LOOPBACK_HPP_
#define OBS_MULTISTREAM_FRONTEND_OAUTH_PKCE_LOOPBACK_HPP_

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "provider.hpp"

// The OAuth 2.0 Authorization Code grant with PKCE over a loopback redirect (RFC
// 7636 + the "OAuth for Native Apps" loopback pattern, RFC 8252 §7.3) -- the auth
// path for platforms that need a browser redirect rather than a device code
// (Kick lands first in Phase 8c; YouTube reuses it in 8d with no client secret).
//
// authorize() runs the whole grant on a worker: generate the PKCE verifier +
// challenge and a CSRF `state`, bind a 127.0.0.1-only ephemeral TCP listener, open
// the system browser to the authorize URL, accept the ONE redirect connection,
// validate `state`, then exchange the code (+ verifier, + secret when configured)
// at the token endpoint. The only per-provider knobs are the endpoints, client id,
// optional secret, scopes, scope version, and the redirect path.
namespace OAuth {

class PkceLoopbackStrategy : public AuthStrategy {
public:
	struct Config {
		std::string authorizeUrl;        // authorization endpoint
		std::string tokenUrl;            // token endpoint
		std::string clientId;            // public client id
		std::string clientSecret;        // empty => public client (no secret, e.g. YouTube)
		std::vector<std::string> scopes; // requested scopes (space-joined into one form field)
		int scopeVer = 0;                // stamped into issued accounts for scope-version invalidation
		std::string redirectPath = "/";  // path of the loopback redirect_uri
		// Host used when composing the redirect_uri STRING only; the listener still
		// binds 127.0.0.1. Kick's OAuth frontend rewrites a literal 127.0.0.1, so it
		// must advertise "localhost" (which resolves to 127.0.0.1 on Windows).
		std::string redirectHost = "127.0.0.1";
		// Extra authorize-URL query params appended (url-encoded) verbatim. Empty for
		// Twitch/Kick; YouTube uses {{"access_type","offline"},{"prompt","consent"}} so
		// Google returns a refresh token (without prompt=consent it omits it on re-consent).
		std::vector<std::pair<std::string, std::string>> extraAuthParams;
	};

	explicit PkceLoopbackStrategy(Config config);

	bool authorize(const AuthContext &ctx, OAuthAccount &acct, std::string &err) override;
	bool refresh(OAuthAccount &acct, std::string &err) override;
	bool ensureFresh(OAuthAccount &acct, const std::string &profileUuid, std::string &err,
			 bool force = false) override;
	int scopeVer() const override { return config_.scopeVer; }

private:
	// One mutex per account key, created on demand, so two concurrent ensureFresh
	// callers for the SAME account serialize (single-flight) while different
	// accounts refresh in parallel. Mirrors DeviceCodeStrategy.
	std::shared_ptr<std::mutex> FlightLock(const std::string &key);

	Config config_;
	std::mutex flightMapMutex_;
	std::map<std::string, std::shared_ptr<std::mutex>> flightLocks_;
};

} // namespace OAuth

#endif // OBS_MULTISTREAM_FRONTEND_OAUTH_PKCE_LOOPBACK_HPP_
