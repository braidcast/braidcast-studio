#include "recent_chatters.hpp"

#include "../util/string_util.hpp"
#include "../voice/FuzzyMatch.hpp"

#include <set>

namespace Chat {

std::string RecentChatters::Key(const std::string &platform, const std::string &authorId,
				const std::string &displayName)
{
	if (!authorId.empty()) {
		return platform + ":" + authorId;
	}
	return platform + ":n:" + StringUtil::ToLower(displayName);
}

void RecentChatters::Note(const std::string &platform, const std::string &authorId, const std::string &displayName,
			  const OAuth::DestinationId &dest, int64_t nowMs)
{
	// Nobody to reply to: a reply is addressed by name, and without an id there is
	// nothing to key the row on either.
	if (displayName.empty()) {
		return;
	}
	const std::string key = Key(platform, authorId, displayName);
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto it = entries_.begin(); it != entries_.end(); ++it) {
		if (Key(it->platform, it->authorId, it->displayName) == key) {
			Chatter existing = *it;
			existing.lastSeenMs = nowMs;
			existing.displayName = displayName; // a renamed account answers to its new name
			existing.accountId = dest.accountId;
			existing.profileUuid = dest.profileUuid;
			entries_.erase(it);
			entries_.push_front(std::move(existing));
			return;
		}
	}
	entries_.push_front(Chatter{platform, authorId, displayName, nowMs, dest.accountId, dest.profileUuid});
	while (entries_.size() > kCapacity) {
		entries_.pop_back();
	}
}

std::vector<Chatter> RecentChatters::Recent(int64_t nowMs) const
{
	std::vector<Chatter> out;
	std::lock_guard<std::mutex> lock(mutex_);
	for (const Chatter &entry : entries_) {
		if (nowMs - entry.lastSeenMs <= kWindowMs) {
			out.push_back(entry);
		}
	}
	return out;
}

std::vector<Chatter> DistinctByName(const std::vector<Chatter> &recent)
{
	std::vector<Chatter> out;
	std::set<std::string> seen;
	for (const Chatter &entry : recent) {
		if (seen.insert(StringUtil::ToLower(entry.displayName)).second) {
			out.push_back(entry);
		}
	}
	return out;
}

std::optional<Chatter> RecentChatters::Resolve(const std::string &spokenName, int64_t nowMs) const
{
	const std::vector<Chatter> people = DistinctByName(Recent(nowMs));
	std::vector<std::string> names;
	names.reserve(people.size());
	for (const Chatter &entry : people) {
		names.push_back(Voice::SpokenHandle(entry.displayName));
	}
	const Voice::SlotMatch match = Voice::BestMatch(spokenName, names);
	if (!match.ok) {
		return std::nullopt; // no match, or two people too alike to choose between
	}
	return people[match.index];
}

void RecentChatters::Clear()
{
	std::lock_guard<std::mutex> lock(mutex_);
	entries_.clear();
}

RecentChatters &Chatters()
{
	static RecentChatters ring;
	return ring;
}

} // namespace Chat
