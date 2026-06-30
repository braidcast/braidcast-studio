#ifndef OBS_MULTISTREAM_FRONTEND_OAUTH_YOUTUBE_PROVIDER_HPP_
#define OBS_MULTISTREAM_FRONTEND_OAUTH_YOUTUBE_PROVIDER_HPP_

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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
	bool applyMetadata(OAuthAccount &acct, const json &fields, std::string &err) override;

	// The YouTube live-chat transport, active only while a broadcast is live.
	Chat::ChatTransport *chat() override;

	// YouTube chat keys off the per-broadcast liveChatId (resolved in applyMetadata),
	// not the account login -- so override the default channel-ref resolution. Empty
	// when no broadcast is currently live, which the hub/transport treat as no chat.
	std::string chatChannelRef(const OAuthAccount &acct) override;

private:
	// YouTubeChat reaches back through this provider for SendAuthed (token coherence)
	// and chatChannelRef (the active liveChatId), so it needs access to both.
	friend class Chat::YouTubeChat;

	// Send an authenticated YouTube request: ensureFresh proactively, stamp the
	// bearer header, and on a 401 force one refresh + retry with the new token.
	// `req` is taken by value so headers are re-applied cleanly on the retry. false
	// + `err` only on a transport failure or an unrecoverable 401; an HTTP error
	// otherwise returns true with the status/body left for the caller to interpret.
	bool SendAuthed(OAuthAccount &acct, Http::HttpReq req, Http::HttpResponse &resp, std::string &err);

	PkceLoopbackStrategy auth_;

	std::unique_ptr<Chat::YouTubeChat> chat_;

	// The active broadcast's liveChatId, set on a successful applyMetadata (the only
	// place a broadcast is created) and read by the chat transport. Guarded because
	// applyMetadata runs on a worker thread while chatChannelRef is read from the
	// ChatHub's start path. Empty when no broadcast is live.
	std::mutex liveChatMutex_;
	std::string liveChatId_;

	// The assignable videoCategories list, fetched once per process and reused
	// (it is static content). Guarded because searchCategories runs on worker
	// threads. Empty until the first successful fetch.
	std::mutex categoriesMutex_;
	std::vector<std::pair<std::string, std::string>> categories_; // {id, name}
};

} // namespace OAuth

#endif // OBS_MULTISTREAM_FRONTEND_OAUTH_YOUTUBE_PROVIDER_HPP_
