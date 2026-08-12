#include "ScheduleRunner.hpp"

#include <algorithm>

#include "ScheduledSetup.hpp"

namespace History {

void ScheduleRunner::Attach(ScheduleStore *store)
{
	store_ = store;
	occurrences_.clear();
	armedIds_.clear();
	startingId_.clear();
	liveId_.clear();
	appliedId_.clear();
}

bool ScheduleRunner::IsArmed(const std::string &id) const
{
	return std::find(armedIds_.begin(), armedIds_.end(), id) != armedIds_.end();
}

void ScheduleRunner::RememberArmed(const std::string &id)
{
	if (!IsArmed(id)) {
		armedIds_.push_back(id);
	}
}

void ScheduleRunner::ForgetArmed(const std::string &id)
{
	for (auto it = armedIds_.begin(); it != armedIds_.end(); ++it) {
		if (*it == id) {
			armedIds_.erase(it);
			return;
		}
	}
}

std::string ScheduleRunner::ImminentArmedId()
{
	std::string best;
	int64_t bestStart = 0;
	for (const std::string &id : armedIds_) {
		const std::unique_ptr<ScheduleEntry> row = store_->Get(id);
		if (!row || row->state != ScheduleState::kArmed) {
			continue;
		}
		if (best.empty() || row->startsAt < bestStart) {
			best = id;
			bestStart = row->startsAt;
		}
	}
	return best;
}

void ScheduleRunner::Detach()
{
	Attach(nullptr);
}

void ScheduleRunner::Log(const std::string &line) const
{
	if (log) {
		log(line);
	}
}

// A store that stops answering stops the whole feature: ListRange returns nothing
// so no entry ever arms, and the sweep settles nothing. Both report through
// LastError() and neither throws, so without this a locked or corrupt history.db
// turns every tick into a silent no-op in a subsystem that logs every transition.
// Said once per distinct failure -- this runs at 1 Hz.
void ScheduleRunner::NoteStoreError(const char *what)
{
	const std::string error = store_->LastError();
	if (error.empty()) {
		lastStoreError_.clear();
		return;
	}
	if (error == lastStoreError_) {
		return;
	}
	lastStoreError_ = error;
	Log("[schedule] " + std::string(what) + " failed; nothing is being scheduled: " + error);
}

void ScheduleRunner::Tick()
{
	if (!store_ || !store_->IsAttached() || !nowMs) {
		return;
	}
	const int64_t now = nowMs();
	bool changed = PruneStale(now);

	// One read per tick, over the only window anything can happen in: from the arm
	// lead ahead of now back to the grace behind it, where a late start or the
	// missed sweep settles an entry.
	std::vector<ScheduleEntryWithDestinations> rows = store_->ListRange(now - kMissedGraceMs, now + kArmLeadMs + 1);
	NoteStoreError("reading the schedule");

	for (ScheduleEntryWithDestinations &row : rows) {
		if (ArmIfDue(row, now)) {
			changed = true;
		}
		if (RefreshArmability(row)) {
			changed = true;
		}
		OpenCountdownIfDue(row, now);
		if (StartIfDue(row, now)) {
			changed = true;
		}
	}

	// Settled last, so an entry that reached T-0 on this tick had its chance to
	// start before the same tick could call it missed. A start still on its way up
	// is excluded: its row has to stay `armed` to reach `live`, or the broadcast it
	// is about to bring up would be filed as missed and its session left unlinked.
	const int missed = store_->SweepMissed(now - kMissedGraceMs, InFlightIds());
	NoteStoreError("the missed sweep");
	if (missed > 0) {
		Log("[schedule] " + std::to_string(missed) + " entr(ies) missed their start");
		changed = true;
	}

	// After the sweep, so an occurrence that settled on this tick puts the routing
	// back on the same tick rather than a second later.
	SettleApplied();

	if (changed && onChanged) {
		onChanged();
	}
}

// Drops the per-occurrence state of entries that are gone, have been moved far
// enough into the future to be a different occurrence, or have been settled long
// enough that the chip no longer needs the explanation. Also disarms any row this
// runner armed that was rescheduled out of reach of its own arm window.
bool ScheduleRunner::PruneStale(int64_t now)
{
	bool changed = false;
	for (auto it = occurrences_.begin(); it != occurrences_.end();) {
		const std::unique_ptr<ScheduleEntry> entry = store_->Get(it->first);
		// An occurrence whose start is on its way up is kept whatever the row says.
		// Dropping it here answers "is this start committed" with no, and every
		// path that asks -- the settle, the disarm, cancelling, deleting -- would
		// then put the routing back under outputs still coming up.
		const bool keep = IsStartInFlight(it->first) || (entry && entry->startsAt - now <= kArmLeadMs &&
								 now - entry->startsAt <= kOccurrenceRetentionMs);
		if (keep) {
			++it;
			continue;
		}
		changed = changed || it->second.canceled || !it->second.blockReason.empty() ||
			  !it->second.destinationRefusals.empty();
		it = occurrences_.erase(it);
	}

	// Armed rows are tracked separately because an entry can be armed without ever
	// accumulating occurrence state -- one that is not auto-start and has nothing
	// wrong with it never touches the map -- and because an entry moved out of its
	// own arm window is no longer in the range the tick reads.
	const std::vector<std::string> armed = armedIds_;
	armedIds_.clear();
	for (const std::string &id : armed) {
		const std::unique_ptr<ScheduleEntry> row = store_->Get(id);
		if (!row || row->state != ScheduleState::kArmed) {
			continue;
		}
		if (DisarmIfOutOfWindow(*row, now)) {
			changed = true;
			continue;
		}
		RememberArmed(id);
	}

	// Held for as long as the start is on its way up, the same predicate the
	// occurrence above is kept on. Dropping it while the outputs are still coming up
	// hands the go-live edge to whichever entry happens to be armed, and this one
	// loses the broadcast it asked for.
	if (!startingId_.empty() && !IsStartInFlight(startingId_)) {
		const std::unique_ptr<ScheduleEntry> starting = store_->Get(startingId_);
		if (!starting || starting->state != ScheduleState::kArmed) {
			startingId_.clear();
		}
	}
	if (!liveId_.empty()) {
		const std::unique_ptr<ScheduleEntry> live = store_->Get(liveId_);
		if (!live || live->state != ScheduleState::kLive) {
			liveId_.clear();
		}
	}
	return changed;
}

bool ScheduleRunner::DisarmIfOutOfWindow(const ScheduleEntry &entry, int64_t now)
{
	if (entry.state != ScheduleState::kArmed || entry.startsAt - now <= kArmLeadMs) {
		return false;
	}
	// A start on its way up is committed: disarming would put the routing back out
	// from under outputs that are still coming up.
	if (IsStartInFlight(entry.id)) {
		return false;
	}
	if (!store_->SetState(entry.id, ScheduleState::kPlanned)) {
		return false;
	}
	Log("[schedule] disarmed '" + entry.title + "' (" + entry.id + "): it was moved out of its arm window");
	occurrences_.erase(entry.id);
	ForgetArmed(entry.id);
	if (appliedId_ == entry.id) {
		RevertApplied();
	}
	return true;
}

void ScheduleRunner::NoteEntryChanged(const std::string &id)
{
	if (!store_ || !store_->IsAttached() || !nowMs) {
		return;
	}
	const std::unique_ptr<ScheduleEntry> entry = store_->Get(id);
	if (entry) {
		DisarmIfOutOfWindow(*entry, nowMs());
		return;
	}
	// Deleted. Nothing can be transitioned, but everything the runner was holding
	// on its behalf has to go, or it would keep stamping a dead id onto sessions.
	const bool committed = IsStartInFlight(id);
	occurrences_.erase(id);
	ForgetArmed(id);
	if (startingId_ == id) {
		startingId_.clear();
	}
	if (liveId_ == id) {
		liveId_.clear();
	}
	if (appliedId_ != id) {
		return;
	}
	if (committed) {
		// The row is gone; the broadcast it asked for is not. Putting the routing
		// back now is exactly what CancelCountdown refuses to do -- the outputs
		// are on their way up and would go out to whatever was there before. The
		// runner lets go, and the broadcast's own stop edge restores it.
		appliedId_.clear();
		Log("[schedule] '" + id + "' was deleted while going live; its routing stays until the broadcast ends");
		return;
	}
	RevertApplied();
}

bool ScheduleRunner::ArmIfDue(ScheduleEntryWithDestinations &row, int64_t now)
{
	const ScheduleEntry &entry = row.entry;
	if (entry.state != ScheduleState::kPlanned || entry.startsAt <= now || entry.startsAt - now > kArmLeadMs) {
		return false;
	}
	const auto it = occurrences_.find(entry.id);
	if (it != occurrences_.end() && it->second.canceled) {
		return false;
	}
	if (!store_->SetState(entry.id, ScheduleState::kArmed)) {
		return false;
	}
	row.entry.state = ScheduleState::kArmed;
	RememberArmed(entry.id);
	// Deliberately read-only. Arming surfaces the entry and starts asking whether it
	// could go live; loading it into the routing waits for T-0, so configuration the
	// user owns is only ever ours for the length of one go-live.
	Log("[schedule] armed '" + entry.title + "' (" + entry.id + ")");
	return true;
}

// Asked on every tick while armed rather than once at the arm instant, so an
// account that disconnects during the five minutes shows up on the chip, and one
// that reconnects clears the reason again instead of leaving a stale refusal.
bool ScheduleRunner::RefreshArmability(const ScheduleEntryWithDestinations &row)
{
	if (row.entry.state != ScheduleState::kArmed) {
		return false;
	}
	// Asked even while the routing is held elsewhere, because this sweep is the only
	// thing that keeps the per-destination reasons current -- skipping it there would
	// freeze them at whatever the last unheld tick saw, and a chip that stopped being
	// true is the failure the refresh exists to avoid. Only the entry-level answer is
	// overridden below.
	std::string reason;
	bool changed = false;
	const bool armable = Armable(row, reason, &changed);
	if (RoutingHeldElsewhere(row.entry.id)) {
		return SetBlockReason(row.entry.id, kAlreadyStreamingReason) || changed;
	}
	return SetBlockReason(row.entry.id, armable ? std::string() : reason) || changed;
}

// Whether something else already owns the routing. Two questions, not one: whether
// THIS RUNNER has another occurrence's start on its way up, and whether anything at all
// is streaming. The first is the only one appliedId_/IsStartInFlight can answer -- a
// go-live started from the modal, a hotkey or the tray is invisible to it, since no
// occurrence asked for it. That one is isStreaming's to catch, which is why isStreaming
// has to answer true for a go-live that has not brought its outputs up yet as well as
// for one that has.
//
// Answered once per tick, in the one place that computes block reasons, so
// StartIfDue's existing gate does the refusing. Asking again there would set a
// reason RefreshArmability had just cleared and push an event every second.
bool ScheduleRunner::RoutingHeldElsewhere(const std::string &id) const
{
	if (IsStartInFlight(id)) {
		return false; // whatever is coming up is this entry's own
	}
	if (!appliedId_.empty() && appliedId_ != id && IsStartInFlight(appliedId_)) {
		return true;
	}
	return isStreaming && isStreaming();
}

// The countdown is a window, not a server-side counter: the client already has
// starts_at and renders the seconds itself. All this does is note that the window
// opened and say so once. Cancelling is gated on the entry being armed and never
// reads this, so it works for the whole armed window and for an entry that does not
// auto-start at all -- there is nothing here to become cancellable.
void ScheduleRunner::OpenCountdownIfDue(const ScheduleEntryWithDestinations &row, int64_t now)
{
	const ScheduleEntry &entry = row.entry;
	if (entry.state != ScheduleState::kArmed || entry.autoStart == 0 || entry.startsAt <= now ||
	    entry.startsAt - now > kCountdownLeadMs) {
		return;
	}
	Occurrence &occurrence = occurrences_[entry.id];
	if (occurrence.canceled || occurrence.countdownOpen) {
		return;
	}
	occurrence.countdownOpen = true;
	Log("[schedule] countdown open for '" + entry.title + "' (" + entry.id + ")");
}

bool ScheduleRunner::StartIfDue(const ScheduleEntryWithDestinations &row, int64_t now)
{
	const ScheduleEntry &entry = row.entry;
	if (entry.state != ScheduleState::kArmed || entry.autoStart == 0 || now < entry.startsAt) {
		return false;
	}
	// The raw flag, not IsStartInFlight: this asks whether the occurrence has ever
	// asked to start, and one whose request was given up on must not ask again.
	if (IsCountdownCanceled(entry.id) || HasRequestedStart(entry.id)) {
		return false;
	}
	std::string error;
	if (!RequestStart(row, now, error)) {
		return SetBlockReason(entry.id, error);
	}
	// The state stays `armed` until the outputs actually come up. A request is not
	// a broadcast, and an entry that reads `live` while nothing is streaming is the
	// one claim this feature must never make. The request itself is reported though
	// -- startRequested is what tells a client the entry is spoken for while it still
	// reads `armed` -- and the gate above means this is reached once per occurrence.
	return true;
}

bool ScheduleRunner::PrepareStart(const ScheduleEntryWithDestinations &row, int64_t now, std::string &error)
{
	const ScheduleEntry &entry = row.entry;
	// Broadcasting to nowhere is worse than not broadcasting. RefreshArmability has
	// already asked this tick and keeps asking through the grace window, so a
	// destination that comes back fifteen seconds late still gets to go live.
	error = BlockReason(entry.id);
	if (!error.empty()) {
		return false;
	}
	// The entry becomes the live configuration here and nowhere earlier. A refusal is
	// left unlatched on purpose: the next tick recomputes it, so a blocker that
	// clears inside the grace window still gets its start.
	if (applyEntry) {
		RevertApplied();
		if (!applyEntry(row.destinations, error)) {
			if (error.empty()) {
				error = "could not load this entry's destinations";
			}
			return false;
		}
		appliedId_ = entry.id;
	}
	// Without requireAllDestinations an entry is allowed to start without everything
	// it names, and its own block reason is empty when it does -- so this line is the
	// only record that the broadcast went out to fewer destinations than were planned.
	// Reached once per occurrence, like the start it belongs to.
	const std::vector<DestinationRefusal> *refusals = RefusalsFor(entry.id);
	if (refusals && !refusals->empty()) {
		Log("[schedule] '" + entry.title + "' (" + entry.id +
		    ") is going live without: " + JoinRefusals(*refusals));
	}
	Occurrence &occurrence = occurrences_[entry.id];
	occurrence.startRequested = true;
	occurrence.startRequestedAtMs = now;
	startingId_ = entry.id;
	return true;
}

bool ScheduleRunner::RequestStart(const ScheduleEntryWithDestinations &row, int64_t now, std::string &error,
				  const char *verb)
{
	if (!PrepareStart(row, now, error)) {
		return false;
	}
	Log("[schedule] " + std::string(verb) + " '" + row.entry.title + "' (" + row.entry.id + ")");
	if (goLive) {
		goLive();
	}
	return true;
}

std::string ScheduleRunner::JoinRefusals(const std::vector<DestinationRefusal> &refusals)
{
	std::string out;
	for (const DestinationRefusal &refusal : refusals) {
		out += out.empty() ? refusal.reason : "; " + refusal.reason;
	}
	return out;
}

const std::vector<ScheduleRunner::DestinationRefusal> *ScheduleRunner::RefusalsFor(const std::string &id) const
{
	const auto it = occurrences_.find(id);
	return it == occurrences_.end() ? nullptr : &it->second.destinationRefusals;
}

std::string ScheduleRunner::DestinationBlockReason(const std::string &id, const std::string &profileId) const
{
	const std::vector<DestinationRefusal> *refusals = RefusalsFor(id);
	if (!refusals) {
		return {};
	}
	for (const DestinationRefusal &refusal : *refusals) {
		if (refusal.profileId == profileId) {
			return refusal.reason;
		}
	}
	return {};
}

bool ScheduleRunner::SetDestinationRefusals(const std::string &id, std::vector<DestinationRefusal> refusals)
{
	const auto existing = occurrences_.find(id);
	if (refusals.empty() && existing == occurrences_.end()) {
		return false; // nothing tracked and nothing to say: do not start tracking
	}
	Occurrence &occurrence = existing == occurrences_.end() ? occurrences_[id] : existing->second;
	const std::vector<DestinationRefusal> &before = occurrence.destinationRefusals;
	bool same = before.size() == refusals.size();
	for (size_t i = 0; same && i < refusals.size(); i++) {
		same = before[i].profileId == refusals[i].profileId && before[i].reason == refusals[i].reason;
	}
	if (same) {
		return false;
	}
	occurrence.destinationRefusals = std::move(refusals);
	return true;
}

bool ScheduleRunner::Armable(const ScheduleEntryWithDestinations &row, std::string &reason, bool *changed)
{
	if (row.destinations.empty()) {
		reason = "this entry has no destinations";
		return false;
	}
	if (!canArm) {
		const bool moved = SetDestinationRefusals(row.entry.id, {});
		if (changed) {
			*changed = moved;
		}
		return true;
	}
	// Every destination, never stopping at the first that can route. An entry naming
	// three that went live on one used to say nothing at all about the other two --
	// the answer it gave was about the entry, and the user was asking about their
	// destinations.
	std::vector<DestinationRefusal> refusals;
	size_t routable = 0;
	for (const ScheduleDestination &destination : row.destinations) {
		std::string why;
		if (canArm(destination.profileId, why)) {
			routable++;
			continue;
		}
		refusals.push_back({destination.profileId, why.empty() ? "it cannot go live" : why});
	}
	const bool moved = SetDestinationRefusals(row.entry.id, refusals);
	if (changed) {
		*changed = moved;
	}
	if (refusals.empty()) {
		return true;
	}
	reason = JoinRefusals(refusals);
	// The user's own call: strict refuses an entry that cannot reach everything it
	// names, rather than putting a broadcast on the air that is missing destinations
	// they planned for. Lenient keeps the older behavior and lets the subset go.
	const bool requireAll = requireAllDestinations && requireAllDestinations();
	return !requireAll && routable > 0;
}

bool ScheduleRunner::SetBlockReason(const std::string &id, const std::string &reason)
{
	const auto existing = occurrences_.find(id);
	if (reason.empty() && existing == occurrences_.end()) {
		return false; // nothing tracked and nothing to say: do not start tracking
	}
	Occurrence &occurrence = existing == occurrences_.end() ? occurrences_[id] : existing->second;
	if (occurrence.blockReason == reason) {
		return false;
	}
	occurrence.blockReason = reason;
	if (!reason.empty()) {
		Log("[schedule] " + id + " cannot go live: " + reason);
	}
	return true;
}

// The application belongs to one occurrence. It stays in place while that entry is
// armed or live, and is put back the moment it is anything else -- cancelled,
// disarmed, missed, or finished.
//
// The row settling is not the same thing as the start being over. kMissedGraceMs
// settles the row after two minutes so the calendar stops calling the entry
// upcoming; a prelude can still be working through platform round trips and RTMP
// retries well past that, and reverting under it would send that broadcast to
// whatever the user was pointing at before. So a start still on its way up holds
// its configuration even though the row already reads `missed`, the same refusal
// CancelCountdown and schedule.delete make.
void ScheduleRunner::SettleApplied()
{
	if (appliedId_.empty() || IsStartInFlight(appliedId_)) {
		return;
	}
	const std::unique_ptr<ScheduleEntry> entry = store_->Get(appliedId_);
	if (!entry || (entry->state != ScheduleState::kArmed && entry->state != ScheduleState::kLive)) {
		RevertApplied();
	}
}

void ScheduleRunner::RevertApplied()
{
	if (appliedId_.empty()) {
		return;
	}
	appliedId_.clear();
	if (revertEntry) {
		revertEntry();
	}
}

bool ScheduleRunner::CancelCountdown(const std::string &id, std::string &error)
{
	if (!store_ || !store_->IsAttached()) {
		error = "scheduling is unavailable: the history database did not open";
		return false;
	}
	const std::unique_ptr<ScheduleEntry> entry = store_->Get(id);
	if (!entry) {
		error = "no scheduled entry with id '" + id + "'";
		return false;
	}
	if (entry->state != ScheduleState::kArmed) {
		error = "that entry is " + entry->state + ", not armed";
		return false;
	}
	// Refused rather than obeyed once the start has been requested. Going live is
	// asynchronous, so the outputs from that request are still on their way up;
	// cancelling would put the routing back underneath them and the broadcast would
	// go out to whatever was there before. Stopping a broadcast is not what a
	// countdown cancel means, so this refuses instead.
	if (IsStartInFlight(id)) {
		error = "that entry is already going live";
		return false;
	}
	if (!store_->SetState(id, ScheduleState::kPlanned)) {
		error = "failed to disarm the entry: " + store_->LastError();
		return false;
	}
	Occurrence &occurrence = occurrences_[id];
	occurrence.canceled = true;
	occurrence.countdownOpen = false;
	occurrence.blockReason.clear();
	ForgetArmed(id);
	if (appliedId_ == id) {
		RevertApplied();
	}
	Log("[schedule] cancelled the countdown for '" + entry->title + "' (" + id + ")");
	if (onChanged) {
		onChanged();
	}
	return true;
}

bool ScheduleRunner::StartNow(const std::string &id, std::string &error)
{
	if (!store_ || !store_->IsAttached() || !nowMs) {
		error = "scheduling is unavailable: the history database did not open";
		return false;
	}
	const std::unique_ptr<ScheduleEntry> entry = store_->Get(id);
	if (!entry) {
		error = "no scheduled entry with id '" + id + "'";
		return false;
	}
	if (IsStartInFlight(id)) {
		error = "that entry is already going live";
		return false;
	}
	if (entry->state == ScheduleState::kLive || entry->state == ScheduleState::kDone ||
	    entry->state == ScheduleState::kCanceled) {
		error = "that entry is " + entry->state + " and cannot be started";
		return false;
	}
	ScheduleEntryWithDestinations row;
	row.entry = *entry;
	row.destinations = store_->DestinationsFor(id);
	std::string reason;
	bool refusalsMoved = false;
	const bool armable = Armable(row, reason, &refusalsMoved);
	// Pushed here rather than left to the clock. This is the only place the refusals
	// are recomputed for a row that is not armed, and the armability pass skips such a
	// row entirely -- and even for an armed one it would find the identical set and
	// report no movement. Without this the destination reasons a refused manual start
	// just worked out sit in memory until something unrelated repaints.
	if (refusalsMoved && onChanged) {
		onChanged();
	}
	if (!armable) {
		error = reason;
		return false;
	}
	if (RoutingHeldElsewhere(id)) {
		error = kAlreadyStreamingReason;
		return false;
	}
	if (entry->state != ScheduleState::kArmed) {
		if (!store_->SetState(id, ScheduleState::kArmed)) {
			error = "failed to arm the entry: " + store_->LastError();
			return false;
		}
		row.entry.state = ScheduleState::kArmed;
		RememberArmed(id);
	}
	Occurrence &occurrence = occurrences_[id];
	occurrence.canceled = false;
	occurrence.startRequested = false;
	occurrence.startFailed = false;
	// Also cleared: RequestStart re-checks BlockReason(id) as its own gate, and a
	// reason left over from a miss earlier in this occurrence's life would refuse a
	// start that the Armable/RoutingHeldElsewhere checks above just cleared.
	occurrence.blockReason.clear();
	if (!RequestStart(row, nowMs(), error, "starting by request")) {
		return false;
	}
	if (onChanged) {
		onChanged();
	}
	return true;
}

bool ScheduleRunner::IsCountdownCanceled(const std::string &id) const
{
	const auto it = occurrences_.find(id);
	return it != occurrences_.end() && it->second.canceled;
}

bool ScheduleRunner::HasRequestedStart(const std::string &id) const
{
	const auto it = occurrences_.find(id);
	return it != occurrences_.end() && it->second.startRequested;
}

bool ScheduleRunner::IsStartInFlight(const std::string &id) const
{
	// Checked before the occurrence, and deliberately not gated on startRequested:
	// a broadcast runs for as long as it runs, which outlasts the kStartInFlightMs
	// allowance below -- that bounds a request nothing ever reported, not a stream
	// that is up. Deleting a live entry mid-broadcast nulls the running session's
	// schedule_id.
	if (!id.empty() && liveId_ == id) {
		return true;
	}
	const auto it = occurrences_.find(id);
	if (it == occurrences_.end() || !it->second.startRequested || it->second.startFailed) {
		return false;
	}
	return nowMs && nowMs() - it->second.startRequestedAtMs < kStartInFlightMs;
}

std::vector<std::string> ScheduleRunner::InFlightIds() const
{
	std::vector<std::string> out;
	for (const auto &occurrence : occurrences_) {
		if (IsStartInFlight(occurrence.first)) {
			out.push_back(occurrence.first);
		}
	}
	return out;
}

void ScheduleRunner::NoteStartFailed()
{
	if (startingId_.empty()) {
		return;
	}
	const std::string id = startingId_;
	startingId_.clear();
	// The request is over, so the entry stops being committed: the sweep may settle
	// it, cancelling and deleting stop refusing, and another entry may take the
	// routing. Left as it was, all three stayed blocked for the full in-flight
	// allowance every time a token expired.
	const auto it = occurrences_.find(id);
	if (it != occurrences_.end()) {
		it->second.startFailed = true;
	}
	if (appliedId_ == id) {
		RevertApplied();
	}
	Log("[schedule] the go-live for '" + id + "' was refused; its routing is back");
	if (onChanged) {
		onChanged();
	}
}

std::string ScheduleRunner::BlockReason(const std::string &id) const
{
	const auto it = occurrences_.find(id);
	return it == occurrences_.end() ? std::string() : it->second.blockReason;
}

std::string ScheduleRunner::ActiveEntryId()
{
	if (!store_ || !store_->IsAttached()) {
		return {};
	}
	// The live one, else the one that asked to start. AdoptImminentArmed is what
	// turns a manual go-live into one of those two -- loading the armed entry's
	// routing and setting startingId_ before goLive runs -- so there is no third,
	// merely-armed fallback here: an entry nothing asked to start is not this
	// broadcast's, whatever the calendar says is coming up next.
	std::string id = liveId_;
	if (id.empty()) {
		id = startingId_;
	}
	if (id.empty()) {
		return {};
	}
	// Re-read rather than trusted: the missed sweep can settle an entry between the
	// tick that armed it and this go-live edge, and a session pointing at an entry
	// that reads `missed` is a disagreement nothing later resolves.
	const std::unique_ptr<ScheduleEntry> entry = store_->Get(id);
	if (!entry || (entry->state != ScheduleState::kArmed && entry->state != ScheduleState::kLive)) {
		return {};
	}
	return id;
}

void ScheduleRunner::AdoptImminentArmed()
{
	// The auto-start and StartNow both set startingId_ before calling goLive, and a
	// live broadcast already belongs to whoever is running it -- adoption only has
	// something to do on a go-live nothing has claimed yet.
	if (!startingId_.empty() || !liveId_.empty()) {
		return;
	}
	if (!store_ || !store_->IsAttached() || !nowMs) {
		return;
	}
	const std::string id = ImminentArmedId();
	if (id.empty()) {
		return;
	}
	// An occurrence that has already asked to start -- even one whose request was given
	// up on -- is not this go-live's to claim. A cancelled one is refused a step
	// earlier: cancelling puts the row back to `planned` and forgets the arm, so
	// ImminentArmedId cannot return it in the first place.
	if (HasRequestedStart(id)) {
		return;
	}
	// The other two PrepareStart callers gate themselves before reaching it; this one
	// has to do the same. PrepareStart opens with RevertApplied(), which drops
	// appliedId_ whatever happens, while ScheduledSetup refuses its own Apply outright
	// once something is streaming -- so a broadcast already running would cost an
	// applied entry its bookkeeping and strand that entry's routing changes with
	// nothing left that knows to put them back.
	if (RoutingHeldElsewhere(id)) {
		return;
	}
	const std::unique_ptr<ScheduleEntry> entry = store_->Get(id);
	if (!entry) {
		return;
	}
	ScheduleEntryWithDestinations row;
	row.entry = *entry;
	row.destinations = store_->DestinationsFor(id);
	std::string error;
	// PrepareStart rather than RequestStart: this runs from inside the manual
	// go-live itself, and RequestStart's goLive() call would re-enter it.
	if (!PrepareStart(row, nowMs(), error)) {
		Log("[schedule] could not adopt '" + entry->title + "' (" + id + ") into this manual start: " + error);
		return;
	}
	Log("[schedule] adopted '" + entry->title + "' (" + id + ") into a manually started broadcast");
}

void ScheduleRunner::NoteWentLive()
{
	if (!store_ || !store_->IsAttached()) {
		return;
	}
	// The entry that asked for this broadcast owns the edge. AdoptImminentArmed is
	// what makes a manual go-live during the armed window ask -- if nothing did,
	// this broadcast is not this runner's to claim, however plausible the calendar
	// makes it look.
	const std::string id = startingId_;
	startingId_.clear();
	if (id.empty()) {
		return;
	}
	const std::unique_ptr<ScheduleEntry> entry = store_->Get(id);
	if (!entry || entry->state != ScheduleState::kArmed) {
		ForgetArmed(id);
		return;
	}
	if (!store_->SetState(id, ScheduleState::kLive)) {
		return;
	}
	liveId_ = id;
	ForgetArmed(id);
	Log("[schedule] '" + entry->title + "' (" + liveId_ + ") is live");
	if (onChanged) {
		onChanged();
	}
}

void ScheduleRunner::NoteStoppedStreaming()
{
	if (!store_ || !store_->IsAttached() || liveId_.empty()) {
		return;
	}
	const std::string id = liveId_;
	liveId_.clear();
	if (!store_->SetState(id, ScheduleState::kDone)) {
		return;
	}
	occurrences_.erase(id);
	if (appliedId_ == id) {
		RevertApplied();
	}
	Log("[schedule] '" + id + "' finished");
	if (onChanged) {
		onChanged();
	}
}

} // namespace History
