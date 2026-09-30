#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_HISTORY_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_CHAT_HISTORY_HPP_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "../oauth/provider.hpp" // OAuth::DestinationId
#include "feed_query.hpp"
#include "chat_archive.hpp"

// The multichat scrollback: the last kCap chat.message frames across every destination,
// held so a dock can page back through what it has not loaded -- reopened, detached into
// its own window, opened mid-broadcast, or scrolled up past its window (chat.list). Past
// the ring, pages continue from the chat archive on disk (chat_archive.hpp) while
// persistence is on; without an archive, or with it off, the ring is all there is.
//
// It is also the hub's dedupe and its admission order. A message is keyed by its
// destination plus its platform id (two transports on one platform share an id space),
// and Add refuses a key it still holds, so a re-delivered message is dropped once, here,
// before it reaches the overlay or any dock. Each admitted message is stamped with `seq`,
// the order the host admitted it in and the order chat reads in (platform clocks disagree
// across platforms), and `rx`, the host's receipt time. The key index holds exactly the
// keys of the messages the ring holds: a key leaves only with its message.
//
// The ring drops its oldest message past kCap only once the archive has committed it, so
// no message is ever in neither place. While the writer catches up the ring grows, up to
// ChatArchive::kRingHardCap, where the archive degrades and the ring falls back to kCap.
//
// Thread-safety: Add runs on whichever worker emits (every chat transport's read loop,
// plus the send workers' local echo); Page runs on the bridge's async lane; Clear and
// SetRetention on TID_UI. All take mutex_, which may then take the archive's queue lock
// and nothing else, and none does I/O while holding it: Page reads the archive after
// releasing it. The archive's writer takes mutex_ too (RaiseSeqAbove), holding none of
// the archive's locks.
namespace Chat {

using json = nlohmann::json;

// One chat.list page: items oldest-first, whether older matching rows are held past them,
// and the clear epoch the page was read under.
struct ChatPage {
	json items = json::array();
	bool more = false;
	uint64_t epoch = 0;
	// The ring ran out and chat.db could not be read yet (persistence has just come on):
	// `items` holds what the ring had, and `more` is set because whether older messages
	// exist is not known. chat.list answers with an error, so the dock asks again.
	bool unreadable = false;
};

class ChatHistory {
public:
	// A ring in memory only: nothing is written or read from disk.
	ChatHistory() = default;
	// A ring backed by `archive`, which must outlive it. The process-wide History() is the
	// only one backed by the process-wide Archive(); a self-test backs its own ring with a
	// private archive.
	explicit ChatHistory(ChatArchive *archive) : archive_(archive) {}

	// Open the archive and fill the ring with what it kept. Call once, before any Add.
	void OpenArchive(const ChatArchive::Options &options);

	// Keep `message` if its key is new, stamping `seq` and `rx` onto both the kept copy and
	// `message` itself; false (nothing kept, nothing stamped) for a key already held. A
	// message without an id cannot be deduped, so it is stamped and passed as new but not
	// kept -- the hub mints an id for every message before this runs, so that is a guard,
	// not a path.
	bool Add(const OAuth::DestinationId &dest, json &message);

	// Up to `limit` messages matching `filter` with seq below `beforeSeq` (every one when
	// absent), newest ones first chosen, returned oldest-first: the ring's, then the
	// archive's.
	ChatPage Page(std::optional<uint64_t> beforeSeq, size_t limit, const Feed::Filter &filter) const;

	// Drop every held message, and every stored one, and open a new epoch, which is
	// returned. A page read before the clear carries the old epoch, so a dock can tell it
	// is stale.
	uint64_t Clear();

	// Apply a moderator's removal to the held messages it covers, and to the stored ones.
	// It covers only messages admitted before it; the returned bound is the seq the next
	// admitted message will get, so it covers exactly those with a lower seq.
	uint64_t Redact(const ModerationOp &op);

	// Change what the archive keeps (Off / This session / 7 days).
	void SetRetention(Retention retention);

	static constexpr size_t kCap = 1000;

private:
	struct Held {
		uint64_t seq;
		std::string dest; // OAuth::DestinationKey
		std::string key;  // dest + ":" + id, the dedupe key
		json frame;
	};

	// Move the messages past kCap that may leave into `out`, oldest first.
	void EvictLocked(std::deque<Held> &out);
	// The archive's SeqRaise: every seq issued from here on is above `storedMax`, the newest
	// in a store the writer opened late. Messages already held keep theirs; the ring still
	// runs in ascending seq, and a page reads the store only below the ring's oldest, so the
	// stored rows at or above a held one's seq are reached once it has left the ring.
	void RaiseSeqAbove(uint64_t storedMax);

	ChatArchive *const archive_ = nullptr;
	mutable std::mutex mutex_;
	std::deque<Held> messages_; // oldest at front, so ascending seq
	std::unordered_set<std::string> keys_;
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
