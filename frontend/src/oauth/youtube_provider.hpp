#ifndef OBS_MULTISTREAM_FRONTEND_OAUTH_YOUTUBE_PROVIDER_HPP_
#define OBS_MULTISTREAM_FRONTEND_OAUTH_YOUTUBE_PROVIDER_HPP_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../events/youtube_events.hpp"
#include "util/http_client.hpp"
#include "broker_strategy.hpp"
#include "provider.hpp"

// The YouTube live-chat transport (Phase 9.0) is constructed per account in
// makeChat(), defined out-of-line in the .cpp where the complete type is available so
// this header stays free of the chat include + the header cycle it would create. Only
// forward-declared here for the friend grant below.
namespace Chat {
class YouTubeChat;
}

// The YouTube stream provider: auth-code + PKCE over a loopback redirect (Google
// desktop clients ship a non-confidential secret that the token calls send when
// configured, plus access_type=offline + prompt=consent so a refresh token is
// issued) and the YouTube Data API v3 live lifecycle the Go Live modal drives.
// Unlike Twitch/Kick (which edit a persistent channel), YouTube creates a fresh
// broadcast on every Go Live (create-per-go-live), binds it to the account's
// reusable ingest stream (created once, remembered in the account store,
// re-verified each go-live), writes the CDN ingest endpoint into the linked stream
// profile, then lets the encoder's connect auto-transition the broadcast to live.
// Endpoints verified against the YouTube Data API v3 reference (2026-06).
namespace OAuth {

// Bumped whenever the requested scope set changes, forcing installs holding
// tokens issued under an older scope set to re-auth (see OAuthAccount::scopeVer).
constexpr int YOUTUBE_SCOPE_VERSION = 1;

// error.errors[0].reason from a YouTube Data API error body ("" when absent /
// unparseable). The ONE reason extractor every YouTube 403 interpreter shares.
std::string YouTubeErrorReason(const std::string &body);

// The two transient YouTube 403/429 classes, split because they demand opposite
// handling: a rate limit subsides within seconds (backoff and retry), while an
// exhausted daily quota lasts until the next midnight Pacific reset (stand down --
// retrying sooner is guaranteed-useless traffic). Everything else is Other.
enum class YouTubeErrorClass { Other, RateLimited, QuotaExhausted };
YouTubeErrorClass ClassifyYouTubeError(long status, const std::string &reason);

class YouTubeProvider : public StreamProvider {
public:
	YouTubeProvider();
	~YouTubeProvider() override;

	std::string id() const override { return "youtube"; }
	std::string displayName() const override { return "YouTube"; }
	std::string brandColor() const override { return "#ff4e45"; }
	int scopeVer() const override { return YOUTUBE_SCOPE_VERSION; }

	json capabilityJson() const override;

	AuthStrategy *auth() override { return &auth_; }

	bool fetchIdentity(OAuthAccount &acct, std::string &err) override;
	bool getMetadata(OAuthAccount &acct, json &out, std::string &err) override;
	bool searchCategories(OAuthAccount &acct, const std::string &query, json &out, std::string &err) override;
	bool applyMetadata(OAuthAccount &acct, const std::string &profileUuid, const json &fields, bool goingLive,
			   std::string &err) override;

	// YouTube creates one broadcast per stream profile, each with its own liveChatId and
	// its own concurrentViewers, so all per-broadcast state keys off the destination.
	bool broadcastPerDestination() const override { return true; }

	// One concurrent-viewer row per live broadcast of `acct` (videos.list
	// liveStreamingDetails.concurrentViewers; absent -> 0). Reading N broadcasts costs N
	// videos.list units, which is the price of not under-reporting; the read set is the
	// account's cached broadcasts deduped by broadcastId, so it never exceeds the number of
	// distinct live broadcasts. The per-channel viewerCount hook is deliberately NOT
	// overridden -- YouTube has no single per-account viewer figure to report.
	bool viewerCounts(OAuthAccount &acct, std::map<DestinationId, int> &out, std::string &err) override;

	// Report the channel's subscriber total (channels.list statistics). Reuses the
	// identity `channels` scope -- no new grant. hiddenSubscriberCount -> hidden=true
	// with count=-1; a successful read is authoritative even when hidden.
	bool audienceCount(OAuthAccount &acct, AudienceResult &out, std::string &err) override;

	// A fresh YouTube live-chat transport for `acct`, active only while a broadcast is
	// live for that account.
	std::unique_ptr<Chat::ChatTransport> makeChat(const OAuthAccount &acct) override;

	// A fresh YouTube REST event transport (Phase 9.2c): Super Chats / subscribers, run
	// by the EventHub on the account-connect lifecycle (real-time events arrive via chat).
	std::unique_ptr<Events::EventTransport> makeEvents(const OAuthAccount &acct) override;

	// YouTube chat keys off the per-broadcast liveChatId (resolved in applyMetadata for
	// `profileUuid`), not the account login -- so override the default channel-ref
	// resolution. Empty when no broadcast is currently live for that destination, which
	// the hub/transport treat as no chat.
	std::string chatChannelRef(const OAuthAccount &acct, const std::string &profileUuid) override;

	// Zero EVERY cached liveChatId/broadcastId belonging to `accountId` (mutex-guarded) so
	// a stream stop drops all of that account's active-broadcast chat + viewer-count
	// targets. Called per connected account from streaming.stop.
	void clearActiveBroadcast(const std::string &accountId) override;
	void clearActiveBroadcastDestination(const DestinationId &dest) override;

	// Balanced hold on "this DESTINATION's live chat is being read", one per running chat
	// poll loop. While a destination's chat is being forwarded, that forward is the
	// authoritative real-time source of its Super Chats.
	//
	// A REFCOUNT, not a flag: the chat hub does not join a stopped loop, so an overlapping
	// re-Start can briefly leave two loops on one destination, and a flag would let the
	// first to exit clear a hold the other still owns. Acquire/release must be balanced;
	// YouTubeChat owns exactly one hold per connect().
	//
	// Keyed per DESTINATION even though the read it gates is channel-wide: the gate is a
	// COVERAGE question ("is every live broadcast's chat being read"), which an account-wide
	// count cannot answer -- one orientation's loop made the count non-zero while a sibling
	// orientation's Super Chats reached neither the forward nor the REST poll.
	void AddLiveChatRef(const DestinationId &dest);
	void ReleaseLiveChatRef(const DestinationId &dest);

	// Should the REST event transport spend a superChatEvents.list request for `accountId`
	// this pass? Two gates. The request is only 1 unit, so this is not about its unit price --
	// ungated it ran on a fixed cycle for every connected account whether or not anything was
	// streaming, which is spend that can never return a result:
	//
	//   1) No live broadcast for the account -> NO. A Super Chat exists only inside a live
	//      chat, so a channel that is not broadcasting has nothing this read can return.
	//   2) Live -> only when some live destination's chat is NOT being forwarded. With every
	//      live destination covered the forward already delivers each Super Chat in real
	//      time, and the REST copy carries a different resource id (superChatEvent vs
	//      liveChatMessage), so running both double-emits. The moment one live destination
	//      lacks a chat loop, its Super Chats reach NEITHER path -- so the read must run.
	//
	// Stays ONE account-wide request whatever the answer: superChatEvents.list is a
	// channel-wide read, so a per-destination poll would multiply an identical result.
	bool ShouldPollSuperChats(const std::string &accountId) const;

	// Record a quota-exhausted verdict from any YouTube Data API response: computes
	// the next midnight-Pacific reset instant and closes the shared gate until then,
	// HostLog'ing once per episode. Later reports while the gate is closed are no-ops.
	// Armed by the SendAuthed/SendAuthedStreaming wrappers below, so one 403 from any
	// consumer teaches the whole app to stand down.
	void NoteQuotaExhausted(const std::string &reason);

	// True while the daily quota is exhausted; clears itself once the reset instant
	// passes (no manual reset, no restart). Fills `retryIn` with the remaining wait
	// when exhausted, so loops can sleep out the outage instead of spinning.
	bool QuotaExhausted(std::chrono::milliseconds *retryIn = nullptr) const;

	// The recorded reset instant as local wall-clock "HH:MM" for user-facing
	// messages ("" before any exhaustion was ever recorded).
	std::string QuotaResetLocalTime() const;

	// The quota gate's choke point: every YouTube consumer (chat, events, viewer +
	// audience pollers, broadcast probe, metadata) sends through these two, so while
	// exhausted the request is refused without spending quota, and a quota-class 403
	// on any response arms the gate for all of them. Twitch/Kick keep the base
	// implementations untouched.
	bool SendAuthed(OAuthAccount &acct, Http::HttpReq req, Http::HttpResponse &resp, std::string &err) override;
	long SendAuthedStreaming(OAuthAccount &acct, Http::HttpReq req,
				 const std::function<bool(std::string_view chunk)> &onChunk, std::string &errorBody,
				 std::string &err) override;

private:
	// YouTubeChat reaches back through this provider for SendAuthed (token coherence)
	// and chatChannelRef (the active liveChatId), so it needs access to both.
	friend class Chat::YouTubeChat;

	// YouTubeEvents reuses SendAuthed for its Data API GETs (same refresh/401 path).
	friend class Events::YouTubeEvents;

	BrokerStrategy auth_;

	// The active broadcast's liveChatId + broadcast/video id, PER DESTINATION. Set on a
	// successful applyMetadata (the only place a broadcast is created) and read by the
	// chat transport (liveChatId) and the viewer poller (broadcastId). Guarded by
	// broadcastMutex_ because applyMetadata runs on a worker thread while chatChannelRef
	// and viewerCounts are read from other threads.
	//
	// Keyed by (accountId, profileUuid), not accountId: YouTube creates a fresh broadcast
	// per stream profile, so an account streaming two orientations holds two live
	// broadcasts at once, with different liveChatIds and separate viewer figures. Keying
	// by account alone made the second go-live's apply overwrite the first's ids, which
	// silently dropped one orientation's chat and its viewers. A destination is absent when
	// no broadcast is live for it.
	struct BroadcastState {
		std::string liveChatId;
		std::string broadcastId;
	};
	mutable std::mutex broadcastMutex_;
	std::map<DestinationId, BroadcastState> broadcasts_;

	// Cache-miss recovery for `broadcasts_` (e.g. after a Studio restart mid-stream, or a
	// broadcast started outside this session): ONE account-wide liveBroadcasts.list that
	// repopulates EVERY destination of the account at once, including liveChatId.
	//
	// Attribution is exact, not a guess. Each destination owns its own reusable ingest
	// stream (OAuthAccount::reusableStreamIds, persisted), a broadcast is bound to exactly
	// one stream, and the probe reads contentDetails.boundStreamId -- so a returned
	// broadcast maps to one destination unambiguously even with several orientations live.
	// A broadcast bound to a stream this app does not remember is skipped rather than
	// misattributed to whichever destination happened to ask.
	//
	// Costs 1 unit per call regardless of how many broadcasts come back (parts are free),
	// and is throttled per ACCOUNT by lastBroadcastProbe_ so several destinations missing
	// the cache together still trigger a single probe.
	bool ProbeActiveBroadcasts(OAuthAccount &acct, std::string &err);
	std::map<std::string /*accountId*/, std::chrono::steady_clock::time_point> lastBroadcastProbe_;

	// One destination's live broadcast: the cache, else one shared probe. False when that
	// destination is not live (even if a sibling destination is).
	bool EnsureActiveBroadcast(OAuthAccount &acct, const std::string &profileUuid, BroadcastState &out,
				   std::string &err);

	// videos.list liveStreamingDetails.concurrentViewers for a BATCH of broadcasts: the single
	// viewer read. `out` is keyed by video id and gains an entry ONLY for an id the response
	// returned, so an omitted broadcast stays absent instead of reading as zero viewers.
	// Appends to `out` rather than replacing it, so several chunks accumulate.
	bool ReadBroadcastViewers(OAuthAccount &acct, const std::vector<std::string> &broadcastIds,
				  std::map<std::string, int> &out, std::string &err);

	// The assignable videoCategories list, fetched once per process and reused
	// (it is static content). Guarded because searchCategories runs on worker
	// threads. Empty until the first successful fetch.
	std::mutex categoriesMutex_;
	std::vector<std::pair<std::string, std::string>> categories_; // {id, name}

	// See AddLiveChatRef: per-destination count of running live-chat poll loops. Its own
	// mutex rather than broadcastMutex_ -- the two protect unrelated state and the chat loops
	// take this one on every announce while metadata applies hold the other. ShouldPollSuperChats
	// reads both, snapshotting under one and releasing it before taking the other, so the two
	// are never nested and no lock order exists to invert.
	mutable std::mutex liveChatMutex_;
	std::map<DestinationId, int> liveChatRefs_;

	// Arm the quota gate when a completed response carries a quota-class 403.
	// Shared tail of the SendAuthed/SendAuthedStreaming wrappers.
	void NoteIfQuotaError(long status, const std::string &body);

	// The refusal message both wrappers hand back while the gate is closed.
	std::string QuotaMessage() const;

	// Epoch seconds when the exhausted daily quota resets (0 = never exhausted).
	// Per-provider-singleton on purpose: the quota is spent per API project, not per
	// account, so one exhausted account means every YouTube account is dark. Atomic:
	// armed and read from many worker threads (chat, events, pollers).
	// Mutable because the lazy seed below runs from the const quota queries: restoring a
	// persisted verdict is cache-filling, not a change of observable state.
	mutable std::atomic<int64_t> quotaResetEpoch_{0};

	// Seed quotaResetEpoch_ from the persisted per-account records, once, on first quota
	// query. Lazy rather than in the constructor: the provider is built during registry
	// setup, before the config path is necessarily resolvable, and this way the store read
	// costs nothing until something actually asks about quota.
	void EnsureQuotaStateLoaded() const;
	mutable std::once_flag quotaLoadOnce_;
};

} // namespace OAuth

#endif // OBS_MULTISTREAM_FRONTEND_OAUTH_YOUTUBE_PROVIDER_HPP_
