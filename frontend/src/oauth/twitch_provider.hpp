#ifndef OBS_MULTISTREAM_FRONTEND_OAUTH_TWITCH_PROVIDER_HPP_
#define OBS_MULTISTREAM_FRONTEND_OAUTH_TWITCH_PROVIDER_HPP_

#include <memory>
#include <string>

#include "../http_client.hpp"
#include "broker_strategy.hpp"
#include "provider.hpp"

// The Twitch chat/event transports are constructed per account in makeChat/makeEvents,
// defined out-of-line in the .cpp where the complete types are available -- so this
// header only forward-declares the event transport for the friend grant below (the
// makeChat/makeEvents return types are the base interfaces provider.hpp forward-declares).
namespace Events {
class TwitchEvents;
}

// The Twitch stream provider: broker auth (public client id kept for Helix) plus the net-new
// Helix stream-metadata hooks (title / category / tags / language / content
// classification / branded content) the Go Live modal reads and writes. The
// legacy client only ever fetched the user + stream key, so every metadata call
// here is new (endpoints verified against current dev.twitch.tv docs).
namespace OAuth {

// Bumped whenever the requested scope set changes, so installs holding tokens
// issued under an older scope set are forced to re-auth (see OAuthAccount::scopeVer).
// v3 adds the EventSub read scopes (moderator:read:followers, channel:read:subscriptions,
// bits:read) for the Phase 9.2b events feed.
constexpr int TWITCH_SCOPE_VERSION = 3;

class TwitchProvider : public StreamProvider {
public:
	TwitchProvider();

	std::string id() const override { return "twitch"; }
	std::string displayName() const override { return "Twitch"; }
	std::string brandColor() const override { return "#a970ff"; }
	int scopeVer() const override { return TWITCH_SCOPE_VERSION; }

	json capabilityJson() const override;

	AuthStrategy *auth() override { return &auth_; }
	std::unique_ptr<Chat::ChatTransport> makeChat(const OAuthAccount &acct) override;
	std::unique_ptr<Events::EventTransport> makeEvents(const OAuthAccount &acct) override;

	bool fetchIdentity(OAuthAccount &acct, std::string &err) override;
	bool getMetadata(OAuthAccount &acct, json &out, std::string &err) override;
	bool searchCategories(OAuthAccount &acct, const std::string &query, json &out, std::string &err) override;
	bool applyMetadata(OAuthAccount &acct, const std::string &profileUuid, const json &fields,
			   std::string &err) override;
	bool fetchStreamKey(OAuthAccount &acct, std::string &key, std::string &err) override;
	bool viewerCount(OAuthAccount &acct, int &out, std::string &err) override;
	bool audienceCount(OAuthAccount &acct, AudienceResult &out, std::string &err) override;

protected:
	// Twitch stamps its public Client-Id ahead of the bearer (Helix requires both);
	// the base SendAuthed calls this per attempt. Client-Id first, then Authorization.
	void stampAuth(Http::HttpReq &r, const OAuthAccount &acct) const override;

private:
	// The event transport reuses SendAuthed for its EventSub subscribe POSTs and the
	// followers backfill GET (same proactive-refresh + reactive-401 path as metadata).
	friend class Events::TwitchEvents;

	BrokerStrategy auth_;
};

} // namespace OAuth

#endif // OBS_MULTISTREAM_FRONTEND_OAUTH_TWITCH_PROVIDER_HPP_
