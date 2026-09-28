#include "chat_history.hpp"

#include <string>
#include <utility>
#include <vector>

#include "../util/random_util.hpp"
#include "../util/time_util.hpp"

namespace Chat {

namespace {

// A string field of a held frame by reference, "" when absent: Page reads three per held
// message under the lock, so none of them is copied.
const std::string &Field(const json &message, const char *key)
{
	static const std::string kEmpty;
	const auto it = message.find(key);
	return it != message.end() && it->is_string() ? it->get_ref<const std::string &>() : kEmpty;
}

} // namespace

bool ChatHistory::Add(const OAuth::DestinationId &dest, json &message)
{
	const auto id = message.find("id");
	const bool keyed = id != message.end() && id->is_string() && !id->get_ref<const std::string &>().empty();
	const std::string key = keyed ? OAuth::DestinationKey(dest) + ":" + id->get_ref<const std::string &>()
				      : std::string();
	// Copied, and the evicted message freed, outside the lock: the transports' workers all
	// come through here, and chat.list waits on the same lock. The stamp keys go in with the
	// copy, so the lock only overwrites two numbers.
	json copy;
	if (keyed) {
		copy = message;
		copy["seq"] = 0;
		copy["rx"] = 0;
	}
	json evicted;
	uint64_t seq = 0;
	int64_t rx = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (keyed && !keys_.add(key)) {
			return false;
		}
		seq = nextSeq_++;
		rx = TimeUtil::NowMs();
		if (keyed) {
			copy["seq"] = seq;
			copy["rx"] = rx;
			messages_.push_back(std::move(copy));
			if (messages_.size() > kCap) {
				evicted = std::move(messages_.front());
				messages_.pop_front();
			}
		}
	}
	message["seq"] = seq;
	message["rx"] = rx;
	return true;
}

ChatPage ChatHistory::Page(std::optional<uint64_t> beforeSeq, size_t limit, const Feed::Filter &filter) const
{
	// One past the limit is collected, so `more` needs no second pass.
	std::vector<json> newestFirst;
	ChatPage page;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		page.epoch = epoch_;
		for (auto it = messages_.rbegin(); it != messages_.rend() && newestFirst.size() <= limit; ++it) {
			if (beforeSeq && it->value("seq", uint64_t(0)) >= *beforeSeq) {
				continue;
			}
			if (!filter.Matches(Field(*it, "platform"), Field(*it, "profileUuid"),
					    Field(*it, "accountId"))) {
				continue;
			}
			newestFirst.push_back(*it);
		}
	}
	page.more = newestFirst.size() > limit;
	if (page.more) {
		newestFirst.pop_back();
	}
	for (auto it = newestFirst.rbegin(); it != newestFirst.rend(); ++it) {
		page.items.push_back(std::move(*it));
	}
	return page;
}

uint64_t ChatHistory::Clear()
{
	std::deque<json> dropped; // freed after the lock is released
	uint64_t epoch = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		dropped.swap(messages_);
		keys_ = SeenIds{kCap};
		epoch = ++epoch_;
	}
	return epoch;
}

ChatHistory &History()
{
	static ChatHistory history;
	return history;
}

const std::string &LaunchId()
{
	// Uniqueness across launches is all it is for, so a failed RNG falls back to the launch
	// time rather than leaving the prefix empty.
	static const std::string id = [] {
		std::string hex = RandomUtil::HexToken(6);
		return hex.empty() ? std::to_string(TimeUtil::NowMs()) : hex;
	}();
	return id;
}

} // namespace Chat
