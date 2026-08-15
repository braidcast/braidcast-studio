#ifndef OBS_MULTISTREAM_FRONTEND_OAUTH_FACEBOOK_PROVIDER_HPP_
#define OBS_MULTISTREAM_FRONTEND_OAUTH_FACEBOOK_PROVIDER_HPP_

#include <cstddef>
#include <map>
#include <memory>
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
	// What this destination's live video actually holds -- title, description, and the `status`
	// that decides whether it is published to the Page at all, mapped back onto the descriptor's
	// own privacy choices. Reads the same node the viewer poll does, so the request shape is one
	// this provider already relies on. Category is not read back: content_tags comes back as a
	// resolved object rather than the interest id that was sent, so a comparison would be
	// between two different things.
	bool readAppliedMetadata(OAuthAccount &acct, const std::string &profileUuid, AppliedBy by, AppliedState &out,
				 std::string &err) override;
	// Where a privacy divergence leaves the user once the corrective push below has been tried and
	// the visibility that was asked for has still not been seen on the Page. Whether Meta accepts
	// `status` on an existing live video is undocumented -- the node reference has answered 404
	// since early 2025 -- so this offers the two routes that do not depend on the answer.
	std::string divergenceRemedy(const std::string &field) const override;
	// The account's Pages, filtered by `query` -- this provider's `page` picker, not a
	// game/category catalog. Facebook Gaming was sunset and no browsable category
	// taxonomy replaced it, so `page` is the only lookup field in the descriptor and this
	// hook has exactly one meaning here.
	bool searchCategories(OAuthAccount &acct, const std::string &query, json &out, std::string &err) override;
	bool applyMetadata(OAuthAccount &acct, const std::string &profileUuid, const json &fields, bool goingLive,
			   std::string &err) override;

	// The ordinary edit sends title and description but no `status`, so repeating it could never
	// correct a visibility that read back wrong. This one carries `status` as well, keeping the
	// contract that every field readAppliedMetadata can report is a field the corrective push
	// carries -- see the note in the body for why it is sent as its own request.
	bool reapplyMetadata(OAuthAccount &acct, const std::string &profileUuid, const json &fields,
			     std::string &err) override;

	// A live video is created per stream profile, so all per-broadcast state keys off the
	// destination -- two profiles on one Page are two independent broadcasts.
	bool broadcastPerDestination() const override { return true; }

	// Answered from liveVideos_, so the base prepareDestination decides create-vs-edit without
	// a Graph request for a destination the Go Live modal just prepared.
	bool hasActiveBroadcast(OAuthAccount &acct, const std::string &profileUuid) override;

	// One target per streamable Page: a Page has its own RTMPS endpoint, so two Pages
	// are two destinations, never two views of one. Reuses FetchPages, so the usable/
	// listed distinction it draws applies here unchanged -- a Page this app holds no
	// token for is not a place this account can stream to.
	bool enumerateTargets(OAuthAccount &acct, TargetList &out, std::string &err) override;

	// The Page field, without a platform call -- what a destination reads to learn which
	// Page it already claims.
	std::string targetFieldKey() const override;

	// Concurrent viewers for every live video this account currently holds, one row per
	// destination. The multi-destination hook rather than the per-channel one because a
	// Facebook broadcast IS per destination (see broadcastPerDestination above): two profiles
	// on two Pages are two live videos with two separate audiences, and a single figure
	// reported under the account would drop every one but the first while mis-attributing that
	// one to a destination nothing streams to.
	//
	// A live video that answers without a concurrent-viewer figure is left OUT of `out` rather
	// than written as zero. The poller reads absent and zero differently, and only absent is
	// honest about a destination that did not answer.
	bool viewerCounts(OAuthAccount &acct, std::map<DestinationId, int> &out, std::string &err) override;

	// The follower total of every Page this account's destinations claim, one row per
	// destination. The multi-destination hook rather than the per-channel one for the same
	// reason viewerCounts is: a Page is a destination, and two Pages have two separate
	// audiences that no single account-level figure can name.
	//
	// Off-air as much as on: the figure is read from the Page node itself, so it needs no
	// live video -- which is what makes it the poller's always-on read rather than a
	// streaming-only one.
	bool audienceCounts(OAuthAccount &acct, std::map<DestinationId, AudienceResult> &out,
			    std::string &err) override;

	// Mirror the account's Page claims into pageClaims_ below, so the poll worker can
	// address a destination without reaching for the UI-thread-only store that owns them.
	void noteTargetClaims(const std::string &accountId, std::map<std::string, std::string> claims) override;

	// This destination's live-comments transport. Both halves of its target come from the
	// two seams below rather than from the account, since a Page's comments are keyed by
	// the broadcast this provider created, not by the person who administers it.
	std::unique_ptr<Chat::ChatTransport> makeChat(const OAuthAccount &acct) override;

	// The live-video id the hub hands the chat transport as its channelRef; empty when
	// this destination is not broadcasting.
	std::string chatChannelRef(const OAuthAccount &acct, const std::string &profileUuid) override;

	// The Page access token that reads that live video's comments; empty on the same
	// terms. A SECOND narrow seam rather than a wider channelRef: channelRef is one
	// platform-specific string the hub logs and passes around, and packing a credential
	// into it would put a Page token on that path.
	std::string chatPageToken(const OAuthAccount &acct, const std::string &profileUuid) const;

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
		// Empty when Meta returns no picture for the Page. Optional by nature, so no
		// caller may treat it as required: the account keeps the profile avatar then.
		std::string avatarUrl;
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
	// This is also what the chat transport reads through chatChannelRef / chatPageToken:
	// the live comments of one broadcast are addressed by its live-video id and read with
	// the Page token that created it, both of which are held here and nowhere else.
	mutable std::mutex liveVideoMutex_;
	std::map<DestinationId, LiveVideo> liveVideos_;

	// Which Page each destination streams to, mirrored from the claim the stream-meta bag
	// owns. That bag REMAINS AUTHORITATIVE: this map is written from it and never read back
	// as the claim -- nothing but the audience poll may consult it, and nothing may resolve
	// a go-live from it. It exists only because the bag's store is UI-thread-only and
	// unguarded while the poll runs on a worker.
	//
	// Its own mutex rather than liveVideoMutex_, which is held across the go-live commit and
	// both stop hooks: a poll must not be able to wait behind a broadcast ending.
	//
	// noteTargetClaims replaces an account's whole set, so a claim cleared or a destination
	// deleted drops out here too. applyMetadata additionally records what a go-live resolved,
	// which covers the single-Page account whose bag never carried an explicit claim; a later
	// claim publish for that account drops it again, the bag having no claim to mirror.
	mutable std::mutex claimMutex_;
	std::map<DestinationId, std::string /*pageId*/> pageClaims_;
};

} // namespace OAuth

#endif // OBS_MULTISTREAM_FRONTEND_OAUTH_FACEBOOK_PROVIDER_HPP_
