#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Schema.hpp"

namespace History {

// Entry states, spelled once. The migration's CHECK constraint carries the same
// vocabulary; these are what the C++ side compares against so the strings are
// not retyped at each call site.
namespace ScheduleState {
inline constexpr const char *kPlanned = "planned";
inline constexpr const char *kArmed = "armed";
inline constexpr const char *kLive = "live";
inline constexpr const char *kDone = "done";
inline constexpr const char *kMissed = "missed";
inline constexpr const char *kCanceled = "canceled";
} // namespace ScheduleState

// An entry and the destinations it will go out to, which is the only shape the
// UI ever wants. Splitting them would make every read two calls that must not
// disagree.
struct ScheduleEntryWithDestinations {
	ScheduleEntry entry;
	std::vector<ScheduleDestination> destinations;
};

// The read and write path over planned broadcasts. Sits beside SessionStore over
// the same database rather than opening its own: history and scheduling are one
// archive, and a second connection would need its own busy handling for no gain.
//
// UI thread only.
class ScheduleStore {
public:
	ScheduleStore() = default;
	~ScheduleStore() = default;
	ScheduleStore(const ScheduleStore &) = delete;
	ScheduleStore &operator=(const ScheduleStore &) = delete;

	// Open the ORM connection over an already-migrated database. False leaves
	// the store unattached and every accessor empty -- scheduling degrades to
	// unavailable, it never takes the app down.
	bool Attach(const std::string &path);
	void Detach();
	bool IsAttached() const { return storage_ != nullptr; }
	const std::string &LastError() const { return lastError_; }

	// Half-open [fromMs, toMs). Every calendar view is a range over starts_at,
	// so this is the only read the views need.
	std::vector<ScheduleEntryWithDestinations> ListRange(int64_t fromMs, int64_t toMs);
	std::unique_ptr<ScheduleEntry> Get(const std::string &id);
	std::vector<ScheduleDestination> DestinationsFor(const std::string &id);

	// `entry.id` is filled in when empty. Destinations are replaced wholesale
	// rather than diffed: the set is small, and a diff is a second place for
	// the two sides to disagree.
	bool Create(ScheduleEntry &entry, const std::vector<ScheduleDestination> &destinations);
	bool Update(const ScheduleEntry &entry, const std::vector<ScheduleDestination> &destinations);
	bool SetState(const std::string &id, const std::string &state);

	// Cascades to destinations. A session that this entry planned keeps its own
	// row and loses only the link -- deleting a plan must not delete the record
	// of the broadcast that actually happened.
	bool Remove(const std::string &id);

	// Entries whose start has passed while still planned or armed. Returns how
	// many were marked, so a caller can decide whether to tell the UI.
	//
	// `except` is for entries whose start is still on its way up: the row has to
	// stay `armed` for the broadcast to reach `live` and for the session to be
	// stamped with it, and the grace this sweep runs on is far shorter than a
	// prelude can legitimately take. Settling one of those would record a running
	// broadcast as missed and orphan the session it produced.
	int SweepMissed(int64_t nowMs, const std::vector<std::string> &except = {});

	// Startup recovery, the schedule counterpart of SessionStore::RecoverCrashed.
	// `armed` and `live` describe what a running process was doing, and no process
	// is doing it now. Left alone, a row still `armed` from a previous run would
	// auto-start the moment the app launches -- no countdown, and nobody
	// necessarily there -- and a row left `live` would read live forever, since
	// only the broadcast's own stop edge settles one and that edge is gone.
	//
	// Armed goes back to `planned`, so an entry whose start is still ahead arms
	// again cleanly and one whose start has passed is settled by the sweep like any
	// other. Live becomes `done`, because it did happen -- the session row is where
	// the crash itself is recorded. Returns how many were recovered.
	int RecoverInterrupted();

private:
	std::unique_ptr<Storage> storage_;
	std::string lastError_;
};

} // namespace History
