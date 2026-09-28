#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_HISTORY_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_HISTORY_HPP_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "../oauth/provider.hpp" // OAuth::DestinationId
#include "feed_query.hpp"
#include "seen_ids.hpp"

// The multichat scrollback: the last kCap chat.message frames across every destination,
// held so a dock can page back through what it has not loaded -- reopened, detached into
// its own window, opened mid-broadcast, or scrolled up past its window (chat.list).
// EventStore's shape without the file: in memory only, gone with the process, and never
// written to disk.
//
// It is also the hub's dedupe and its admission order. A message is keyed by its
// destination plus its platform id (two transports on one platform share an id space),
// and Add refuses a key it still holds, so a re-delivered message is dropped once, here,
// before it reaches the overlay or any dock. Each admitted message is stamped with `seq`,
// the order the host admitted it in and the order chat reads in (platform clocks disagree
// across platforms), and `rx`, the host's receipt time. The key index is a SeenIds with
// the ring's own cap, fed only by admitted messages: the two evict oldest-first in
// lockstep, so the index covers exactly the rows the ring still holds.
//
// Thread-safety: Add runs on whichever worker emits (every chat transport's read loop,
// plus the send workers' local echo); Page runs on the bridge's async lane and Clear on
// TID_UI. All take mutex_, and none calls out while holding it.
namespace Chat {

using json = nlohmann::json;

// One chat.list page: items oldest-first, whether older matching rows are held past them,
// and the clear epoch the page was read under.
struct ChatPage {
	json items = json::array();
	bool more = false;
	uint64_t epoch = 0;
};

class ChatHistory {
public:
	// Keep `message` if its key is new, stamping `seq` and `rx` onto both the kept copy and
	// `message` itself; false (nothing kept, nothing stamped) for a key already held. A
	// message without an id cannot be deduped, so it is stamped and passed as new but not
	// kept -- the hub mints an id for every message before this runs, so that is a guard,
	// not a path.
	bool Add(const OAuth::DestinationId &dest, json &message);

	// Up to `limit` held messages matching `filter` with seq below `beforeSeq` (every held
	// message when absent), newest ones first chosen, returned oldest-first.
	ChatPage Page(std::optional<uint64_t> beforeSeq, size_t limit, const Feed::Filter &filter) const;

	// Drop every held message and open a new epoch, which is returned. A page read before
	// the clear carries the old epoch, so a dock can tell it is stale.
	uint64_t Clear();

	static constexpr size_t kCap = 1000;

private:
	mutable std::mutex mutex_;
	std::deque<json> messages_; // oldest at front, so ascending seq
	SeenIds keys_{kCap};
	uint64_t nextSeq_ = 1; // 0 is never issued
	uint64_t epoch_ = 0;
};

// The process-wide scrollback (function-local static, like Hub()).
ChatHistory &History();

// A random id minted once per process. Folded into every id the hub synthesizes for a
// message that arrived without one, so two launches can never mint the same id.
const std::string &LaunchId();

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_HISTORY_HPP_
