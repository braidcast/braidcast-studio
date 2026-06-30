#ifndef OBS_MULTISTREAM_FRONTEND_OAUTH_TWITCH_PROVIDER_HPP_
#define OBS_MULTISTREAM_FRONTEND_OAUTH_TWITCH_PROVIDER_HPP_

#include <string>

#include "../chat/twitch_chat.hpp"
#include "../http_client.hpp"
#include "device_code.hpp"
#include "provider.hpp"

// The Twitch stream provider: device-code auth (no secret) plus the net-new
// Helix stream-metadata hooks (title / category / tags / language / content
// classification / branded content) the Go Live modal reads and writes. The
// legacy client only ever fetched the user + stream key, so every metadata call
// here is new (endpoints verified against current dev.twitch.tv docs).
namespace OAuth {

// Bumped whenever the requested scope set changes, so installs holding tokens
// issued under an older scope set are forced to re-auth (see OAuthAccount::scopeVer).
constexpr int TWITCH_SCOPE_VERSION = 2;

class TwitchProvider : public StreamProvider {
public:
	TwitchProvider();

	std::string id() const override { return "twitch"; }
	std::string displayName() const override { return "Twitch"; }
	std::string brandColor() const override { return "#a970ff"; }
	int scopeVer() const override { return TWITCH_SCOPE_VERSION; }

	json capabilityJson() const override;

	AuthStrategy *auth() override { return &auth_; }
	Chat::ChatTransport *chat() override { return &chat_; }

	bool fetchIdentity(OAuthAccount &acct, std::string &err) override;
	bool getMetadata(OAuthAccount &acct, json &out, std::string &err) override;
	bool searchCategories(OAuthAccount &acct, const std::string &query, json &out, std::string &err) override;
	bool applyMetadata(OAuthAccount &acct, const json &fields, std::string &err) override;
	bool fetchStreamKey(OAuthAccount &acct, std::string &key, std::string &err) override;

private:
	// Send an authenticated Helix request: ensureFresh proactively, stamp the
	// Client-Id + bearer headers, and on a 401 force one refresh + retry. `req` is
	// taken by value so headers are re-applied cleanly on the retry (the bearer
	// changes after a refresh). false + `err` only on a transport failure or an
	// unrecoverable 401 ("re-authentication required"); an HTTP error otherwise
	// returns true with the status/body left for the caller to interpret.
	bool SendAuthed(OAuthAccount &acct, Http::HttpReq req, Http::HttpResponse &resp, std::string &err);

	DeviceCodeStrategy auth_;
	// Declared after auth_ so it is constructed second -- chat_ captures &auth_ for a
	// reactive token refresh, so auth_ must already exist.
	TwitchChat chat_{&auth_};
};

} // namespace OAuth

#endif // OBS_MULTISTREAM_FRONTEND_OAUTH_TWITCH_PROVIDER_HPP_
