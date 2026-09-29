#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_CHAT_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_CHAT_HPP_

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../events/event_model.hpp" // Events::NormalizedEvent
#include "chat_archive.hpp"          // ModerationOp
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
// same broadcast's video id, taken from the provider's chatBroadcastRef -- which resolves that
// broadcast's privacy in the same lookup, because a PRIVATE broadcast is invisible to the free
// reader and is refused rather than read on the billed pair below. All token coherence
// (proactive refresh + reactive-401 force-refresh-and-retry) for the Data API reads is
// delegated to YouTubeProvider::SendAuthed / SendAuthedStreaming, so this transport carries no
// auth logic of its own -- and the InnerTube read deliberately carries no credential at all.
namespace OAuth {
class YouTubeProvider;
}

namespace Chat {

// One liveChatMessages item read as a moderator's removal, or nothing when it is not one.
// A tombstone deletes the message whose place it holds: its own `id`, the item `id` a frame
// from this read carries. (messageDeletedEvent, the old per-deletion item, is no longer
// returned by the API, so a live delete is not seen on this read; a tombstone arrives only
// where the deleted message would have been listed.) userBannedEvent, temporary or
// permanent, removes the lines of userBannedDetails.bannedUserDetails.channelId, which is the
// frames' `author.id` (authorDetails.channelId), and carries `beforeTs` from the item's
// snippet.publishedAt when that parses (none when it does not). A delete, exact by id, carries
// no time. The op's `dest` is left empty for the hub to fill in. Pure.
std::optional<ModerationOp> DecodeYouTubeModerationItem(const json &item);

// One liveChatMessageListResponse's items[]: each chat line to ctx.emit and, in addition, each
// monetization/membership item to `emitEvent`; each removal to ctx.emitModeration, in item
// order. A `backlog` response (the first after connecting) emits no line and no event, but
// its removals still apply -- they name lines this destination may already hold, read before
// a handover from InnerTube or stored by an earlier launch -- except an author-wide one with
// no time of its own (SafeToReplay). Read live, an op's time is dropped: the seq bound it gets
// when applied is already exact. Runs on the read worker.
void ProcessYouTubeChatItems(const ChatContext &ctx, const json &items, const std::string &liveChatId,
			     const std::unordered_map<std::string, std::string> &thirdPartyEmotes,
			     const std::function<bool()> &canceled, bool backlog,
			     const std::function<void(Events::NormalizedEvent &ev)> &emitEvent);

class YouTubeChat : public ChatTransport {
public:
	explicit YouTubeChat(OAuth::YouTubeProvider &owner) : owner_(owner) {}

	// Read loop: walks the endpoint ladder above, first enabled path first
	// (BRAIDCAST_YOUTUBE_INNERTUBE=false drops to the Data API,
	// BRAIDCAST_YOUTUBE_STREAMLIST=false additionally forces .list). Every path emits only
	// messages that arrive AFTER the cold connect -- the first response's backlog is dropped
	// and only its cursor and its removals kept -- and every path shares ONE session, so the connected state
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

	// Live polls in THIS transport's broadcast chat, for the same reason send() posts there:
	// create is liveChatMessages.insert of a pollEvent, end is liveChatMessages.transition to
	// closed with part=snippet so the response carries the final tallies.
	bool createPoll(OAuth::OAuthAccount &acct, const std::string &question, const std::vector<std::string> &options,
			json &poll, std::string &err) override;
	bool endPoll(OAuth::OAuthAccount &acct, const std::string &pollId, json &poll, std::string &err) override;

	// Every read path returns EVERY message in the chat -- including ones this
	// account inserted via send() -- so the read loop already emits the sender's
	// own messages (on the next poll). A local echo would double them.
	bool reflectsOwnSend() const override { return true; }

	// Flip the stop flag so the poll loop returns promptly (the worker that owns the
	// loop performs the actual teardown; nothing socket-bound is held here).
	void disconnect() override { stop_.store(true, std::memory_order_release); }

private:
	// The liveChatId connect() is reading, "" while not connected. The one read of the
	// published target, shared by every call that writes into this broadcast's chat.
	std::string CurrentLiveChatId() const;

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
