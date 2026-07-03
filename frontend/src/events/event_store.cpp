#include "event_store.hpp"

#include "../multistream/StorePaths.hpp"

#include <obs.hpp>
#include <util/platform.h>

#include <filesystem>

namespace Events {

namespace {

// Rebuild a NormalizedEvent from its ToJson() shape. Every optional field defaults
// to empty/zero (ToJson omits them), so a follow event round-trips without stray
// keys and an older/newer file with missing fields loads cleanly.
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
	ev.actorName = j.value("actorName", std::string());
	ev.actorColor = j.value("actorColor", std::string());
	ev.amount = j.value("amount", static_cast<int64_t>(0));
	ev.currency = j.value("currency", std::string());
	ev.tier = j.value("tier", std::string());
	ev.months = j.value("months", 0);
	ev.count = j.value("count", 0);
	ev.message = j.value("message", std::string());
	return ev;
}

} // namespace

std::string EventStore::FilePath()
{
	return MultistreamBasicPath("events.json");
}

bool EventStore::Add(const NormalizedEvent &ev)
{
	json snapshot;
	bool doWrite = false;
	uint64_t writeSeq = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (ev.id.empty() || ids_.count(ev.id)) {
			return false; // no id (undedupable) or already stored -> drop
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
			writeSeq = seq_; // stamp the snapshot with the current epoch
			lastSaveNs_ = now;
			dirty_ = false;
			doWrite = true;
		}
	}
	if (doWrite) {
		WriteToDisk(snapshot, writeSeq);
	}
	return true;
}

std::vector<NormalizedEvent> EventStore::List() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return std::vector<NormalizedEvent>(events_.rbegin(), events_.rend()); // newest-first
}

void EventStore::Clear()
{
	json snapshot;
	uint64_t writeSeq = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		events_.clear();
		ids_.clear();
		// Open a new epoch: any in-flight Add snapshot built before this point is now
		// stale, so WriteToDisk will drop it even if it wins writeMutex_ after us.
		writeSeq = ++seq_;
		snapshot = BuildJsonLocked(); // empty feed
		dirty_ = false;
		lastSaveNs_ = os_gettime_ns();
	}
	WriteToDisk(snapshot, writeSeq);
}

void EventStore::Flush()
{
	json snapshot;
	uint64_t writeSeq = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!dirty_) {
			return;
		}
		snapshot = BuildJsonLocked();
		writeSeq = seq_; // stamp with the current epoch
		dirty_ = false;
		lastSaveNs_ = os_gettime_ns();
	}
	WriteToDisk(snapshot, writeSeq);
}

void EventStore::Load()
{
	// Called from the ctor before `this` is visible to any other thread, so no lock.
	OBSDataAutoRelease root = obs_data_create_from_json_file_safe(FilePath().c_str(), "bak");
	const char *js = root ? obs_data_get_json(root) : nullptr;
	if (!js) {
		return;
	}
	json parsed;
	try {
		parsed = json::parse(js);
	} catch (...) {
		return; // a corrupt file starts the store empty rather than aborting boot
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

json EventStore::BuildJsonLocked() const
{
	json arr = json::array();
	for (const NormalizedEvent &ev : events_) {
		arr.push_back(ev.ToJson());
	}
	return json{{"events", std::move(arr)}};
}

void EventStore::WriteToDisk(const json &root, uint64_t seq) const
{
	// Serialize concurrent writers (Add vs. Flush vs. Clear) so two passes can't
	// interleave on the shared tmp path; mutex_ is NOT held here, so the deque stays
	// writable during the (slow) file I/O.
	std::lock_guard<std::mutex> wlock(writeMutex_);
	// Drop a snapshot a later epoch already superseded: a stale in-flight Add that built
	// its snapshot before a Clear must not win writeMutex_ after Clear and resurrect the
	// wiped feed. Equal seq is allowed (same epoch -- e.g. a post-Clear Add persisting
	// genuinely new events, or the initial epoch 0).
	if (seq < lastWrittenSeq_) {
		return;
	}
	lastWrittenSeq_ = seq;
	OBSDataAutoRelease data = obs_data_create_from_json(root.dump().c_str());

	// Ensure the parent dir exists (it does once a profile is created, but be safe).
	std::filesystem::path dir = std::filesystem::u8path(FilePath()).parent_path();
	os_mkdirs(dir.u8string().c_str());

	obs_data_save_json_pretty_safe(data, FilePath().c_str(), "tmp", "bak");
}

} // namespace Events
