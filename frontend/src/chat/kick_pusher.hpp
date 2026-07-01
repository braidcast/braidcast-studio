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

// A browser-like User-Agent for the unofficial kick.com/api/v2 lookup -- the
// research flags that bot-detection TLS/UA fingerprinting may block non-browser
// clients on that internal endpoint.
inline constexpr const char *kKickBrowserUserAgent =
	"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
	"Chrome/126.0.0.0 Safari/537.36";

// The Pusher app WebSocket URL built from the shared app key/host/version.
std::string KickPusherUrl();

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
