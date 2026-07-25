#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_CHAT_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_CHAT_HPP_

#include <atomic>
#include <mutex>
#include <string>

#include "chat_transport.hpp"

// The YouTube live-chat transport (Phase 9.0). YouTube exposes live chat over the
// YouTube Data API v3: the push-based liveChatMessages.streamList (~1s latency, billed
// per connection) is the default read, with liveChatMessages.list (polling, billed per
// call, honoring the server-dictated pollingIntervalMillis + nextPageToken cursor) kept
// as the fallback for when the stream endpoint is unavailable or connects without
// delivering, plus liveChatMessages.insert (send). The read target is the active
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

	// Read loop: streamList by default, falling back to .list polling when the stream
	// endpoint is unavailable or delivers nothing (BRAIDCAST_YOUTUBE_STREAMLIST=false
	// forces .list), emitting only messages that arrive AFTER the cold connect (the first
	// response's backlog is dropped and only its cursor kept; subsequent reads resume from
	// the cursor and emit). Re-checks cancellation frequently via the poll/chunk callback
	// + CancelableSleep so a Stop() returns within ~0.5s. `channelRef` is the liveChatId;
	// empty (no active broadcast) is a clean no-op that returns false with an empty `err`.
	bool connect(const ChatContext &ctx, OAuth::OAuthAccount &acct, const std::string &channelRef,
		     std::string &err) override;

	// liveChatMessages.insert a textMessageEvent into THIS transport's broadcast chat --
	// the liveChatId connect() is reading, not whichever broadcast the account most
	// recently created. An account streaming two orientations has one transport per
	// broadcast, so re-resolving the target off the provider would post every reply into
	// whichever of them went live last.
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

	// The liveChatId connect() is currently reading, published for send() (which runs on a
	// different worker). Empty while not connected.
	mutable std::mutex targetMutex_;
	std::string liveChatId_;
};

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_CHAT_HPP_
