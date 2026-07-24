#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_CHAT_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_CHAT_HPP_

#include <atomic>
#include <mutex>
#include <string>

#include "chat_transport.hpp"

// The YouTube live-chat transport (Phase 9.0). YouTube exposes live chat over the
// YouTube Data API v3: liveChatMessages.streamList (read -- a push-based server
// stream over REST, ~1s latency, far lower quota than polling) with
// liveChatMessages.list as the fallback when the streamList transcode endpoint is
// unavailable (honoring the server-dictated pollingIntervalMillis + nextPageToken
// cursor), and liveChatMessages.insert (send). The read target is the active
// broadcast's `liveChatId`, which exists only while a broadcast is live -- the
// YouTubeProvider resolves it from the broadcast it created in applyMetadata (Phase
// 8d) and hands it in as the `channelRef`. All token coherence (proactive refresh +
// reactive-401 force-refresh-and-retry) is delegated to YouTubeProvider::SendAuthed
// / SendAuthedStreaming, so this transport carries no auth logic of its own.
namespace OAuth {
class YouTubeProvider;
}

namespace Chat {

class YouTubeChat : public ChatTransport {
public:
	explicit YouTubeChat(OAuth::YouTubeProvider &owner) : owner_(owner) {}

	// Streaming read loop: liveChatMessages.streamList with the rolling nextPageToken
	// (falling back to .list polling if the stream endpoint is unavailable), emitting
	// only messages that arrive AFTER the cold connect (the first response's backlog is
	// dropped and only its cursor kept; reconnects resume from the cursor and emit).
	// Reconnects promptly on a clean server close, re-checking cancellation frequently
	// via the stream's chunk callback / CancelableSleep so a Stop() returns within
	// ~0.5s. `channelRef` is the liveChatId; empty (no active broadcast) is a clean
	// no-op that returns false with an empty `err`.
	bool connect(const ChatContext &ctx, OAuth::OAuthAccount &acct, const std::string &channelRef,
		     std::string &err) override;

	// liveChatMessages.insert a textMessageEvent into the active broadcast's chat.
	bool send(OAuth::OAuthAccount &acct, const std::string &text, std::string &err) override;

	// liveChatMessages.list returns EVERY message in the chat -- including ones
	// this account inserted via send() -- so the poll loop already emits the
	// sender's own messages (on the next poll). A local echo would double them.
	bool reflectsOwnSend() const override { return true; }

	// Flip the stop flag so the poll loop returns promptly (the worker that owns the
	// loop performs the actual teardown; nothing socket-bound is held here).
	void disconnect() override { stop_.store(true, std::memory_order_release); }

private:
	OAuth::YouTubeProvider &owner_;
	std::mutex runMutex_;           // serializes connect() across overlapping Start/Stop
	std::atomic<bool> stop_{false}; // set by disconnect(); secondary to ctx.canceled()
};

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_CHAT_HPP_
