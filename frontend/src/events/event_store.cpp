#include "event_store.hpp"

#include "../log.hpp"
#include "../multistream/StorePaths.hpp"
#include "../util/time_util.hpp"

#include <obs.hpp>
#include <util/platform.h>

#include <algorithm>

namespace Events {

namespace {

// (ts, id) order, the order events.list pages in. std::string compares bytes as unsigned
// char, so a UTF-8 id sorts by code point.
bool SortsBefore(int64_t aTs, const std::string &aId, int64_t bTs, const std::string &bId)
{
	return aTs < bTs || (aTs == bTs && aId < bId);
}

} // namespace

// Every optional field defaults to empty/zero (ToJson omits them), so a follow event
// round-trips without stray keys and an older/newer file with missing fields loads
// cleanly.
NormalizedEvent EventFromJson(const json &j)
{
	NormalizedEvent ev;
	if (!j.is_object()) {
		return ev;
	}
	ev.id = j.value("id", std::string());
	ev.platform = j.value("platform", std::string());
	ev.type = j.value("type", std::string());
	ev.ts = j.value("ts", static_cast<int64_t>(0));
	ev.accountId = j.value("accountId", std::string());
	ev.profileUuid = j.value("profileUuid", std::string());
	ev.actorName = j.value("actorName", std::string());
	ev.actorColor = j.value("actorColor", std::string());
	ev.amount = j.value("amount", static_cast<int64_t>(0));
	ev.currency = j.value("currency", std::string());
	ev.tier = j.value("tier", std::string());
	ev.months = j.value("months", 0);
	ev.count = j.value("count", 0);
	ev.message = j.value("message", std::string());
	ev.msgId = j.value("msgId", std::string());
	ev.authorId = j.value("authorId", std::string());
	ev.deleted = j.value("deleted", std::string());
	return ev;
}

std::string EventStore::FilePath()
{
	return MultistreamBasicPath("events.json");
}

bool EventStore::Add(const NormalizedEvent &ev)
{
	json snapshot;
	bool doWrite = false;
	uint64_t stamp = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (ev.id.empty()) {
			return false; // no id -> undedupable, drop
		}
		if (ids_.count(ev.id)) {
			AdoptIdentityLocked(ev);
			return false; // already stored -> drop
		}
		ids_.insert(ev.id);
		events_.push_back(ev);
		while (events_.size() > kCap) {
			ids_.erase(events_.front().id);
			events_.pop_front();
		}
		dirty_ = true;
		// Debounce: write at most once per kSaveIntervalNs. The gate is evaluated under
		// mutex_, so only one Add per interval commits a write. A trailing dirty state
		// that never reaches the interval is persisted by the next Add or by Flush() on
		// clean shutdown (a hard kill loses at most the last few seconds of feed cache).
		const uint64_t now = os_gettime_ns();
		if (lastSaveNs_ == 0 || now - lastSaveNs_ >= kSaveIntervalNs) {
			snapshot = BuildJsonLocked();
			stamp = writer_.Stamp();
			lastSaveNs_ = now;
			dirty_ = false;
			doWrite = true;
		}
	}
	if (doWrite) {
		Persist(snapshot, stamp);
	}
	return true;
}

void EventStore::AdoptIdentityLocked(const NormalizedEvent &copy)
{
	// The same purchase can reach the store twice under one content-derived id: from the
	// Super Chat REST poll, which knows neither the chat message id nor the broadcast, and
	// from live chat, which knows both. The first copy is kept, so it takes what the
	// second knows that it doesn't, and a removal of that message or on its broadcast then
	// reaches it. Never the text.
	const auto it = std::find_if(events_.begin(), events_.end(),
				     [&copy](const NormalizedEvent &ev) { return ev.id == copy.id; });
	if (it == events_.end() || it->accountId != copy.accountId) {
		return;
	}
	bool changed = false;
	const auto adopt = [&changed](std::string &field, const std::string &known) {
		if (field.empty() && !known.empty()) {
			field = known;
			changed = true;
		}
	};
	adopt(it->msgId, copy.msgId);
	adopt(it->authorId, copy.authorId);
	adopt(it->profileUuid, copy.profileUuid);
	if (changed) {
		dirty_ = true; // persisted by the next write or Flush
	}
}

bool EventStore::Contains(const std::string &id) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return ids_.count(id) != 0;
}

std::vector<NormalizedEvent> EventStore::Select(const std::function<bool(const NormalizedEvent &)> &pick) const
{
	std::vector<NormalizedEvent> out;
	std::lock_guard<std::mutex> lock(mutex_);
	for (const NormalizedEvent &ev : events_) {
		if (pick(ev)) {
			out.push_back(ev);
		}
	}
	return out;
}

std::vector<NormalizedEvent> EventStore::List() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return std::vector<NormalizedEvent>(events_.rbegin(), events_.rend()); // newest-first
}

EventPage EventStore::Page(const std::optional<EventCursor> &before, size_t limit, const Feed::Filter &filter) const
{
	// The copy is bounded by kCap and made under the lock; the sort runs after it.
	std::vector<NormalizedEvent> matching;
	EventPage page;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		page.epoch = seq_;
		matching.reserve(events_.size());
		for (const NormalizedEvent &ev : events_) {
			if (before && !SortsBefore(ev.ts, ev.id, before->ts, before->id)) {
				continue;
			}
			if (filter.Matches(ev.platform, ev.profileUuid, ev.accountId)) {
				matching.push_back(ev);
			}
		}
	}
	// Newest first, one past the limit, so `more` needs no second pass.
	const size_t keep = std::min(matching.size(), limit + 1);
	std::partial_sort(matching.begin(), matching.begin() + static_cast<std::ptrdiff_t>(keep), matching.end(),
			  [](const NormalizedEvent &a, const NormalizedEvent &b) {
				  return SortsBefore(b.ts, b.id, a.ts, a.id);
			  });
	matching.resize(keep);
	page.more = matching.size() > limit;
	if (page.more) {
		matching.pop_back();
	}
	std::reverse(matching.begin(), matching.end());
	page.items = std::move(matching);
	return page;
}

uint64_t EventStore::Clear()
{
	json snapshot;
	uint64_t stamp = 0;
	uint64_t epoch = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		events_.clear();
		ids_.clear();
		snapshot = NewEpochSnapshotLocked(stamp); // empty feed
		epoch = seq_;
	}
	Persist(snapshot, stamp);
	return epoch;
}

template<typename Pred> size_t EventStore::RemoveIf(Pred drop)
{
	json snapshot;
	uint64_t stamp = 0;
	size_t removed = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (const NormalizedEvent &ev : events_) {
			if (drop(ev)) {
				ids_.erase(ev.id);
				++removed;
			}
		}
		if (removed == 0) {
			return 0;
		}
		events_.erase(std::remove_if(events_.begin(), events_.end(), drop), events_.end());
		snapshot = NewEpochSnapshotLocked(stamp);
	}
	Persist(snapshot, stamp);
	return removed;
}

std::vector<NormalizedEvent> EventStore::RedactMessages(const std::function<bool(const NormalizedEvent &)> &reaches,
							const std::string &mark)
{
	std::vector<NormalizedEvent> changed;
	json snapshot;
	uint64_t stamp = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (NormalizedEvent &ev : events_) {
			if (ev.message.empty() || !reaches(ev)) {
				continue;
			}
			ev.message.clear();
			ev.deleted = mark;
			changed.push_back(ev);
		}
		if (changed.empty()) {
			return changed;
		}
		snapshot = NewEpochSnapshotLocked(stamp);
	}
	Persist(snapshot, stamp);
	return changed;
}

size_t EventStore::PurgeAccount(const std::string &accountId)
{
	if (accountId.empty()) {
		return 0;
	}
	return RemoveIf([&](const NormalizedEvent &ev) { return ev.accountId == accountId; });
}

size_t EventStore::PruneOlderThan(const std::string &platform, int64_t cutoffMs)
{
	return RemoveIf([&](const NormalizedEvent &ev) {
		return ev.ts < cutoffMs && Feed::NormalizePlatform(ev.platform) == platform;
	});
}

size_t EventStore::PruneExpired()
{
	const int64_t now = TimeUtil::NowMs();
	size_t removed = 0;
	for (const MaxAge &limit : kMaxAge) {
		removed += PruneOlderThan(limit.platform, now - limit.ms);
	}
	if (removed > 0) {
		HostLog("[events] dropped " + std::to_string(removed) +
			" stored event(s) past their platform's storage limit");
	}
	return removed;
}

void EventStore::Flush()
{
	json snapshot;
	uint64_t stamp = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!dirty_) {
			return;
		}
		snapshot = BuildJsonLocked();
		stamp = writer_.Stamp();
		dirty_ = false;
		lastSaveNs_ = os_gettime_ns();
	}
	Persist(snapshot, stamp);
}

void EventStore::Load()
{
	// Called from the ctor before `this` is visible to any other thread, so no lock.
	DropStoreHistory(path_);
	OBSDataAutoRelease root = obs_data_create_from_json_file_safe(path_.c_str(), "bak");
	const char *js = root ? obs_data_get_json(root) : nullptr;
	if (!js) {
		return;
	}
	json parsed;
	try {
		parsed = json::parse(js);
	} catch (const std::exception &e) {
		// A corrupt file starts the store empty rather than aborting boot, but the lost
		// history must not read as "no events yet".
		HostLog(std::string("[events] events.json unparseable (") + e.what() +
			"); starting with no stored history");
		return;
	}
	if (!parsed.is_object() || !parsed.contains("events") || !parsed["events"].is_array()) {
		return;
	}
	for (const json &item : parsed["events"]) {
		NormalizedEvent ev = EventFromJson(item);
		if (ev.id.empty() || ids_.count(ev.id)) {
			continue;
		}
		ids_.insert(ev.id);
		events_.push_back(ev);
	}
	while (events_.size() > kCap) {
		ids_.erase(events_.front().id);
		events_.pop_front();
	}
}

json EventStore::NewEpochSnapshotLocked(uint64_t &stamp)
{
	++seq_;
	stamp = writer_.Stamp();
	dirty_ = false;
	lastSaveNs_ = os_gettime_ns();
	return BuildJsonLocked();
}

json EventStore::BuildJsonLocked() const
{
	json arr = json::array();
	for (const NormalizedEvent &ev : events_) {
		arr.push_back(ev.ToJson());
	}
	return json{{"events", std::move(arr)}};
}

void EventStore::Persist(const json &root, uint64_t stamp)
{
	if (WriteToDisk(root, stamp)) {
		return;
	}
	// Not on disk: the shutdown Flush, or the next write, tries again. Until then the file
	// can still hold what this write removed.
	std::lock_guard<std::mutex> lock(mutex_);
	dirty_ = true;
}

bool EventStore::WriteToDisk(const json &root, uint64_t stamp)
{
	if (!persist_) {
		return true;
	}
	// The shared ordered writer serializes concurrent writers (Add vs. Flush vs. Clear) on
	// the shared tmp path with mutex_ NOT held, so the deque stays writable during the slow
	// file I/O, and it drops a snapshot older than one already written: a stale in-flight Add
	// that built its snapshot before a Clear can never land after it and resurrect the wiped
	// feed. Drop history on every write, not only the removals: a removal's own write would
	// otherwise rotate the file it replaces, removed content included, into the backup Load
	// falls back to.
	return writer_.Write(root, path_, stamp);
}

} // namespace Events
