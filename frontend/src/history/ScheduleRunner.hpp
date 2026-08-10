#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ScheduleStore.hpp"
#include "util/time_util.hpp"

namespace History {

// How far ahead of its start an entry is armed, and how far ahead the cancellable
// countdown opens.
inline constexpr int64_t kArmLeadMs = 5 * 60 * 1000;
inline constexpr int64_t kCountdownLeadMs = 60 * 1000;

// How long past its start an entry may still be claimed before it settles as
// missed. Going live is asynchronous -- the request returns immediately and the
// outputs report live through the engine's state edge seconds later -- so a zero
// grace would call an entry missed on the same tick it was started. It doubles as
// the window in which someone running late can still start an armed entry by hand.
// The startup sweep applies the same grace, or launching thirty seconds late would
// settle an entry a running app would still have started.
inline constexpr int64_t kMissedGraceMs = 2 * 60 * 1000;

// How long a settled occurrence's cancel flag and block reason are kept after its
// start. Long enough that someone who stepped away still sees why it did not run,
// bounded so a long session does not accumulate every occurrence it ever tracked.
inline constexpr int64_t kOccurrenceRetentionMs = 60 * 60 * 1000;

// The one unit that acts on its own initiative: it watches the clock and moves
// planned entries through armed -> live -> done, or settles them as missed.
//
// Every outside effect is an injected callable, so the whole state machine runs in
// a test against a fake clock with no waiting, no database of the user's, and no
// broadcast. Nothing here knows about the bridge, libobs, or CEF.
//
// Tick() rides the app's existing 1 Hz stats sampler rather than owning a timer;
// two timers for one clock would drift against each other.
//
// UI thread only.
//
// HOW A CANCELLED OCCURRENCE IS REPRESENTED
//
// Cancelling puts the entry back to `planned` and records its id in a transient
// in-memory map that survives only for the life of the process. The row is
// deliberately not moved to the `canceled` state: that would settle the entry
// itself, and what the user cancelled is this occurrence. Recurrence is out of
// scope for v1, so one flag per entry id is enough; a recurring entry would key
// the same map by (id, occurrence start) instead.
//
// The flag is what keeps the occurrence from re-arming and from auto-starting,
// and IsCountdownCanceled() is how schedule.list/get expose it, so the UI can
// tell a cancelled occurrence from one that was never armed -- both read
// `planned`, and the chip renders them differently. It is dropped when the entry
// is deleted or rescheduled beyond the arm lead, which is a new occurrence; it
// deliberately outlives the entry settling as missed, so the chip can still say
// the start was cancelled rather than merely skipped.
class ScheduleRunner {
public:
	ScheduleRunner() = default;
	~ScheduleRunner() = default;
	ScheduleRunner(const ScheduleRunner &) = delete;
	ScheduleRunner &operator=(const ScheduleRunner &) = delete;

	// Wall clock in epoch milliseconds. Replaced by a fake in tests.
	std::function<int64_t()> nowMs = &TimeUtil::NowMs;

	// Whether `profileId` can carry an output right now. False must fill `reason`
	// with something the UI can show. Unset means every destination is armable.
	std::function<bool(const std::string &profileId, std::string &reason)> canArm;

	// Load the entry into the go-live path at arm time: the destinations it names
	// become the enabled routing, and the metadata it carries becomes what the
	// go-live path will send. `revertEntry` puts back what was there when the
	// occurrence ends without a broadcast to show for it, so a cancelled countdown
	// does not leave the user's destination set quietly rewritten. Whoever supplies
	// these owns the memory of the previous state; the runner only says when.
	//
	// One occurrence is applied at a time, and it stays applied for as long as the
	// entry is armed or live. Detach() drops the knowledge without reverting -- the
	// stores it would have to touch are already gone by then -- so an application
	// outlives a shutdown that happens mid-window.
	std::function<void(const std::vector<ScheduleDestination> &)> applyEntry;
	std::function<void()> revertEntry;

	// The go-live tail, wired to the one entry point every other start funnels
	// through. Invoked at most once per occurrence.
	std::function<void()> goLive;

	// Push schedule.changed. Called at most once per tick, and only when the tick
	// changed something schedule.list reports.
	std::function<void()> onChanged;

	// Lifecycle logging, one line per transition. Never per tick.
	std::function<void(const std::string &)> log;

	// The runner holds the store; it does not own it. Pass nullptr to detach.
	void Attach(ScheduleStore *store);
	void Detach();

	// One pass of the state machine. Cheap and side-effect-free when nothing is
	// due, so it is safe on every sampler tick.
	void Tick();

	// Disarm this occurrence: it will neither re-arm nor auto-start, and the entry
	// row stays intact. False fills `error` with the reason.
	bool CancelCountdown(const std::string &id, std::string &error);

	// Re-check one entry after it was edited. schedule.update can move an armed
	// entry out of its own arm window, and the row has to come back to `planned`
	// with it: left armed the chip reads armed until the new start, and a manual
	// go-live in between would stamp this entry's id onto an unrelated session.
	// Emits nothing -- the caller edited the entry and is already pushing.
	void NoteEntryChanged(const std::string &id);

	bool IsCountdownCanceled(const std::string &id) const;

	// Why this occurrence cannot go live, or empty when nothing is wrong. Surfaced
	// alongside the state so a refused auto-start reads as an explanation rather
	// than as an entry that silently did not happen.
	std::string BlockReason(const std::string &id) const;

	// The entry a broadcast starting now belongs to -- the live one, else the
	// armed one, else empty. What stamps schedule_id onto the session row, for a
	// manual go-live during the armed window as much as for an auto-start.
	const std::string &ActiveEntryId() const;

	// The broadcast edges, called from the one place that observes them. Going
	// live is what moves an armed entry to `live`: the request alone does not,
	// since outputs that never come up are not a broadcast.
	void NoteWentLive();
	void NoteStoppedStreaming();

private:
	// Per-occurrence state that is deliberately not persisted: it describes this
	// run of this app, and a restart is a fresh reading of the clock.
	struct Occurrence {
		bool canceled = false;
		bool countdownOpen = false;
		bool startRequested = false;
		std::string blockReason;
	};

	bool PruneStale(int64_t now);
	bool DisarmIfOutOfWindow(const ScheduleEntry &entry, int64_t now);
	bool ArmIfDue(ScheduleEntryWithDestinations &row, int64_t now);
	bool RefreshArmability(const ScheduleEntryWithDestinations &row);
	void OpenCountdownIfDue(const ScheduleEntryWithDestinations &row, int64_t now);
	void StartIfDue(const ScheduleEntryWithDestinations &row, int64_t now);
	bool Armable(const ScheduleEntryWithDestinations &row, std::string &reason) const;
	bool SetBlockReason(const std::string &id, const std::string &reason);
	void SettleApplied();
	void RevertApplied();
	void Log(const std::string &line) const;

	ScheduleStore *store_ = nullptr;
	std::unordered_map<std::string, Occurrence> occurrences_;
	std::string armedId_;
	std::string liveId_;
	// The occurrence whose configuration is currently loaded into the go-live path.
	std::string appliedId_;
};

} // namespace History
