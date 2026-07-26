#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_INNERTUBE_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_INNERTUBE_HPP_

#include <functional>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "../events/event_model.hpp"

// YouTube's InnerTube live-chat read (youtubei/v1/live_chat/get_live_chat) -- the endpoint
// youtube.com's own web client polls. PROTOCOL ONLY: given a videoId it resolves a
// continuation, runs the poll loop, decodes the renderer payloads into the normalized
// chat/event shapes and hands each one to a callback. Deliberately NOT a ChatTransport:
// YouTubeChat owns the transport lifecycle (state, refcounts, terminal reporting, the
// fallback ladder) and drives this as its primary read.
//
// WHY it exists: liveChatMessages.list/.streamList bill against ONE Cloud project's
// 10,000-unit daily budget shared by every install, and a single continuously-streaming
// user with four destinations spends roughly 166,000 units a day. This endpoint costs ZERO
// quota, so it becomes the primary read and the official surfaces stay behind it as the
// fallback -- they are authoritative on terminal reasons and are the only way to read
// member-only chat.
//
// COMPLIANCE: every request here is ANONYMOUS -- no OAuth token, no Authorization header,
// no API key, no ?key=, no cookies. The requests themselves are built and sent by
// util/innertube_client, which is where that rule -- and the process-wide InnerTube rate
// ceiling every reader shares -- is enforced for every consumer at once.
namespace Chat {

using json = nlohmann::json;

namespace YouTubeInnerTube {

// What the read loop hands back. Each is called on the loop's own worker thread, in arrival
// order, and every one MUST be populated.
struct Callbacks {
	// One normalized chat.message frame, ready to emit (third-party emotes already applied).
	std::function<void(const json &message)> emitMessage;
	// A monetization/membership event, IN ADDITION to that item's chat line -- never
	// instead of it. Non-const so the transport can stamp the destination identity on.
	std::function<void(Events::NormalizedEvent &ev)> emitEvent;
	// The first response proving the chat is being read. The transport's connected state
	// AND its per-destination live-chat refcount hold hang off this, so it must be called
	// before any message is emitted. Idempotent on the transport's side.
	std::function<void()> announce;
	// The chat ended for good; the loop has already stopped and will not retry.
	std::function<void(const std::string &reason)> terminal;
	// A transient transport state worth showing while the loop keeps retrying.
	std::function<void(bool connected, const std::string &error)> state;
	std::function<bool()> canceled;
};

struct Config {
	std::string videoId;   // the live broadcast's video id (YouTubeProvider's broadcastId)
	std::string channelId; // stamped into every frame's "channelId" (the liveChatId)
	// 7TV/BTTV/FFZ code -> URL map, applied to the text runs exactly as the official read
	// applies it, so third-party emote resolution composes identically on both paths.
	// Borrowed for the call's duration; null is treated as an empty map.
	const std::unordered_map<std::string, std::string> *thirdPartyEmotes = nullptr;
	std::string destTag; // OAuth::DestinationKey(dest), for the gated log lines
};

// Run the whole read on the CALLING thread until canceled or the chat ends. Returns true to
// request the caller's fallback read -- the continuation never resolved, or the endpoint
// stopped being usable mid-session; false when the session ended for good (terminal, already
// reported through Callbacks::terminal) or was canceled.
bool Run(const Config &cfg, const Callbacks &cb);

} // namespace YouTubeInnerTube

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_INNERTUBE_HPP_
