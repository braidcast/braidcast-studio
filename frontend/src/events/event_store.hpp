#ifndef OBS_MULTISTREAM_FRONTEND_EVENTS_EVENT_STORE_HPP_
#define OBS_MULTISTREAM_FRONTEND_EVENTS_EVENT_STORE_HPP_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "event_model.hpp"
#include "../chat/feed_query.hpp"
#include "../util/time_util.hpp"

// The persisted, de-duplicated event history (Phase 9.2a). A single global file
// (<config>/braidcast/basic/events.json -- the same dir as streams.json,
// since events are per-account and shared across scene collections) loaded on
// construction so the dock shows history immediately. A bounded ring (cap 500,
// most-recent-wins) plus an id set for O(1) dedupe. Thread-safe: EventHub workers
// call Add() from multiple threads, so every method locks an internal mutex.
namespace Events {

// Rebuild a NormalizedEvent from its ToJson() shape (what events.json holds).
NormalizedEvent EventFromJson(const json &j);

// Where an events.list page ends: the oldest row the dock holds. A value, not a position,
// so a cursor whose rows have since been evicted pages on from wherever it would have sat.
struct EventCursor {
	int64_t ts = 0;
	std::string id;
};

// One events.list page: items oldest-first in (ts, id) order, whether older matching
// events are held past them, and the clear epoch the page was read under.
struct EventPage {
	std::vector<NormalizedEvent> items;
	bool more = false;
	uint64_t epoch = 0;
};

class EventStore {
public:
	EventStore()
	{
		Load();
		PruneExpired();
	}

	// A store that neither reads nor writes events.json, for self-tests that must not
	// touch the user's file.
	struct InMemory {};
	explicit EventStore(InMemory) : persist_(false) {}

	// Append `ev` if its id is new; returns false (a no-op) when the id is empty or
	// already present (dedupe). On a new id: append, evict the oldest past the cap,
	// persist. Callable from any worker thread.
	bool Add(const NormalizedEvent &ev);

	// The whole history newest-first (arrival order).
	std::vector<NormalizedEvent> List() const;

	// Up to `limit` stored events matching `filter` that sort before `before` in (ts, id)
	// order -- every stored event when absent -- the newest of them chosen, returned
	// oldest-first. Ids compare as raw bytes, which for UTF-8 is code-point order.
	EventPage Page(const std::optional<EventCursor> &before, size_t limit, const Feed::Filter &filter) const;

	// Drop all history + persist the empty file, for events.clear. Returns the new epoch.
	uint64_t Clear();

	// Drop every stored event of `accountId` and persist, for an account's removal (a
	// revoked grant's data must go). Returns how many were dropped.
	size_t PurgeAccount(const std::string &accountId);

	// Drop the events of `platform` (as platformKey() normalizes it) older than
	// `cutoffMs` and persist. Returns how many were dropped.
	size_t PruneOlderThan(const std::string &platform, int64_t cutoffMs);

	// Apply every platform's storage limit (kMaxAge) as of now. Runs at load and hourly.
	size_t PruneExpired();

	// How long a platform's events may be kept: YouTube's API policy caps stored API data
	// at 30 days.
	struct MaxAge {
		const char *platform;
		int64_t ms;
	};
	static constexpr MaxAge kMaxAge[] = {{"youtube", 30 * TimeUtil::kDayMs}};

	// Persist any coalesced pending write immediately. Called on clean shutdown so a
	// debounced trailing event isn't lost. No-op when nothing is dirty.
	void Flush();

	// <config>/braidcast/basic/events.json.
	static std::string FilePath();

	static constexpr size_t kCap = 500;

private:
	void Load(); // read events.json into events_/ids_ (called from the ctor)

	// Drop every event `drop` selects and persist the rest. Opens a new write epoch, as
	// Clear does, so an in-flight snapshot that still holds a dropped event cannot be
	// written after this and bring it back.
	template<typename Pred> size_t RemoveIf(Pred drop);

	// Serialize events_ into the on-disk shape. Caller must hold mutex_.
	json BuildJsonLocked() const;
	// Write a prebuilt snapshot to disk. Does its own file I/O with NO deque lock held
	// (serialized against other writers by writeMutex_), so a write never blocks Add's
	// deque access -- the point of the debounce. `seq` is the epoch the snapshot was
	// captured at; a snapshot older than the last one written is dropped (see below).
	void WriteToDisk(const json &root, uint64_t seq) const;

	// Coalesce disk writes to at most one per this interval; bursts of events (a raid,
	// a sub train) then cost a single write instead of one rewrite per event.
	static constexpr uint64_t kSaveIntervalNs = 3'000'000'000ULL; // 3s

	mutable std::mutex mutex_;
	std::deque<NormalizedEvent> events_;  // oldest at front, newest at back
	std::unordered_set<std::string> ids_; // dedupe index over events_
	bool dirty_ = false;                  // unpersisted change pending (guarded by mutex_)
	uint64_t lastSaveNs_ = 0;             // last WriteToDisk time (guarded by mutex_)

	// Monotonic write-epoch counter, bumped by Clear() and by every removal (content
	// discontinuities) under mutex_. Add/Clear/Flush capture its value with their snapshot; WriteToDisk drops any
	// snapshot older than the last written epoch, so a stale in-flight Add that built its
	// snapshot before a Clear can't win writeMutex_ afterward and resurrect the wiped feed.
	// It is also the epoch a Page reports, which is how a dock tells a pre-Clear page apart.
	uint64_t seq_ = 0; // guarded by mutex_

	const bool persist_ = true; // false: never read or write events.json (InMemory)

	mutable std::mutex writeMutex_; // serializes WriteToDisk; never held with mutex_
	// Highest epoch written to disk. Guarded by writeMutex_ ONLY (never mutex_), so the
	// "never hold mutex_ and writeMutex_ together" rule is preserved.
	mutable uint64_t lastWrittenSeq_ = 0;
};

} // namespace Events

#endif // OBS_MULTISTREAM_FRONTEND_EVENTS_EVENT_STORE_HPP_
