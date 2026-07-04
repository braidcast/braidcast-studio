#ifndef OBS_MULTISTREAM_FRONTEND_EVENTS_TWITCH_EVENTS_HPP_
#define OBS_MULTISTREAM_FRONTEND_EVENTS_TWITCH_EVENTS_HPP_

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "../chat/ws_client.hpp"
#include "event_transport.hpp"

// The Twitch EventSub-over-WebSocket event transport (Phase 9.2b): the always-on
// real-time source for follow/sub/resub/subgift/cheer/raid. Constructed per account by
// TwitchProvider::makeEvents and owned by the EventHub. connect() opens a single
// socket to wss://eventsub.wss.twitch.tv/ws, reads the session_welcome to learn the
// session id, then POSTs one Helix subscription per event type bound to that session
// (attempt-and-skip on 401/403 so a missing scope degrades gracefully rather than
// aborting). It then reads notification frames, normalizes each by subscription type,
// and emits via ctx.emit until canceled or the socket drops. Mirrors TwitchChat's
// worker/lock/backoff model: connect() runs on the EventHub worker; every ws_ access
// is serialized by wsMutex_ (libcurl easy handles are not concurrency-safe) and
// connect() itself by runMutex_ so an overlapping re-Start never drives two loops
// over the same socket.
namespace OAuth {
class TwitchProvider;
}

namespace Events {

class TwitchEvents : public EventTransport {
public:
	explicit TwitchEvents(OAuth::TwitchProvider *provider) : provider_(provider) {}

	bool connect(const EventContext &ctx, OAuth::OAuthAccount &acct, std::string &err) override;
	bool backfill(const EventContext &ctx, OAuth::OAuthAccount &acct, std::vector<NormalizedEvent> &out,
		      std::string &err) override;
	void disconnect() override;

private:
	// Create every desired EventSub subscription bound to `sessionId` for
	// `broadcasterId`, one Helix POST each. Attempt-and-skip: a 401/403 (dead token /
	// missing scope) or a transport failure is logged and skipped, never fatal, so the
	// unscoped channel.raid subscription still lights up while the scoped ones wait for
	// the user's re-auth. Polls `canceled` between POSTs to bail promptly on Stop.
	void createSubscriptions(OAuth::OAuthAccount &acct, const std::string &broadcasterId,
				 const std::string &sessionId, const std::function<bool()> &canceled);

	OAuth::TwitchProvider *provider_; // owner; used for the authed Helix POST/GET (SendAuthed)

	std::mutex runMutex_;              // serializes connect() across overlapping Start/Stop
	std::mutex wsMutex_;               // serializes every ws_ access (recv vs connect/close)
	Chat::WsClient ws_;                // the EventSub socket (guarded by wsMutex_)
	std::atomic<bool> stopped_{false}; // set by disconnect(); secondary to ctx.canceled()
};

} // namespace Events

#endif // OBS_MULTISTREAM_FRONTEND_EVENTS_TWITCH_EVENTS_HPP_
