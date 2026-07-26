#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_CHAT_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_CHAT_HPP_

#include <atomic>
#include <mutex>
#include <string>

#include "chat_transport.hpp"

// The YouTube live-chat transport (Phase 9.0). It reads one broadcast's chat over a LADDER of
// endpoints, each handing over to the next when its own becomes unusable:
//
//   1. InnerTube live_chat/get_live_chat (see youtube_innertube) -- the primary read, because
//      it costs ZERO quota. The Data API reads below bill against ONE Cloud project's
//      10,000-unit daily budget shared by every install, which a single user streaming
//      continuously to a few destinations exhausts many times over.
//   2. liveChatMessages.streamList -- push-based, billed per connection.
//   3. liveChatMessages.list -- polling, billed per call, honoring the server-dictated
//      pollingIntervalMillis + nextPageToken cursor. The terminal fallback.
//
// The Data API pair stays behind InnerTube rather than being retired: they are authoritative
// on WHY a chat ended, and they are the only surface that can read member-only chat.
// liveChatMessages.insert remains the send path regardless of which read is active.
//
// The read target is the active broadcast's `liveChatId`, which exists only while a broadcast
// is live -- the YouTubeProvider resolves it from the broadcast it created in applyMetadata
// (Phase 8d) and hands it in as the `channelRef`; the InnerTube read additionally needs that
// same broadcast's video id, taken from the provider's chatVideoRef. All token coherence
// (proactive refresh + reactive-401 force-refresh-and-retry) for the Data API reads is
// delegated to YouTubeProvider::SendAuthed / SendAuthedStreaming, so this transport carries no
// auth logic of its own -- and the InnerTube read deliberately carries no credential at all.
namespace OAuth {
class YouTubeProvider;
}

namespace Chat {

class YouTubeChat : public ChatTransport {
public:
	explicit YouTubeChat(OAuth::YouTubeProvider &owner) : owner_(owner) {}

	// Read loop: walks the endpoint ladder above, first enabled path first
	// (BRAIDCAST_YOUTUBE_INNERTUBE=false drops to the Data API,
	// BRAIDCAST_YOUTUBE_STREAMLIST=false additionally forces .list). Every path emits only
	// messages that arrive AFTER the cold connect -- the first response's backlog is dropped
	// and only its cursor kept -- and every path shares ONE session, so the connected state
	// and this destination's live-chat refcount hold survive a handover exactly once.
	// Re-checks cancellation frequently via the poll/chunk callback + CancelableSleep so a
	// Stop() returns within ~0.5s. `channelRef` is the liveChatId; empty (no active
	// broadcast) is a clean no-op that returns false with an empty `err`.
	bool connect(const ChatContext &ctx, OAuth::OAuthAccount &acct, const std::string &channelRef,
		     std::string &err) override;

	// liveChatMessages.insert a textMessageEvent into THIS transport's broadcast chat --
	// the liveChatId connect() is reading, not whichever broadcast the account most
	// recently created. An account streaming two orientations has one transport per
	// broadcast, so re-resolving the target off the provider would post every reply into
	// whichever of them went live last.
	bool send(OAuth::OAuthAccount &acct, const std::string &text, std::string &err) override;

	// Every read path returns EVERY message in the chat -- including ones this
	// account inserted via send() -- so the read loop already emits the sender's
	// own messages (on the next poll). A local echo would double them.
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
