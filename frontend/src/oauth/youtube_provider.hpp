#ifndef OBS_MULTISTREAM_FRONTEND_OAUTH_YOUTUBE_PROVIDER_HPP_
#define OBS_MULTISTREAM_FRONTEND_OAUTH_YOUTUBE_PROVIDER_HPP_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../events/youtube_events.hpp"
#include "../http_client.hpp"
#include "pkce_loopback.hpp"
#include "provider.hpp"

// The YouTube live-chat transport (Phase 9.0) is owned by the provider and held by
// unique_ptr to an incomplete type (its dtor is defined out-of-line so this header
// stays free of the chat include + the header cycle it would create).
namespace Chat {
class YouTubeChat;
}

// The YouTube stream provider: auth-code + PKCE over a loopback redirect (Google
// desktop clients ship a non-confidential secret that the token calls send when
// configured, plus access_type=offline + prompt=consent so a refresh token is
// issued) and the YouTube Data API v3 live lifecycle the Go Live modal drives.
// Unlike Twitch/Kick (which edit a persistent channel), YouTube creates a fresh
// broadcast + stream on every Go Live (create-per-go-live), binds them, writes the
// CDN ingest endpoint into the linked stream profile, then lets the encoder's
// connect auto-transition the broadcast to live. Endpoints verified against the
// YouTube Data API v3 reference (2026-06).
namespace OAuth {

// Bumped whenever the requested scope set changes, forcing installs holding
// tokens issued under an older scope set to re-auth (see OAuthAccount::scopeVer).
constexpr int YOUTUBE_SCOPE_VERSION = 1;

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
	bool applyMetadata(OAuthAccount &acct, const std::string &profileUuid, const json &fields,
			   std::string &err) override;

	// Report the active broadcast's concurrent viewer count (Phase 9.0). Returns
	// false (not live) when no broadcast is currently live; otherwise reads
	// videos.list liveStreamingDetails.concurrentViewers (absent -> 0).
	bool viewerCount(OAuthAccount &acct, int &out, std::string &err) override;

	// The YouTube live-chat transport, active only while a broadcast is live.
	Chat::ChatTransport *chat() override;

	// The YouTube REST event transport (Phase 9.2c): Super Chats / subscribers, run by
	// the EventHub on the account-connect lifecycle (real-time events arrive via chat).
	Events::EventTransport *events() override { return &events_; }

	// YouTube chat keys off the per-broadcast liveChatId (resolved in applyMetadata),
	// not the account login -- so override the default channel-ref resolution. Empty
	// when no broadcast is currently live, which the hub/transport treat as no chat.
	std::string chatChannelRef(const OAuthAccount &acct) override;

	// Zero the cached liveChatId/broadcastId (mutex-guarded) so a stream stop drops
	// the active-broadcast chat + viewer-count target. Called from streaming.stop.
	void clearActiveBroadcast() override;

	// True while YouTubeChat is actively polling a live broadcast's chat. During that
	// window the chat forward is the authoritative, real-time source of Super Chats, so
	// the REST event transport skips its superChatEvents.list query to avoid emitting a
	// second copy under a different id (the liveChatMessage id and superChatEvent id do
	// not match, so id-dedupe cannot collapse the pair). Set by YouTubeChat around its
	// poll loop; read by YouTubeEvents::collect. Atomic: written and read from different
	// worker threads with no other invariant to guard.
	void SetLiveChatActive(bool active) { liveChatActive_.store(active, std::memory_order_release); }
	bool LiveChatActive() const { return liveChatActive_.load(std::memory_order_acquire); }

private:
	// YouTubeChat reaches back through this provider for SendAuthed (token coherence)
	// and chatChannelRef (the active liveChatId), so it needs access to both.
	friend class Chat::YouTubeChat;

	// YouTubeEvents reuses SendAuthed for its Data API GETs (same refresh/401 path).
	friend class Events::YouTubeEvents;

	// Send an authenticated YouTube request: ensureFresh proactively, stamp the
	// bearer header, and on a 401 force one refresh + retry with the new token.
	// `req` is taken by value so headers are re-applied cleanly on the retry. false
	// + `err` only on a transport failure or an unrecoverable 401; an HTTP error
	// otherwise returns true with the status/body left for the caller to interpret.
	bool SendAuthed(OAuthAccount &acct, Http::HttpReq req, Http::HttpResponse &resp, std::string &err);

	PkceLoopbackStrategy auth_;

	std::unique_ptr<Chat::YouTubeChat> chat_;

	// The REST event transport. Value member (unlike chat_'s unique_ptr) because
	// youtube_events.hpp forward-declares this provider and only stores the pointer, so
	// including it creates no header cycle. It captures `this` at construction but only
	// stashes the pointer, so passing a not-yet-fully-constructed provider is safe.
	Events::YouTubeEvents events_{this};

	// The active broadcast's liveChatId + broadcast/video id, set on a successful
	// applyMetadata (the only place a broadcast is created) and read by the chat
	// transport (liveChatId_) and the viewer poller (broadcastId_). Guarded by the
	// same mutex because applyMetadata runs on a worker thread while chatChannelRef
	// and viewerCount are read from other threads. Empty when no broadcast is live.
	std::mutex liveChatMutex_;
	std::string liveChatId_;
	std::string broadcastId_;

	// The assignable videoCategories list, fetched once per process and reused
	// (it is static content). Guarded because searchCategories runs on worker
	// threads. Empty until the first successful fetch.
	std::mutex categoriesMutex_;
	std::vector<std::pair<std::string, std::string>> categories_; // {id, name}

	// See SetLiveChatActive: true only while the live-chat poll loop is running. This is
	// per-provider-singleton, which fits the current single-YouTube-account model; if
	// multi-account YouTube is ever enabled, one account going live would suppress REST
	// Super Chats for all YouTube accounts (revisit as a per-account flag then).
	std::atomic<bool> liveChatActive_{false};
};

} // namespace OAuth

#endif // OBS_MULTISTREAM_FRONTEND_OAUTH_YOUTUBE_PROVIDER_HPP_
