#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_HISTORY_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_HISTORY_HPP_

#include <cstddef>
#include <deque>
#include <mutex>

#include <nlohmann/json.hpp>

#include "../oauth/provider.hpp" // OAuth::DestinationId
#include "seen_ids.hpp"

// The multichat scrollback: the last kCap chat.message frames across every destination,
// held so a dock that mounts late -- reopened, detached into its own window, or opened
// mid-broadcast -- can hydrate what it missed (chat.list). EventStore's shape without the
// file: in memory only, gone with the process, and never written to disk.
//
// It is also the hub's dedupe. A message is keyed by its destination plus its platform id
// (two transports on one platform share an id space), and Add refuses a key it still
// holds, so a re-delivered message is dropped once, here, before it reaches the overlay or
// any dock. The key index is a SeenIds with the ring's own cap, fed only by admitted
// messages: the two evict oldest-first in lockstep, so the index covers exactly the rows
// the ring still holds.
//
// Thread-safety: Add runs on whichever worker emits (every chat transport's read loop,
// plus the send workers' local echo); List runs on the bridge's async lane. Both take
// mutex_, and neither calls out while holding it.
namespace Chat {

class ChatHistory {
public:
	using json = nlohmann::json;

	// Keep `message` if its key is new; false (and nothing kept) for a key already held.
	// A message without an id cannot be deduped, so it is passed as new and not kept --
	// the hub mints an id for every message before this runs, so that is a guard, not a
	// path.
	bool Add(const OAuth::DestinationId &dest, const json &message);

	// Every held message, oldest first.
	json List() const;

	static constexpr size_t kCap = 1000;

private:
	mutable std::mutex mutex_;
	std::deque<json> messages_; // oldest at front
	SeenIds keys_{kCap};
};

// The process-wide scrollback (function-local static, like Hub()).
ChatHistory &History();

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_HISTORY_HPP_
