#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_TRANSPORT_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_TRANSPORT_HPP_

#include <functional>
#include "../event_names.hpp"
#include <string>

#include <nlohmann/json.hpp>

#include "../oauth/provider.hpp"

// The per-platform chat transport interface (Phase 9.0). One concrete transport
// per platform (Twitch IRC-over-WebSocket, YouTube liveChat poll, Kick Pusher)
// lives in its own file under frontend/src/chat/ and is constructed per account by
// that platform's StreamProvider (StreamProvider::makeChat), then owned by the
// ChatHub (as a shared_ptr shared with its worker) for the account's live session.
// The ChatHub runs each live transport on its own worker thread between go-live and
// stop, normalizes the stream into one model, and fans it to JS.
//
// A transport NEVER touches the ChatHub: the hub passes in the emit/cancel
// context and the platform-specific `channelRef` it resolved via
// StreamProvider::chatChannelRef, so adding a platform is purely one new file +
// the provider's makeChat() override.
namespace Chat {

using json = nlohmann::json;

// The runtime context the hub hands a transport for one live connection.
//
// `emit` pushes one payload toward JS. The payload MUST carry a top-level
// "event" string naming the bridge event ("chat.message" or "chat.state"); the
// hub strips it and forwards the remainder via the alive-guarded EmitEvent, so
// the hub stays free of per-platform / per-message-type branches.
//
// `canceled` returns true once go-live stops or the bridge tears down. The read
// loop MUST check it frequently (at least every ~0.5s) and bail promptly.
//
// Normalized message payload (event "chat.message"):
//   { "event": "chat.message",
//     "platform":  <providerId>,           // "twitch" | "youtube" | "kick"
//     "channelId": <string>,
//     "id":        <string>,               // platform message id
//     "ts":        <number>,               // epoch milliseconds
//     "author": { "name":   <string>,
//                 "color":  <string>,      // "#RRGGBB" ("" if unset)
//                 "badges": [ { "kind": <string>, "url": <string?> } ] },
//     "fragments": [ { "type": "text",  "text": <string> }
//                  | { "type": "emote", "code": <string>, "url": <string> } ] }
//
// Connection-state payload (event "chat.state"):
//   { "event": "chat.state", "platform": <providerId>,
//     "connected": <bool>, "error": <string?> }
struct ChatContext {
	std::function<void(const json &payload)> emit;
	std::function<bool()> canceled;
};

// Emit one connection-state frame with a FIXED key set (event/platform/connected/
// error) every time, so the wire shape can't drift per platform or per call site
// (the drift this replaces: some sites omitted `error`, others always sent it).
// `error` defaults to "" for the connected/success case.
inline void EmitChatState(const ChatContext &ctx, const char *platform, bool connected, const std::string &error = "")
{
	ctx.emit(json{{"event", EventNames::kChatState}, {"platform", platform}, {"connected", connected}, {"error", error}});
}

class ChatTransport {
public:
	virtual ~ChatTransport() = default;

	// Run the WHOLE read loop on the calling worker thread until ctx.canceled()
	// turns true or the connection drops / disconnect() is called; emit normalized
	// messages and state via ctx.emit. `channelRef` is the platform-specific
	// channel reference the hub resolved (login/slug for IRC/Pusher, liveChatId for
	// YouTube). `acct` is non-const so a reactive token refresh propagates back.
	// Returns false with `err` set on a fatal failure; a clean cancel returns false
	// with `err` possibly empty (the hub suppresses the log when canceled).
	virtual bool connect(const ChatContext &ctx, OAuth::OAuthAccount &acct, const std::string &channelRef,
			     std::string &err) = 0;

	// Post one message as `acct`. false + `err` on failure. `acct` is non-const for
	// the same refresh-propagation reason as connect.
	virtual bool send(OAuth::OAuthAccount &acct, const std::string &text, std::string &err) = 0;

	// Signal the read loop to stop and close any open sockets. May be called from a
	// different thread than connect()'s worker, so it must only flip a flag / shut
	// the socket so the worker's loop returns promptly -- the worker owns the actual
	// teardown (curl handles are not safe for concurrent use).
	virtual void disconnect() = 0;
};

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_TRANSPORT_HPP_
