#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_TRANSPORT_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_TRANSPORT_HPP_

#include <functional>
#include "../event_names.hpp"
#include <string>

#include <nlohmann/json.hpp>

#include "../events/transport_health.hpp"
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
//                 "id":     <string?>,     // stable platform user id; OMITTED when unknown
//                 "color":  <string>,      // "#RRGGBB" ("" if unset)
//                 "badges": [ { "kind": <string>, "url": <string?> } ] },
//     "fragments": [ { "type": "text",  "text": <string> }
//                  | { "type": "emote", "code": <string>, "url": <string> } ] }
//
// Connection-state payload (event "chat.state"):
//   { "event": "chat.state", "platform": <providerId>,
//     "connected": <bool>, "error": <string?> }
//
// A transport never sets "accountId"/"profileUuid" itself: the hub stamps the
// destination identity onto every frame at its single fan-out point (see ChatHub::Start),
// so the wire shape cannot drift per platform.
struct ChatContext {
	std::function<void(const json &payload)> emit;
	std::function<bool()> canceled;

	// Which destination this transport is reading. Needed by a transport that forwards
	// into another subsystem on its own (YouTube's live chat is the only push source for
	// Super Chats and ingests them straight into the EventHub), so the forwarded record can
	// name the account and broadcast it came from. `profileUuid` is empty for a per-channel
	// platform's account-wide destination.
	OAuth::DestinationId dest;

	// Report this transport's connection-health transition to the shared aggregator
	// (R14/G1). Always populated by the hub with the transport id bound in; transports
	// reach it through EmitChatState / EmitChatTerminal below, so a per-platform transport
	// never names an id or the aggregator. Guard `if (ctx.reportHealth)` for safety.
	std::function<void(Transports::TransportHealth::State state, const std::string &error)> reportHealth;
};

// Assemble the author object of the shape documented above. The ONE place the author keys
// are named, so the transports that assemble their own frame (Twitch, Kick) and the ones
// that go through BuildChatMessage cannot drift apart on it.
//
// `id` is the platform's stable per-user id, which a consumer keys a per-chatter tally on
// so a mid-stream display-name change stays one person and two people sharing a name stay
// two. An UNKNOWN id OMITS the key rather than writing "": an empty id is a value two
// different chatters would share, which is the misattribution the id exists to remove.
inline json BuildChatAuthor(const std::string &name, const std::string &id, const std::string &color,
			    const json &badges)
{
	json author = json{{"name", name}, {"color", color}, {"badges", badges}};
	if (!id.empty()) {
		author["id"] = id;
	}
	return author;
}

// Assemble one normalized chat.message frame from already-normalized parts. The ONE
// assembler for the shape documented above, so a platform reading the same chat over TWO
// different APIs (YouTube's official liveChatMessages surface and its InnerTube surface,
// whose payload schemas share nothing) cannot let the wire shape drift between them. The
// caller owns the per-schema decoding -- fragments, badges, author, timestamps -- and this
// owns only the frame.
inline json BuildChatMessage(const char *platform, const std::string &channelId, const std::string &id, int64_t ts,
			     const std::string &authorName, const std::string &authorId, const std::string &authorColor,
			     const json &badges, const json &fragments)
{
	return json{
		{"event", EventNames::kChatMessage},
		{"platform", platform},
		{"channelId", channelId},
		{"id", id},
		{"ts", ts},
		{"author", BuildChatAuthor(authorName, authorId, authorColor, badges)},
		{"fragments", fragments},
	};
}

// Emit one connection-state frame with a FIXED key set (event/platform/connected/
// error) every time, so the wire shape can't drift per platform or per call site
// (the drift this replaces: some sites omitted `error`, others always sent it). The
// shared body of both reports below, so the frame and the health hop stay one seam
// however a transport is reporting.
inline void EmitChatFrame(const ChatContext &ctx, const char *platform, bool connected,
			  Transports::TransportHealth::State health, const std::string &error)
{
	ctx.emit(json{{"event", EventNames::kChatState},
		      {"platform", platform},
		      {"connected", connected},
		      {"error", error}});
	if (ctx.reportHealth) {
		ctx.reportHealth(health, error);
	}
}

// A transport's ordinary state transition: connected, or dropped and about to retry
// (Reconnecting). `error` defaults to "" for the connected/success case. The Connecting
// bookend (pre-connect) and the Disconnected the hub writes on Stop() are the hub's, which
// owns that lifecycle.
inline void EmitChatState(const ChatContext &ctx, const char *platform, bool connected, const std::string &error = "")
{
	EmitChatFrame(ctx, platform, connected,
		      connected ? Transports::TransportHealth::State::Connected
				: Transports::TransportHealth::State::Reconnecting,
		      error);
}

// A read loop that has stopped FOR GOOD -- chat ended, authorization revoked, broadcast
// gone -- reports Failed, not Reconnecting: it is not retrying, and a row that claims it is
// leaves the user watching a chat that will never come back. Deliberately NOT the
// Disconnected the hub writes on Stop(): "this destination's chat died mid-session" and
// "the session ended" are different facts and both have to survive in the data. `reason`
// is the whole content of a terminal row, so never pass an empty one.
inline void EmitChatTerminal(const ChatContext &ctx, const char *platform, const std::string &reason)
{
	EmitChatFrame(ctx, platform, false, Transports::TransportHealth::State::Failed, reason);
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

	// Whether the platform's read transport reflects the sender's own outbound
	// messages back to us (so the chat pane shows them without a local echo).
	// Default false: echo locally so a sent message is never invisible.
	virtual bool reflectsOwnSend() const { return false; }

	// The channel id this transport's incoming chat.message frames carry, so the
	// hub's local echo (for a !reflectsOwnSend() platform) groups identically to
	// real messages in the frontend. "" until connect() has resolved it. Default ""
	// -- transports that reflect never need an echo, so they skip the override.
	virtual std::string channelId() const { return std::string(); }

	// Signal the read loop to stop and close any open sockets. May be called from a
	// different thread than connect()'s worker, so it must only flip a flag / shut
	// the socket so the worker's loop returns promptly -- the worker owns the actual
	// teardown (curl handles are not safe for concurrent use).
	virtual void disconnect() = 0;
};

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_TRANSPORT_HPP_
