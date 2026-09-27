#include "chat_history.hpp"

#include <string>
#include <utility>

namespace Chat {

bool ChatHistory::Add(const OAuth::DestinationId &dest, const json &message)
{
	const auto id = message.find("id");
	if (id == message.end() || !id->is_string() || id->get_ref<const std::string &>().empty()) {
		return true;
	}
	const std::string key = OAuth::DestinationKey(dest) + ":" + id->get_ref<const std::string &>();
	// Copied, and the evicted message freed, outside the lock: the transports' workers all
	// come through here, and chat.list waits on the same lock.
	json copy = message;
	json evicted;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!keys_.add(key)) {
			return false;
		}
		messages_.push_back(std::move(copy));
		if (messages_.size() > kCap) {
			evicted = std::move(messages_.front());
			messages_.pop_front();
		}
	}
	return true;
}

ChatHistory::json ChatHistory::List() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	json arr = json::array();
	for (const json &m : messages_) {
		arr.push_back(m);
	}
	return arr;
}

ChatHistory &History()
{
	static ChatHistory history;
	return history;
}

} // namespace Chat
