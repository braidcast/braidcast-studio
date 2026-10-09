#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_RECENT_CHATTERS_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_RECENT_CHATTERS_HPP_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../oauth/provider.hpp" // OAuth::DestinationId

namespace Chat {

struct Chatter {
	std::string platform;
	std::string authorId; // empty on platforms that do not expose one
	std::string displayName;
	int64_t lastSeenMs = 0;
	// The destination the message arrived on, which is what a reply must be routed back
	// to. Without these two a reply can only be addressed by platform, and on a platform
	// with one broadcast per destination that posts it to EVERY live broadcast -- the
	// fan-out ChatHub::SendToPlatforms' declaration says a reply must avoid. Together they
	// are an OAuth::DestinationId.
	std::string accountId;
	std::string profileUuid;
};

// Who has spoken in chat lately, so a spoken "reply to Dave" has a list to search.
// Owned beside the chat hub, which is the one place every incoming message passes.
//
// Bounded twice over: the last 200 speakers, and only the last 30 minutes. Both
// bounds exist so a name from earlier in a long stream cannot outrank the person
// talking now, and so the fuzzy search stays over a small set.
//
// Thread safe: written from the chat transports' worker threads, read from the UI
// thread.
class RecentChatters {
public:
	static constexpr size_t kCapacity = 200;
	static constexpr int64_t kWindowMs = 30 * 60 * 1000;

	// A message arrived. Repeats move the speaker to the front rather than adding a row,
	// and adopt the newest message's destination: the same person may speak on two of our
	// broadcasts, and a reply belongs in the one they last used.
	void Note(const std::string &platform, const std::string &authorId, const std::string &displayName,
		  const OAuth::DestinationId &dest, int64_t nowMs);

	// Most recent first, within the window.
	std::vector<Chatter> Recent(int64_t nowMs) const;

	// The one speaker whose name matches `spokenName`, or nothing when there is no
	// match or more than one plausible match. Fuzzy, through Voice::BestMatch over each
	// handle as it is said (Voice::SpokenHandle), because the user says a name rather
	// than spelling a handle. One display name used on two platforms is one candidate,
	// the more recent speaker.
	std::optional<Chatter> Resolve(const std::string &spokenName, int64_t nowMs) const;

	void Clear();

private:
	// platform + ":" + author id, or platform + ":n:" + lowercased name where the
	// platform has no stable id.
	static std::string Key(const std::string &platform, const std::string &authorId,
			       const std::string &displayName);

	mutable std::mutex mutex_;
	std::deque<Chatter> entries_; // front = most recent
};

// The process-wide ring, owned beside the chat hub.
RecentChatters &Chatters();

// The speakers in `recent` (most recent first) with one entry per display name, the most
// recent speaker under it, compared case-insensitively. A name heard on two platforms is
// one person to the ear, and BestMatch would call two equal names ambiguous.
std::vector<Chatter> DistinctByName(const std::vector<Chatter> &recent);

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_RECENT_CHATTERS_HPP_
