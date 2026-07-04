#ifndef OBS_MULTISTREAM_FRONTEND_EVENTS_EVENT_STORE_HPP_
#define OBS_MULTISTREAM_FRONTEND_EVENTS_EVENT_STORE_HPP_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "event_model.hpp"

// The persisted, de-duplicated event history (Phase 9.2a). A single global file
// (<config>/braidcast/basic/events.json -- the same dir as streams.json,
// since events are per-account and shared across scene collections) loaded on
// construction so the dock shows history immediately. A bounded ring (cap 500,
// most-recent-wins) plus an id set for O(1) dedupe. Thread-safe: EventHub workers
// call Add() from multiple threads, so every method locks an internal mutex.
namespace Events {

class EventStore {
public:
	EventStore() { Load(); }

	// Append `ev` if its id is new; returns false (a no-op) when the id is empty or
	// already present (dedupe). On a new id: append, evict the oldest past the cap,
	// persist. Callable from any worker thread.
	bool Add(const NormalizedEvent &ev);

	// The whole history newest-first, for events.list.
	std::vector<NormalizedEvent> List() const;

	// Drop all history + persist the empty file, for events.clear.
	void Clear();

	// Persist any coalesced pending write immediately. Called on clean shutdown so a
	// debounced trailing event isn't lost. No-op when nothing is dirty.
	void Flush();

	// <config>/braidcast/basic/events.json.
	static std::string FilePath();

private:
	void Load(); // read events.json into events_/ids_ (called from the ctor)

	// Serialize events_ into the on-disk shape. Caller must hold mutex_.
	json BuildJsonLocked() const;
	// Write a prebuilt snapshot to disk. Does its own file I/O with NO deque lock held
	// (serialized against other writers by writeMutex_), so a write never blocks Add's
	// deque access -- the point of the debounce. `seq` is the epoch the snapshot was
	// captured at; a snapshot older than the last one written is dropped (see below).
	void WriteToDisk(const json &root, uint64_t seq) const;

	static constexpr size_t kCap = 500;
	// Coalesce disk writes to at most one per this interval; bursts of events (a raid,
	// a sub train) then cost a single write instead of one rewrite per event.
	static constexpr uint64_t kSaveIntervalNs = 3'000'000'000ULL; // 3s

	mutable std::mutex mutex_;
	std::deque<NormalizedEvent> events_;  // oldest at front, newest at back
	std::unordered_set<std::string> ids_; // dedupe index over events_
	bool dirty_ = false;                  // unpersisted change pending (guarded by mutex_)
	uint64_t lastSaveNs_ = 0;             // last WriteToDisk time (guarded by mutex_)

	// Monotonic write-epoch counter, bumped by Clear() (a content discontinuity) under
	// mutex_. Add/Clear/Flush capture its value with their snapshot; WriteToDisk drops any
	// snapshot older than the last written epoch, so a stale in-flight Add that built its
	// snapshot before a Clear can't win writeMutex_ afterward and resurrect the wiped feed.
	uint64_t seq_ = 0; // guarded by mutex_

	mutable std::mutex writeMutex_; // serializes WriteToDisk; never held with mutex_
	// Highest epoch written to disk. Guarded by writeMutex_ ONLY (never mutex_), so the
	// "never hold mutex_ and writeMutex_ together" rule is preserved.
	mutable uint64_t lastWrittenSeq_ = 0;
};

} // namespace Events

#endif // OBS_MULTISTREAM_FRONTEND_EVENTS_EVENT_STORE_HPP_
