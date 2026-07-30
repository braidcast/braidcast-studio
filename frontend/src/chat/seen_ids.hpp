#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_SEEN_IDS_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_SEEN_IDS_HPP_

#include <cstddef>
#include <deque>
#include <string>
#include <unordered_set>

// Bounded FIFO id memory, shared by every chat read whose surface can hand back a message
// it already delivered: InnerTube re-sends recent items after a filter switch or a reload
// continuation, and Facebook's live-comment stream carries no resume, so each reconnect
// gap-fills over a window the stream may also have covered. Dedupe is by platform message
// id and eviction is oldest-first, so the set cannot grow with uptime on a day-long
// broadcast.
namespace Chat {

class SeenIds {
public:
	// True when `id` is new (and is now remembered); false when it was already seen.
	bool add(const std::string &id)
	{
		if (id.empty()) {
			return true; // nothing to key on -- pass it through rather than dropping it
		}
		if (!ids_.insert(id).second) {
			return false;
		}
		order_.push_back(id);
		if (order_.size() > kCap) {
			ids_.erase(order_.front());
			order_.pop_front();
		}
		return true;
	}

private:
	// Covers every re-delivery window either surface has been measured to produce while
	// costing a few tens of kilobytes at most.
	static constexpr size_t kCap = 512;

	std::unordered_set<std::string> ids_;
	std::deque<std::string> order_;
};

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_SEEN_IDS_HPP_
