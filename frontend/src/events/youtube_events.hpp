#ifndef OBS_MULTISTREAM_FRONTEND_EVENTS_YOUTUBE_EVENTS_HPP_
#define OBS_MULTISTREAM_FRONTEND_EVENTS_YOUTUBE_EVENTS_HPP_

#include <atomic>
#include <string>
#include <vector>

#include "event_transport.hpp"

// The YouTube REST event transport (Phase 9.2c): Super Chats / Super Stickers /
// memberships / recent subscribers. YouTube has no real-time event socket -- the
// only push source is a live broadcast's chat, already polled by Chat::YouTubeChat,
// which forwards its event-type messages straight into Events::Hub().Ingest. This
// transport therefore covers the REST half, which works even when NOT live and so
// provides pre-live history:
//   - connect() no-ops (the real-time source is the chat sink);
//   - backfill() seeds recent Super Chats + subscribers once on connect;
//   - poll() re-fetches them on a ~90s cadence (the store dedupes, so overlap with
//     backfill and with the live chat forward is harmless).
// Constructed per account by YouTubeProvider::makeEvents and owned by the EventHub;
// reuses the provider's SendAuthed for the same proactive-refresh + reactive-401
// path the metadata/chat calls use.
namespace OAuth {
class YouTubeProvider;
}

namespace Events {

class YouTubeEvents : public EventTransport {
public:
	explicit YouTubeEvents(OAuth::YouTubeProvider *provider) : provider_(provider) {}

	// No real-time socket: return immediately. The hub loops connect() + poll(), so a
	// clean true here just hands control to poll() on the pollIntervalMs() cadence.
	bool connect(const EventContext &ctx, OAuth::OAuthAccount &acct, std::string &err) override;

	// One-shot REST seed: recent Super Chats + recent subscribers into `out`.
	bool backfill(const EventContext &ctx, OAuth::OAuthAccount &acct, std::vector<NormalizedEvent> &out,
		      std::string &err) override;

	// Periodic REST tick: the same two queries, emitted via ctx.emit (store dedupes).
	void poll(const EventContext &ctx, OAuth::OAuthAccount &acct) override;

	int pollIntervalMs() override { return 90000; }

	void disconnect() override { stopped_.store(true); }

private:
	// Shared body of backfill/poll: fetch recent Super Chats + subscribers, handing each
	// normalized event to `sink`. Cancel-aware between requests; non-2xx (past a handled
	// 401 refresh) is logged and skipped, never fatal. Used with an `out`-appending sink
	// for backfill and a ctx.emit sink for poll.
	void collect(const EventContext &ctx, OAuth::OAuthAccount &acct,
		     const std::function<void(NormalizedEvent &&)> &sink);

	OAuth::YouTubeProvider *provider_; // owner; used for the authed Data API GETs (SendAuthed)
	std::atomic<bool> stopped_{false}; // set by disconnect(); secondary to ctx.canceled()
};

} // namespace Events

#endif // OBS_MULTISTREAM_FRONTEND_EVENTS_YOUTUBE_EVENTS_HPP_
