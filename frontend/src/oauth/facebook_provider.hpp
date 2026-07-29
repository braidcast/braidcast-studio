#ifndef OBS_MULTISTREAM_FRONTEND_OAUTH_FACEBOOK_PROVIDER_HPP_
#define OBS_MULTISTREAM_FRONTEND_OAUTH_FACEBOOK_PROVIDER_HPP_

#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "broker_strategy.hpp"
#include "provider.hpp"

// The Facebook Live stream provider, PAGES ONLY. Meta's token model is the one thing
// that shapes this module: the authorization-code grant returns a SHORT-lived user
// token, which the broker trades for a long-lived one (grant_type=fb_exchange_token),
// which in turn yields per-Page access tokens that DO NOT EXPIRE. There is no refresh
// token anywhere in that chain, which is why the strategy is configured
// usesRefreshToken=false (see AuthStrategy::usesRefreshToken).
//
// The stored account is the USER (its long-lived token is what lists the Pages); the
// Page is chosen per stream profile in the Go Live modal, because one account can
// administer several. Every Page-scoped call presents that Page's own token, never the
// user's -- see AsPage in the .cpp.
//
// Like YouTube, Facebook creates a fresh broadcast per go-live: POST
// /{page-id}/live_videos returns the RTMPS ingest URL + key, which is written into the
// linked stream profile, and the live video is explicitly ended on stop. There is no
// category/game catalog to offer (Facebook Gaming was sunset), so this descriptor has
// no category field.
namespace OAuth {

// Bumped whenever the requested scope set changes, so installs holding tokens issued
// under an older scope set are forced to re-auth (see OAuthAccount::scopeVer).
// v2 adds business_management, without which /me/accounts omits business-owned and New
// Pages Experience Pages -- a token issued under v1 cannot see a Page at all.
constexpr int FACEBOOK_SCOPE_VERSION = 2;

class FacebookProvider : public StreamProvider {
public:
	FacebookProvider();

	std::string id() const override { return "facebook"; }
	// "Facebook Live" verbatim: it is the rtmp-services entry name, so it is what
	// StreamProfile::PlatformName() yields for such a profile and what the Streams tab
	// matches a profile against to decide whether to offer a Connect button.
	std::string displayName() const override { return "Facebook Live"; }
	int scopeVer() const override { return FACEBOOK_SCOPE_VERSION; }

	json capabilityJson() const override;

	AuthStrategy *auth() override { return &auth_; }

	bool fetchIdentity(OAuthAccount &acct, std::string &err) override;
	bool getMetadata(OAuthAccount &acct, json &out, std::string &err) override;
	// The account's Pages, filtered by `query` -- this provider's `page` picker, not a
	// game/category catalog. Facebook Gaming was sunset and no browsable category
	// taxonomy replaced it, so `page` is the only lookup field in the descriptor and this
	// hook has exactly one meaning here.
	bool searchCategories(OAuthAccount &acct, const std::string &query, json &out, std::string &err) override;
	bool applyMetadata(OAuthAccount &acct, const std::string &profileUuid, const json &fields, bool goingLive,
			   std::string &err) override;

	// A live video is created per stream profile, so all per-broadcast state keys off the
	// destination -- two profiles on one Page are two independent broadcasts.
	bool broadcastPerDestination() const override { return true; }

	// One target per streamable Page: a Page has its own RTMPS endpoint, so two Pages
	// are two destinations, never two views of one. Reuses FetchPages, so the usable/
	// listed distinction it draws applies here unchanged -- a Page this app holds no
	// token for is not a place this account can stream to.
	bool enumerateTargets(OAuthAccount &acct, TargetList &out, std::string &err) override;

	// End every live video this account still holds (stream stop), then one destination's
	// (that output ended while others continue). Both pop the entry under the mutex and
	// hand the end request to a worker: the callers run on the UI thread and must not
	// block on Graph. Idempotent -- a destination already popped ends nothing twice.
	void clearActiveBroadcast(const std::string &accountId) override;
	void clearActiveBroadcastDestination(const DestinationId &dest) override;

private:
	// One Page the connected user administers. `token` is that Page's access token: it
	// does not expire, is never persisted, and is never logged.
	struct PageRef {
		std::string id;
		std::string name;
		std::string token;
	};

	// One destination's live video. The Page token is carried alongside because ending
	// the broadcast is a Page-scoped call that must work from a worker thread after the
	// destination has already been dropped from the map.
	struct LiveVideo {
		std::string id;
		std::string pageToken;
	};

	// One /me/accounts read. `pages` holds only the Pages that can actually be streamed
	// to -- an id AND an access token -- while `returned` counts every row Meta sent, so
	// "this account administers no Pages" stays distinguishable from "Meta listed Pages
	// but handed this app a token for none of them". The two need different remedies.
	struct PageList {
		std::vector<PageRef> pages;
		size_t returned = 0;
	};

	// GET /me/accounts -- every Page the account administers, with its access token.
	// The single Page reader: the picker's option list, the single-Page prefill and the
	// go-live resolution all come through here rather than each spelling out the request.
	bool FetchPages(OAuthAccount &acct, PageList &out, std::string &err);

	// The Page this apply targets: the one the `page` field names, or -- when the field is
	// empty and the account administers exactly one Page -- that Page. Anything else is a
	// failure with a sentence telling the user to choose one.
	bool ResolvePage(OAuthAccount &acct, const json &fields, PageRef &out, std::string &err);

	// This destination's live video, false when it is not broadcasting.
	bool ActiveLiveVideo(const DestinationId &dest, LiveVideo &out) const;

	// POST end_live_video=true for each entry, on a worker thread. Best-effort: a failure
	// is logged, never surfaced -- the local stop has already happened and stays happened,
	// and Facebook ends an abandoned live video on its own once ingest stops.
	void EndLiveVideos(std::vector<LiveVideo> ending);

	BrokerStrategy auth_;

	// Live videos keyed by destination, written by applyMetadata (the only place one is
	// created) and read/popped by the stop hooks. Guarded because applyMetadata runs on a
	// worker thread while the stop hooks run on the UI thread.
	//
	// This is also where a comments/chat transport would attach: Facebook's live comments
	// arrive over a long-lived read of /{live-video-id}/comments, which belongs on
	// StreamProvider::SendAuthedStreaming with the Page token, keyed by exactly this
	// destination. Not implemented in this pass.
	mutable std::mutex liveVideoMutex_;
	std::map<DestinationId, LiveVideo> liveVideos_;
};

} // namespace OAuth

#endif // OBS_MULTISTREAM_FRONTEND_OAUTH_FACEBOOK_PROVIDER_HPP_
