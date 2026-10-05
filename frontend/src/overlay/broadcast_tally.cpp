#include "broadcast_tally.hpp"

#include <utility>
#include <vector>

#include "../events/event_store.hpp"
#include "../util/time_util.hpp"

namespace Overlay {

using json = nlohmann::json;

namespace {

constexpr int kRecordVersion = 1;

int64_t IntField(const json &j, const char *key)
{
	const auto it = j.find(key);
	return it != j.end() && it->is_number_integer() ? it->get<int64_t>() : 0;
}

std::string StrField(const json &j, const char *key)
{
	const auto it = j.find(key);
	return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// Whether a finished broadcast's figures from `platform` are past that platform's storage
// limit: they go, and the rest of the broadcast's count stands.
bool PastStorageLimit(const std::string &platform, int64_t untilMs, int64_t nowMs)
{
	const int64_t maxAge = Events::EventStore::MaxAgeMs(platform);
	return untilMs > 0 && maxAge > 0 && nowMs - untilMs > maxAge;
}

} // namespace

std::string TallyKind(const Events::NormalizedEvent &ev)
{
	if (ev.type == "member") {
		return ev.months > 0 ? "milestone" : "new";
	}
	return std::string();
}

std::string BroadcastTally::FilePath()
{
	return MultistreamBasicPath("overlay_tally.json");
}

void BroadcastTally::Open(const std::string &path)
{
	const json record = LoadStoreJson(path);
	std::lock_guard<std::mutex> lock(mutex_);
	if (!path_.empty()) {
		return;
	}
	path_ = path;
	const int64_t since = IntField(record, "since");
	if (since <= 0 || IntField(record, "version") != kRecordVersion) {
		return;
	}
	since_ = since;
	// A window still open on disk is a broadcast the app did not see end: it closed no later
	// than the last save, and nothing after that was recorded.
	const int64_t until = IntField(record, "until");
	const int64_t savedAt = IntField(record, "savedAt");
	until_ = until > 0 ? until : (savedAt > since ? savedAt : since);
	const int64_t now = TimeUtil::NowMs();
	const auto rows = record.find("totals");
	if (rows == record.end() || !rows->is_array()) {
		return;
	}
	for (const json &row : *rows) {
		if (!row.is_object()) {
			continue;
		}
		const std::string platform = StrField(row, "platform");
		const std::string type = StrField(row, "type");
		if (platform.empty() || type.empty() || PastStorageLimit(platform, until_, now)) {
			continue;
		}
		Sum &sum = totals_[Key{platform, type, StrField(row, "kind")}];
		sum.events += IntField(row, "events");
		sum.units += IntField(row, "units");
		sum.amount += IntField(row, "amount");
	}
}

void BroadcastTally::CountLocked(const Recent &r)
{
	Sum &sum = totals_[r.key];
	sum.events += 1;
	sum.units += r.units;
	sum.amount += r.amount;
	countedIds_.push_back(r.id);
	while (countedIds_.size() > kRecentIds) {
		countedIds_.pop_front();
	}
	dirty_ = true;
}

void BroadcastTally::Add(const Events::NormalizedEvent &ev)
{
	if (ev.id.empty() || ev.platform.empty() || ev.type.empty()) {
		return;
	}
	Recent r;
	r.id = ev.id;
	r.key = Key{ev.platform, ev.type, TallyKind(ev)};
	r.ts = ev.ts;
	r.units = ev.count > 0 ? ev.count : 1;
	r.amount = ev.amount;
	r.seenMs = TimeUtil::NowMs();

	json record;
	std::string path;
	uint64_t stamp = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (OpenLocked() && r.ts >= since_) {
			CountLocked(r);
		}
		const int64_t now = TimeUtil::NowMs();
		recent_.push_back(std::move(r));
		// By the app's clock, not the events' own: platforms' clocks disagree, and an event
		// stamped in the future must not outstay the window.
		while (!recent_.empty() &&
		       (recent_.size() > kRecentEvents || recent_.front().seenMs < now - kRecentWindowMs)) {
			recent_.pop_front();
		}
		if (!SaveDueLocked(now)) {
			return;
		}
		record = RecordLocked(now, path, stamp);
	}
	Persist(record, path, stamp);
}

std::optional<json> BroadcastTally::OnStreamState(bool active, int64_t startedAtMs, int64_t nowMs)
{
	json snapshot;
	json record;
	std::string path;
	uint64_t stamp = 0;
	bool persist = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (active && !OpenLocked() && startedAtMs > 0) {
			since_ = startedAtMs;
			until_ = 0;
			totals_.clear();
			countedIds_.clear();
			// What was broadcast between the start and this frame -- an output reports its
			// start a moment after it begins sending -- belongs to this broadcast.
			for (const Recent &r : recent_) {
				if (r.ts >= since_) {
					CountLocked(r);
				}
			}
		} else if (!active && OpenLocked()) {
			until_ = nowMs;
		} else {
			return std::nullopt;
		}
		snapshot = SnapshotLocked();
		persist = !path_.empty();
		if (persist) {
			record = RecordLocked(nowMs, path, stamp);
		}
	}
	if (persist) {
		Persist(record, path, stamp);
	}
	return snapshot;
}

json BroadcastTally::Snapshot() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return SnapshotLocked();
}

int64_t BroadcastTally::OpenSince() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return OpenLocked() ? since_ : 0;
}

void BroadcastTally::Flush()
{
	json record;
	std::string path;
	uint64_t stamp = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!dirty_ || path_.empty()) {
			return;
		}
		record = RecordLocked(TimeUtil::NowMs(), path, stamp);
	}
	Persist(record, path, stamp);
}

void BroadcastTally::SaveIfDue(int64_t nowMs)
{
	json record;
	std::string path;
	uint64_t stamp = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!SaveDueLocked(nowMs)) {
			return;
		}
		record = RecordLocked(nowMs, path, stamp);
	}
	Persist(record, path, stamp);
}

bool BroadcastTally::SaveDueLocked(int64_t nowMs) const
{
	return dirty_ && !path_.empty() && nowMs - lastSaveMs_ >= kSaveDebounceMs;
}

void BroadcastTally::Persist(const json &record, const std::string &path, uint64_t stamp)
{
	if (writer_.Write(record, path, stamp)) {
		return;
	}
	// Still owed a save. The next due save retries it -- kSaveDebounceMs after this attempt,
	// which RecordLocked dated -- so a disk that keeps failing is tried at that pace, not in a
	// loop.
	std::lock_guard<std::mutex> lock(mutex_);
	dirty_ = true;
}

json BroadcastTally::SnapshotLocked() const
{
	const int64_t now = TimeUtil::NowMs();
	json totals = json::array();
	for (const auto &[key, sum] : totals_) {
		if (PastStorageLimit(std::get<0>(key), until_, now)) {
			continue;
		}
		totals.push_back(json{{"platform", std::get<0>(key)},
				      {"type", std::get<1>(key)},
				      {"kind", std::get<2>(key)},
				      {"events", sum.events},
				      {"units", sum.units},
				      {"amount", sum.amount}});
	}
	json recentIds = json::array();
	for (const std::string &id : countedIds_) {
		recentIds.push_back(id);
	}
	return json{
		{"since", since_ > 0 ? json(since_) : json(nullptr)},
		{"until", since_ > 0 && until_ > 0 ? json(until_) : json(nullptr)},
		{"totals", std::move(totals)},
		{"recentIds", std::move(recentIds)},
	};
}

json BroadcastTally::RecordLocked(int64_t nowMs, std::string &path, uint64_t &stamp)
{
	json record = SnapshotLocked();
	// Ids can name the actor (Kick's carry the username), and a restart has no live page to
	// dedupe against, so they stay in memory.
	record.erase("recentIds");
	record["version"] = kRecordVersion;
	record["savedAt"] = nowMs;
	dirty_ = false;
	lastSaveMs_ = nowMs;
	path = path_;
	stamp = writer_.Stamp();
	return record;
}

} // namespace Overlay
