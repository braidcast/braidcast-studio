#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_KICK_PUSHER_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_KICK_PUSHER_HPP_

#include <string>

// Shared Kick/Pusher connection config + channel-id lookup, factored out of
// kick_chat.cpp so the chat transport (chatroom messages) and the events transport
// (subscriptions/gifts/host/followers) reference ONE copy of the app key/cluster
// and ONE lookup rather than each hardcoding their own.
//
// REVERSE-ENGINEERED from the Kick web client -- NOT an official/published Kick API.
// These values (app key, cluster host, client version) and the /api/v2 lookup shape
// can change without notice; this pair of files is the single edit point for both
// Kick transports. Sources (research note 2026-06-30 §1 + KickLib): the Kick web
// client's Pusher handshake; Bukk94/KickLib; devozdemirhasancan gist;
// SongoMen/kick-chat-wrapper; caesarakalaeii/all-chat.
namespace Chat {

// Kick's Pusher app key + "us2" cluster host + client version. If the handshake
// stops yielding "pusher:connection_established", the key/cluster most likely
// rotated (each transport logs a clear diagnostic making that detectable).
inline constexpr const char *kKickPusherAppKey = "32cbd69e4b950bf97679";
inline constexpr const char *kKickPusherHost = "ws-us2.pusher.com"; // "us2" cluster
inline constexpr const char *kKickPusherClientVersion = "8.4.0-rc2";

// The unofficial kick.com/api/v2 lookup declares the app's canonical browser
// User-Agent (InnerTube::BrowserUserAgent) -- the research flags that bot-detection
// TLS/UA fingerprinting may block non-browser clients on that internal endpoint, and a
// UA hardcoded here decayed into claiming a Chrome nobody runs while the embedded CEF
// moved on.

// The Pusher app WebSocket URL built from the shared app key/host/version.
std::string KickPusherUrl();

// Pusher's own protocol frames (as opposed to Kick's `App\Events\...` application
// events, which are per-transport). Both Kick transports run the identical handshake,
// so the names they dispatch on live here with the key/cluster they arrive over.
inline constexpr const char *kPusherConnectionEstablished = "pusher:connection_established";
inline constexpr const char *kPusherSubscriptionSucceeded = "pusher_internal:subscription_succeeded";
inline constexpr const char *kPusherPing = "pusher:ping";
inline constexpr const char *kPusherError = "pusher:error";

// The app-level pong answering kPusherPing -- distinct from the WS-level PONG WsClient
// sends itself, which does not satisfy Pusher's application ping.
inline constexpr const char *kPusherPongFrame = "{\"event\":\"pusher:pong\",\"data\":{}}";

// A `pusher:subscribe` frame for `channel`. Every Kick channel either transport reads is
// public, so the auth field is always the empty string.
std::string PusherSubscribeFrame(const std::string &channel);

// The Pusher channel carrying a chatroom's messages AND its sub/gift/host events, so the
// chat and events transports cannot spell the same channel two ways.
std::string PusherChatroomChannel(const std::string &chatroomId);

// Resolve a channel slug to BOTH its Pusher chatroom id (carries chat + sub/gift/
// host events on `chatrooms.<id>.v2`) AND its numeric channel id (carries follower
// events on `channel.<id>`), via the UNOFFICIAL GET
// https://kick.com/api/v2/channels/<slug> -- the same lookup kick_chat uses, whose
// response contains both ids. Fills `chatroomIdOut`/`channelIdOut` (either may be
// empty when that id is absent from the response); returns false + `err` only on a
// transport/HTTP failure or an unusable (missing/zero) chatroom id. A caller that
// needs only one id ignores the other. This /api/v2 path is internal and may be
// gated/removed; the error surfaces that clearly.
bool ResolveKickChannelIds(const std::string &slug, std::string &chatroomIdOut, std::string &channelIdOut,
			   std::string &err);

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_KICK_PUSHER_HPP_
