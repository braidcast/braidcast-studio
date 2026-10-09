#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_LIMITS_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_LIMITS_HPP_

#include <cstddef>
#include <string>
#include <vector>

// How long a chat message may be on each platform. The chat pipeline has no length check
// of its own (a typed message reaches the platform as typed); voice dictation checks here
// first, because a message it sends goes to several platforms at once and they do not
// agree. The typed path should reuse this seam when it gains a check.
namespace Chat {

// The limit applied to a platform we have no number for. Deliberately the smallest of the
// known limits: refusing a message the platform would have accepted is recoverable,
// silently sending half of one is not.
inline constexpr size_t kDefaultMessageLimit = 200;

// Unicode code points in a UTF-8 string. Platforms count characters; we hold bytes.
size_t CodePointCount(const std::string &utf8);

// The maximum message length for a platform id, or kDefaultMessageLimit.
size_t MessageLimit(const std::string &platform);

// True when `text` is non-empty and fits every platform in `platforms` (which must not be
// empty). On false, `offender` names the platform with the smallest limit that the message
// exceeded, or is empty when the message or the platform list was empty.
bool FitsEverywhere(const std::string &text, const std::vector<std::string> &platforms, std::string &offender);

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_LIMITS_HPP_
