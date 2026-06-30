#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_CHAT_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_CHAT_HPP_

#include <atomic>
#include <string>

#include "chat_transport.hpp"

// The YouTube live-chat transport (Phase 9.0). Unlike Twitch/Kick (WebSocket
// push), YouTube exposes live chat only as an HTTP long-poll over the YouTube
// Data API v3: liveChatMessages.list (read, honoring the server-dictated
// pollingIntervalMillis + nextPageToken cursor) and liveChatMessages.insert
// (send). The poll target is the active broadcast's `liveChatId`, which exists
// only while a broadcast is live -- the YouTubeProvider resolves it from the
// broadcast it created in applyMetadata (Phase 8d) and hands it in as the
// `channelRef`. All token coherence (proactive refresh + reactive-401
// force-refresh-and-retry) is delegated to YouTubeProvider::SendAuthed, so this
// transport carries no auth logic of its own.
namespace OAuth {
class YouTubeProvider;
}

namespace Chat {

class YouTubeChat : public ChatTransport {
public:
	explicit YouTubeChat(OAuth::YouTubeProvider &owner) : owner_(owner) {}

	// Long-poll loop: liveChatMessages.list with the rolling nextPageToken,
	// emitting only messages that arrive AFTER connect (the first response's
	// backlog is dropped and only its cursor kept). Sleeps the server's
	// pollingIntervalMillis between polls, re-checking cancellation each ~100ms via
	// CancelableSleep so a Stop() returns within ~0.5s. `channelRef` is the
	// liveChatId; empty (no active broadcast) is a clean no-op that returns false
	// with an empty `err`.
	bool connect(const ChatContext &ctx, OAuth::OAuthAccount &acct, const std::string &channelRef,
		     std::string &err) override;

	// liveChatMessages.insert a textMessageEvent into the active broadcast's chat.
	bool send(OAuth::OAuthAccount &acct, const std::string &text, std::string &err) override;

	// Flip the stop flag so the poll loop returns promptly (the worker that owns the
	// loop performs the actual teardown; nothing socket-bound is held here).
	void disconnect() override { stop_.store(true, std::memory_order_release); }

private:
	OAuth::YouTubeProvider &owner_;
	std::atomic<bool> stop_{false};
};

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_CHAT_HPP_
