#include "chat_history.hpp"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "../log.hpp"
#include "../util/json_util.hpp"
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

// Repairs are logged on the first and then every this-many-th, so a source that sends
// nothing but bad bytes cannot flood the log.
constexpr uint64_t kRepairLogEvery = 100;

// Text that is not valid UTF-8 (IRC hands over raw bytes) makes dump() throw, and a held
// message is dumped with every page it falls in -- so each of those pages would fail. The
// bad sequences are replaced with U+FFFD before the message is held or forwarded.
void RepairUtf8(json &message)
{
	static std::atomic<uint64_t> repaired{0};
	try {
		(void)message.dump();
	} catch (const json::type_error &) {
		message = json::parse(JsonUtil::DumpLossy(message));
		const uint64_t n = repaired.fetch_add(1, std::memory_order_relaxed) + 1;
		if (n == 1 || n % kRepairLogEvery == 0) {
			HostLog("[chat] repaired invalid UTF-8 in a " + Field(message, "platform") + " message (" +
				std::to_string(n) + " this launch)");
		}
	}
}

// The dedupe key of message `id` from destination `destKey` (OAuth::DestinationKey).
std::string HeldKey(const std::string &destKey, const std::string &id)
{
	return destKey + ":" + id;
}

} // namespace

void ChatHistory::OpenArchive(const ChatArchive::Options &options)
{
	if (!archive_) {
		return;
	}
	ChatArchive::Seed seed;
	archive_->Open(options, seed);
	std::lock_guard<std::mutex> lock(mutex_);
	for (ChatArchive::Seed::Entry &entry : seed.rows) {
		std::string key = HeldKey(entry.dest, Field(entry.frame, "id"));
		if (!keys_.insert(key).second) {
			continue;
		}
		const uint64_t seq = entry.frame.value("seq", uint64_t(0));
		messages_.push_back({seq, std::move(entry.dest), std::move(key), std::move(entry.frame)});
	}
	nextSeq_ = std::max(nextSeq_, seed.nextSeq);
}

bool ChatHistory::Add(const OAuth::DestinationId &dest, json &message)
{
	RepairUtf8(message);
	const auto id = message.find("id");
	const bool keyed = id != message.end() && id->is_string() && !id->get_ref<const std::string &>().empty();
	std::string destKey = keyed ? OAuth::DestinationKey(dest) : std::string();
	std::string key = keyed ? HeldKey(destKey, id->get_ref<const std::string &>()) : std::string();
	// Copied, serialized, and the evicted messages freed, outside the lock: the transports'
	// workers all come through here, and chat.list waits on the same lock. The stamp keys
	// go in with the copy, so the lock only overwrites two numbers.
	json copy;
	std::optional<ChatArchive::Row> row;
	if (keyed) {
		copy = message;
		copy["seq"] = 0;
		copy["rx"] = 0;
		if (archive_ && archive_->Active()) {
			row = ChatArchive::MakeRow(dest, message);
		}
	}
	std::deque<Held> evicted;
	ChatArchive::OpQueue dropped;
	bool degraded = false;
	uint64_t seq = 0;
	int64_t rx = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (keyed && !keys_.insert(key).second) {
			return false;
		}
		seq = nextSeq_++;
		rx = TimeUtil::NowMs();
		if (keyed) {
			copy["seq"] = seq;
			copy["rx"] = rx;
			// Queued under the lock, so the queue runs in seq order. Persistence may have
			// come on since the check above; the row is then built here.
			if (archive_ && archive_->Active()) {
				if (!row) {
					row = ChatArchive::MakeRow(dest, message);
				}
				if (row) {
					row->seq = seq;
					row->rx = rx;
					archive_->Enqueue(std::move(*row));
				}
			}
			messages_.push_back({seq, std::move(destKey), std::move(key), std::move(copy)});
			if (archive_ && messages_.size() > ChatArchive::kRingHardCap && archive_->Active()) {
				dropped =
					archive_->Degrade("the writer fell " + std::to_string(messages_.size() - kCap) +
							  " messages behind");
				degraded = true;
			}
			EvictLocked(evicted);
		}
	}
	if (degraded) {
		ChatArchive::LogDegraded("the writer fell behind by " + std::to_string(dropped.size()) +
					 " queued message(s)");
	}
	message["seq"] = seq;
	message["rx"] = rx;
	return true;
}

void ChatHistory::EvictLocked(std::deque<Held> &out)
{
	// While the archive is writing, a message leaves the ring only once it is committed.
	const bool waitForDisk = archive_ && archive_->Active();
	const uint64_t persisted = waitForDisk ? archive_->PersistedSeq() : 0;
	while (messages_.size() > kCap && (!waitForDisk || messages_.front().seq <= persisted)) {
		keys_.erase(messages_.front().key);
		out.push_back(std::move(messages_.front()));
		messages_.pop_front();
	}
}

ChatPage ChatHistory::Page(std::optional<uint64_t> beforeSeq, size_t limit, const Feed::Filter &filter) const
{
	// One past the limit is collected, so `more` needs no second pass.
	std::vector<json> newestFirst;
	ChatPage page;
	uint64_t ringOldest = 0;
	bool readArchive = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		page.epoch = epoch_;
		ringOldest = messages_.empty() ? nextSeq_ : messages_.front().seq;
		readArchive = archive_ && archive_->Active();
		for (auto it = messages_.rbegin(); it != messages_.rend() && newestFirst.size() <= limit; ++it) {
			if (beforeSeq && it->seq >= *beforeSeq) {
				continue;
			}
			if (!filter.Matches(Field(it->frame, "platform"), Field(it->frame, "profileUuid"),
					    Field(it->frame, "accountId"))) {
				continue;
			}
			newestFirst.push_back(it->frame);
		}
	}
	// The ring ran out: the rest comes from disk, starting below everything the ring holds.
	// Every message older than the ring's oldest was committed before it left the ring.
	if (readArchive && newestFirst.size() <= limit) {
		const uint64_t below = beforeSeq ? std::min(*beforeSeq, ringOldest) : ringOldest;
		std::optional<std::vector<json>> older =
			archive_->ReadOlder(below, limit + 1 - newestFirst.size(), filter);
		if (!older) {
			page.unreadable = true;
		} else {
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if (epoch_ != page.epoch) {
					// Cleared during the read: whatever came back predates the Clear.
					return ChatPage{json::array(), false, epoch_};
				}
			}
			std::move(older->begin(), older->end(), std::back_inserter(newestFirst));
		}
	}
	page.more = page.unreadable || newestFirst.size() > limit;
	if (newestFirst.size() > limit) {
		newestFirst.pop_back();
	}
	for (auto it = newestFirst.rbegin(); it != newestFirst.rend(); ++it) {
		page.items.push_back(std::move(*it));
	}
	return page;
}

uint64_t ChatHistory::Clear()
{
	std::deque<Held> dropped; // freed after the lock is released
	std::unordered_set<std::string> droppedKeys;
	uint64_t epoch = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		dropped.swap(messages_);
		droppedKeys.swap(keys_);
		epoch = ++epoch_;
		if (archive_) {
			archive_->EnqueueClear(nextSeq_);
		}
	}
	return epoch;
}

uint64_t ChatHistory::Redact(const ModerationOp &op)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const Redaction redaction = Redaction::From(op, nextSeq_);
	for (Held &held : messages_) {
		if (redaction.Matches(held.dest, held.seq, held.frame)) {
			RedactFrame(held.frame, op.action);
		}
	}
	if (archive_) {
		archive_->EnqueueRedact(redaction);
	}
	return redaction.belowSeq;
}

void ChatHistory::SetRetention(Retention retention)
{
	if (!archive_) {
		return;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	archive_->SetRetention(retention, nextSeq_ - 1);
}

ChatHistory &History()
{
	static ChatHistory history{&Archive()};
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
