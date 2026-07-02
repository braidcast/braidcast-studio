#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_THIRD_PARTY_EMOTES_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_THIRD_PARTY_EMOTES_HPP_

#include <functional>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

// Third-party emote resolver (7TV + BetterTTV + FrankerFaceZ) for chat overlays
// (Phase 9). Given a channel identity, it fetches each provider's global + channel
// emote sets and returns a `code -> image URL` lookup the chat transport uses to
// substitute plain-text words with emote fragments.
namespace Chat {

using json = nlohmann::json;

// The host platform the third-party providers key their channel sets off. 7TV and
// BTTV serve Twitch, Kick, and YouTube channels behind the same
// /users/<platform>/<id> shape; FFZ is Twitch-only.
enum class EmotePlatform {
	Twitch,
	Kick,
	YouTube,
};

// Fetch the merged third-party emote map for one channel. `login` is the channel's
// platform login (FFZ keys rooms by login -- Twitch only); `userId` is the platform's
// channel id (7TV + BTTV key channel sets by it). Either may be empty -- an empty
// `userId` simply skips the id-keyed channel sets while still fetching every global
// set (and, for Twitch, the FFZ room-by-login set).
//
// BLOCKING: issues several HTTP GETs in sequence, so it MUST run on a worker thread,
// never the UI thread. Every provider/set fetch is best-effort: a failure is logged
// and skipped, yielding fewer emotes rather than an error. `canceled` is polled
// before each GET so a Stop() during teardown returns promptly with whatever partial
// map was built (an empty map is fine -- the caller's post-pass tolerates it).
std::unordered_map<std::string, std::string> FetchThirdPartyEmotes(EmotePlatform platform, const std::string &login,
								   const std::string &userId,
								   const std::function<bool()> &canceled);

// Rescan already-built fragments and substitute third-party (7TV/BTTV/FFZ) emotes.
// Only `text` fragments are touched -- native platform emote fragments win and pass
// through verbatim. A word (a maximal run of non-space chars) is replaced only on an
// EXACT, case-sensitive match, since third-party codes are case-sensitive. Spaces
// are kept in the text runs so the message reads back byte-identical apart from the
// matched words, and the emitted emote fragment shape matches the transports'.
json ApplyThirdPartyEmotes(const json &fragments, const std::unordered_map<std::string, std::string> &emotes);

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_THIRD_PARTY_EMOTES_HPP_
