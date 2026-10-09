#include "chat_limits.hpp"

namespace Chat {

namespace {

struct PlatformLimit {
	const char *platform;
	size_t limit;
};

// Each number should carry its source; re-check them when a platform is added. None of the
// four could be confirmed where this was written (the platforms' documentation sites were
// unreachable from it, and Kick's public docs repository only points at an OpenAPI file on
// api.kick.com), so all four are the design's numbers, marked assumed, unverified. Erring
// short costs a refused message; erring long costs a truncated one, which is why the
// fallback for an unknown platform is the smallest.
constexpr PlatformLimit kLimits[] = {
	{"twitch", 500},    // IRC PRIVMSG body; assumed, unverified (Twitch chat documentation)
	{"youtube", 200},   // liveChatMessages.insert snippet.textMessageDetails.messageText;
			    // assumed, unverified (YouTube Live Streaming API reference)
	{"kick", 500},      // POST /public/v1/chat `content`; assumed, unverified (Kick API)
	{"facebook", 8000}, // Graph API comment `message`; assumed, unverified
};

} // namespace

size_t CodePointCount(const std::string &utf8)
{
	size_t count = 0;
	for (const unsigned char c : utf8) {
		// Continuation bytes (10xxxxxx) belong to the character before them, so a
		// malformed sequence counts at most one per byte and never loops.
		if ((c & 0xc0) != 0x80) {
			++count;
		}
	}
	return count;
}

size_t MessageLimit(const std::string &platform)
{
	for (const PlatformLimit &entry : kLimits) {
		if (platform == entry.platform) {
			return entry.limit;
		}
	}
	return kDefaultMessageLimit;
}

bool FitsEverywhere(const std::string &text, const std::vector<std::string> &platforms, std::string &offender)
{
	offender.clear();
	if (text.empty() || platforms.empty()) {
		return false;
	}
	const size_t length = CodePointCount(text);
	size_t smallest = 0;
	for (const std::string &platform : platforms) {
		const size_t limit = MessageLimit(platform);
		if (length > limit && (offender.empty() || limit < smallest)) {
			offender = platform;
			smallest = limit;
		}
	}
	return offender.empty();
}

} // namespace Chat
