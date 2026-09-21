#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_POLL_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_POLL_HPP_

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// The pure half of YouTube live polls: the liveChatMessages.insert body that opens one, and
// the reading of a liveChatMessage resource back into the platform-neutral poll shape the
// transport hands up (see ChatTransport::createPoll). No I/O, so the self-test drives it
// offline.
namespace YouTubePoll {

// The liveChatMessages.insert(part=snippet) body for a pollEvent in `liveChatId`. Options go
// in the order given, which is the order YouTube shows them.
nlohmann::json BuildInsertBody(const std::string &liveChatId, const std::string &question,
			       const std::vector<std::string> &options);

// A liveChatMessage resource read into {id, question, options:[{text, tally}], status}.
// `tally` is a number, or null when the resource carries none (an open poll's insert
// response) or one that does not parse; it is accepted as a JSON number or a numeric string.
// `status` is "active" or "closed" as the resource states it, else `fallbackStatus` -- the
// state the caller's own request implies. A missing question or option list comes back
// empty; the caller keeps what it asked for in that case.
nlohmann::json Normalize(const nlohmann::json &message, const char *fallbackStatus);

} // namespace YouTubePoll

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_POLL_HPP_
