#ifndef OBS_MULTISTREAM_FRONTEND_EVENTS_KICK_EVENTS_HPP_
#define OBS_MULTISTREAM_FRONTEND_EVENTS_KICK_EVENTS_HPP_

#include <atomic>
#include <mutex>
#include <string>

#include "../chat/ws_client.hpp"
#include "event_transport.hpp"

// The Kick Pusher event transport (Phase 9.2d): the always-on best-effort source
// for sub / gifted-sub / host(raid) / (rarely) follow. Owned by KickProvider and
// handed to the EventHub via StreamProvider::events(). connect() resolves the
// slug to its chatroom id + numeric channel id, opens ITS OWN Pusher socket
// (separate from kick_chat's -- events are account-lifecycle, chat is go-live, so
// when live there are two Pusher connections to Kick), subscribes to
// `chatrooms.<chatroomId>.v2` (sub/gift/host) and `channel.<channelId>` /
// `channel_<channelId>` (followers), and reads/normalizes frames until canceled.
//
// !!! REVERSE-ENGINEERED + BEST-EFFORT !!!
// Kick publishes NO desktop-usable official events API, so this rides Kick's public
// Pusher exactly as the web client does. The Pusher `App\Events\...` event names
// and payload shapes below are community-reverse-engineered (primary source:
// Bukk94/KickLib's KickClient event mapping) and CAN DRIFT WITHOUT NOTICE. Unknown
// event names (including chat messages, which arrive on the chatroom channel) are
// ignored; a name/shape change silently drops that event type rather than crashing.
//
// Mirrors kick_chat's Pusher handshake / ping-pong / double-decode / reconnect
// backoff, and TwitchEvents' worker/lock model on the shared EventHub: connect()
// runs on the EventHub worker, is serialized by runMutex_ (the hub does not join old
// workers, so an overlapping re-Start must not drive two loops), and every ws_
// access is serialized by wsMutex_ (libcurl easy handles are not concurrency-safe).
namespace OAuth {
class KickProvider;
}

namespace Events {

class KickEvents : public EventTransport {
public:
	explicit KickEvents(OAuth::KickProvider *provider) : provider_(provider) {}

	// Resolve ids, connect the Pusher socket, subscribe, and run the read loop
	// (reconnecting with backoff on a drop) until canceled. Returns false with empty
	// `err` on a clean cancel, false with `err` only on a fatal (no-WebSocket)
	// failure. No backfill()/poll() -- Kick's Pusher exposes no REST event history.
	bool connect(const EventContext &ctx, OAuth::OAuthAccount &acct, std::string &err) override;

	void disconnect() override;

private:
	OAuth::KickProvider *provider_; // owner; kept for symmetry with the other transports

	std::mutex runMutex_;              // serializes connect() across overlapping Start/Stop
	std::mutex wsMutex_;               // serializes every ws_ access (recv vs connect/close)
	Chat::WsClient ws_;                // the Pusher socket (guarded by wsMutex_)
	std::atomic<bool> stopped_{false}; // set by disconnect(); secondary to ctx.canceled()
};

} // namespace Events

#endif // OBS_MULTISTREAM_FRONTEND_EVENTS_KICK_EVENTS_HPP_
