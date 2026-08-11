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

// How long a requested start is treated as still on its way up, measured from the
// request rather than from the entry's start.
//
// It is deliberately far longer than kMissedGraceMs, which settles the ROW so the
// calendar stops calling an entry upcoming. The routing has to outlive that: a
// go-live prelude is several round trips to a platform (create the broadcast,
// ensure the stream, bind, transition) before RTMP even connects, and a degraded
// link retries on top of that. Putting the routing back underneath a start still
// working through all of it sends that broadcast wherever the user was pointing
// before -- the same outcome cancelling and deleting already refuse to cause.
//
// Nothing here observes the outputs; the engine's live edge is the only thing that
// does, and it arrives as NoteWentLive. So this is a deadline for giving up on a
// start that never reported anything, not a measurement of one.
inline constexpr int64_t kStartInFlightMs = 10 * 60 * 1000;

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

	// Whether a broadcast is already running. An entry cannot take over the routing
	// from one, so this is a refusal, surfaced through the same block reason the
	// chip already shows -- and surfaced for the whole armed window, which is time
	// enough to stop the other broadcast before the start is due.
	std::function<bool()> isStreaming;

	// Load the entry into the go-live path: the destinations it names become the
	// enabled routing, and the metadata it carries becomes what the go-live path
	// will send. False + `reason` refuses the start.
	//
	// Called at T-0 immediately before `goLive`, NOT when the entry arms. Arming is
	// a preparation and stays read-only; this rewrites configuration the user owns,
	// so the window in which that is true is one go-live rather than five minutes.
	// `revertEntry` puts it back once the broadcast ends or the occurrence settles
	// without one. Whoever supplies these owns the memory of the previous state;
	// the runner only says when.
	//
	// Detach() drops that bookkeeping without reverting: MultistreamEngine is
	// already destroyed by the time teardown reaches the runner, and putting the
	// routing back means flipping bindings through it. So a shutdown during a
	// scheduled broadcast leaves its routing in place -- which is the routing that
	// broadcast was using.
	std::function<bool(const std::vector<ScheduleDestination> &, std::string &reason)> applyEntry;
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
	// row stays intact. False fills `error` with the reason -- including once the
	// start has already been requested, which this never takes back: a countdown
	// cancel does not mean stop a broadcast.
	bool CancelCountdown(const std::string &id, std::string &error);

	// Skip the clock and go live on this entry right away, for someone who does not
	// want to wait for its start time. False fills `error` with a UI-showable reason
	// for anything that is not a fresh, unstarted occurrence: already live, done, or
	// cancelled; already going live; not armable; or the routing held by something
	// else. `kPlanned`, `kArmed` and `kMissed` are all accepted -- each one means
	// "has not run yet" -- and the request runs through the same RequestStart the
	// clock itself uses, so a manual start is refused, applied, and logged on the
	// same terms an automatic one is.
	//
	// A row that is not already armed is armed here first: RequestStart only loads
	// an entry's routing, it never changes state, and NoteWentLive requires the row
	// to read `armed` before it can flip it to `live`. The occurrence's cancelled /
	// startRequested / startFailed / blockReason state is cleared too, since this is
	// a new intent overriding whatever the clock had decided about this occurrence --
	// IsStartInFlight is what stops an actual double-start, not those flags.
	bool StartNow(const std::string &id, std::string &error);

	// Called at the top of a manual go-live, before anything reads output bindings,
	// to make good on what IsStartInFlight and ActiveEntryId already promise: a
	// manual go-live during the armed window is that entry's broadcast. Left to a
	// bare "which entry looks imminent" guess, the row would flip to `live` and the
	// session would carry its id without the entry's own destinations or metadata
	// ever having been loaded -- the plan claiming to have run when it did not.
	//
	// A no-op once something has already claimed this go-live (the auto-start or
	// StartNow both set startingId_ before calling goLive) or once one is already
	// live, and a no-op for an occurrence that is cancelled or has already asked to
	// start on its own -- an unrelated manual press must not resurrect either. A
	// refusal to prepare the entry's routing is logged and swallowed rather than
	// blocking the go-live: the user pressed the button and is entitled to stream,
	// just not with this entry's claim attached.
	void AdoptImminentArmed();

	// Re-check one entry after it was edited or deleted. schedule.update can move an
	// armed entry out of its own arm window, and the row has to come back to
	// `planned` with it: left armed the chip reads armed until the new start, and a
	// manual go-live in between would stamp this entry's id onto an unrelated
	// session. A deleted entry the runner was holding is let go of here for the same
	// reason. Emits nothing -- the caller changed the entry and is already pushing.
	void NoteEntryChanged(const std::string &id);

	bool IsCountdownCanceled(const std::string &id) const;

	// Whether this entry's broadcast is under way -- either already live, or a start
	// asked for and not yet reported. While it answers true the entry is committed:
	// it cannot be cancelled, it cannot be deleted, its configuration is not put
	// back, the missed sweep leaves its row alone, and no other entry may take the
	// routing. Every one of those would otherwise redirect or orphan a broadcast
	// that is running or about to be.
	//
	// True for as long as the entry is live however it was started -- a manual
	// go-live during the armed window is still this entry's broadcast, because
	// AdoptImminentArmed loads that entry's routing and asks for it before goLive
	// runs, not because being merely armed was ever taken as being started. A start
	// that has not reported expires after kStartInFlightMs, because nothing here can
	// tell a slow prelude from one that will never report at all, and holding the
	// user's routing forever on a start that died is its own failure. NoteStartFailed
	// ends it early on the one refusal the system does observe.
	bool IsStartInFlight(const std::string &id) const;

	// Why this occurrence cannot go live, or empty when nothing is wrong. Surfaced
	// alongside the state so a refused auto-start reads as an explanation rather
	// than as an entry that silently did not happen.
	std::string BlockReason(const std::string &id) const;

	// The entry a broadcast starting now belongs to -- the live one, else the one
	// that asked to start, else empty. What stamps schedule_id onto the session
	// row, for a manual go-live adopted during the armed window as much as for an
	// auto-start; an armed entry nothing asked to start is not this broadcast's,
	// so there is no soonest-armed fallback here.
	//
	// Re-read from the row rather than answered from the cached id: the missed
	// sweep can settle an entry between the tick that armed it and the go-live
	// edge, and a session pointing at an entry that reads `missed` is a
	// disagreement nothing later resolves.
	std::string ActiveEntryId();

	// The broadcast edges, called from the one place that observes them. Going
	// live is what moves an armed entry to `live`: the request alone does not,
	// since outputs that never come up are not a broadcast.
	void NoteWentLive();
	void NoteStoppedStreaming();

	// The go-live was refused before anything started -- a destination that could
	// not be prepared, or a stop that landed during the prelude. There is no stop
	// edge for a broadcast that never began, so this is the only word the runner
	// gets: it ends the in-flight start and puts the routing back rather than
	// holding both for the full allowance a genuinely slow prelude is given.
	//
	// Takes no id on purpose. The runner knows which start it asked for, and the
	// refusal happens far from anything that does.
	void NoteStartFailed();

private:
	// Per-occurrence state that is deliberately not persisted: it describes this
	// run of this app, and a restart is a fresh reading of the clock.
	struct Occurrence {
		bool canceled = false;
		bool countdownOpen = false;
		bool startRequested = false;
		// The go-live came back refused. Deliberately separate from clearing
		// startRequested: that flag means "this occurrence has asked", and
		// clearing it would have the next tick ask again, and the one after
		// that -- retrying a refused prepare against the platform at 1 Hz.
		bool startFailed = false;
		// When the start was asked for, which is what kStartInFlightMs runs from.
		// Meaningless unless startRequested.
		int64_t startRequestedAtMs = 0;
		std::string blockReason;
	};

	bool PruneStale(int64_t now);
	bool DisarmIfOutOfWindow(const ScheduleEntry &entry, int64_t now);
	bool ArmIfDue(ScheduleEntryWithDestinations &row, int64_t now);
	bool RefreshArmability(const ScheduleEntryWithDestinations &row);
	bool RoutingHeldElsewhere(const std::string &id) const;
	void OpenCountdownIfDue(const ScheduleEntryWithDestinations &row, int64_t now);
	bool StartIfDue(const ScheduleEntryWithDestinations &row, int64_t now);
	// Everything a start does short of the go-live call itself: the block-reason
	// refusal, loading the entry's destinations into the go-live path, and marking
	// the occurrence as requested. Split out from RequestStart for AdoptImminentArmed,
	// which runs from inside a manual go-live already under way -- calling goLive()
	// from there would re-enter it. A refusal is reported through `error` rather than
	// latched onto the occurrence: StartIfDue is the one caller that still latches,
	// by calling SetBlockReason itself with what this returns, since a manual start
	// or an adoption has nowhere to latch a reason that would otherwise stick around
	// blocking a clock-driven retry it was never asked to make.
	bool PrepareStart(const ScheduleEntryWithDestinations &row, int64_t now, std::string &error);
	// PrepareStart, then the log line and the go-live call -- the body of a start
	// that does end in goLive(), shared by the clock (StartIfDue) and the explicit
	// StartNow. `verb` is only the log wording ("auto-starting" vs. a manual start's
	// own), so the one line both callers emit still reads as what actually happened.
	bool RequestStart(const ScheduleEntryWithDestinations &row, int64_t now, std::string &error,
			  const char *verb = "auto-starting");
	bool Armable(const ScheduleEntryWithDestinations &row, std::string &reason) const;
	bool SetBlockReason(const std::string &id, const std::string &reason);
	void SettleApplied();
	void RevertApplied();
	void ForgetArmed(const std::string &id);
	// The armed entry whose start comes first, which is the one a broadcast started
	// by hand belongs to. Reads the rows rather than assuming arm order: an entry
	// created inside another's arm window arms later and starts sooner.
	std::string ImminentArmedId();
	// Every occurrence whose broadcast is under way, for the sweep to leave alone.
	std::vector<std::string> InFlightIds() const;
	void NoteStoreError(const char *what);
	void Log(const std::string &line) const;

	ScheduleStore *store_ = nullptr;
	std::unordered_map<std::string, Occurrence> occurrences_;
	// Every entry currently armed, not just the last one to arm. Windows overlap --
	// a five-minute lead covers any entry starting within it -- and a single slot
	// would hand the go-live edge to whichever armed most recently rather than to
	// the one that is actually starting.
	std::vector<std::string> armedIds_;
	// The entry whose start this runner requested. It owns the go-live edge over any
	// merely-armed one, since that broadcast is the one it asked for.
	std::string startingId_;
	std::string liveId_;
	// The occurrence whose configuration is currently loaded into the go-live path.
	std::string appliedId_;
	// The last store failure reported, so a database that stays broken is said once
	// rather than once a second.
	std::string lastStoreError_;
};

} // namespace History
