#include "ScheduleRunner.hpp"

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
	// start before the same tick could call it missed.
	const int missed = store_->SweepMissed(now - kMissedGraceMs);
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
		const bool keep = IsStartRequested(it->first) || (entry && entry->startsAt - now <= kArmLeadMs &&
								  now - entry->startsAt <= kOccurrenceRetentionMs);
		if (keep) {
			++it;
			continue;
		}
		changed = changed || it->second.canceled || !it->second.blockReason.empty();
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
		armedIds_.push_back(id);
	}

	if (!startingId_.empty()) {
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
	if (IsStartRequested(entry.id)) {
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
	const bool committed = IsStartRequested(id);
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
	armedIds_.push_back(entry.id);
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
	if (RoutingHeldElsewhere(row.entry.id)) {
		return SetBlockReason(row.entry.id, kAlreadyStreamingReason);
	}
	std::string reason;
	return SetBlockReason(row.entry.id, Armable(row, reason) ? std::string() : reason);
}

// Whether something else already owns the routing. Two readings of one situation a
// few seconds apart: a broadcast whose outputs have reported, and a start that has
// asked but not reported yet. Only the first is visible to isStreaming, and a start
// spends its whole prelude in the second -- which is precisely the window a second
// entry would otherwise walk into and redirect.
//
// Answered once per tick, in the one place that computes block reasons, so
// StartIfDue's existing gate does the refusing. Asking again there would set a
// reason RefreshArmability had just cleared and push an event every second.
bool ScheduleRunner::RoutingHeldElsewhere(const std::string &id) const
{
	if (IsStartRequested(id)) {
		return false; // whatever is coming up is this entry's own
	}
	if (!appliedId_.empty() && appliedId_ != id && IsStartRequested(appliedId_)) {
		return true;
	}
	return isStreaming && isStreaming();
}

// The countdown is a window, not a server-side counter: the client already has
// starts_at and renders the seconds itself. What happens here is that the window
// becomes cancellable and says so once in the log.
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
	// The raw flag, not IsStartRequested: this asks whether the occurrence has ever
	// asked to start, and one whose request was given up on must not ask again.
	const auto it = occurrences_.find(entry.id);
	if (it != occurrences_.end() && (it->second.canceled || it->second.startRequested)) {
		return false;
	}
	// Broadcasting to nowhere is worse than not broadcasting. RefreshArmability has
	// already asked this tick and keeps asking through the grace window, so a
	// destination that comes back fifteen seconds late still gets to go live.
	if (!BlockReason(entry.id).empty()) {
		return false;
	}
	// The entry becomes the live configuration here and nowhere earlier. A refusal is
	// left unlatched on purpose: the next tick recomputes it, so a blocker that
	// clears inside the grace window still gets its start.
	if (applyEntry) {
		RevertApplied();
		std::string reason;
		if (!applyEntry(row.destinations, reason)) {
			return SetBlockReason(entry.id,
					      reason.empty() ? "could not load this entry's destinations" : reason);
		}
		appliedId_ = entry.id;
	}
	Occurrence &occurrence = occurrences_[entry.id];
	occurrence.startRequested = true;
	occurrence.startRequestedAtMs = now;
	startingId_ = entry.id;
	Log("[schedule] auto-starting '" + entry.title + "' (" + entry.id + ")");
	if (goLive) {
		goLive();
	}
	// The state stays `armed` until the outputs actually come up. A request is not
	// a broadcast, and an entry that reads `live` while nothing is streaming is the
	// one claim this feature must never make. Nothing schedule.list reports has
	// changed yet either, so this reports no change.
	return false;
}

bool ScheduleRunner::Armable(const ScheduleEntryWithDestinations &row, std::string &reason) const
{
	if (row.destinations.empty()) {
		reason = "this entry has no destinations";
		return false;
	}
	if (!canArm) {
		return true;
	}
	std::string problems;
	for (const ScheduleDestination &destination : row.destinations) {
		std::string why;
		if (canArm(destination.profileId, why)) {
			return true; // one destination is a broadcast
		}
		if (!why.empty()) {
			problems += problems.empty() ? why : "; " + why;
		}
	}
	reason = problems.empty() ? "no destination can go live" : problems;
	return false;
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
	if (appliedId_.empty() || IsStartRequested(appliedId_)) {
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
	if (IsStartRequested(id)) {
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

bool ScheduleRunner::IsCountdownCanceled(const std::string &id) const
{
	const auto it = occurrences_.find(id);
	return it != occurrences_.end() && it->second.canceled;
}

bool ScheduleRunner::IsStartRequested(const std::string &id) const
{
	const auto it = occurrences_.find(id);
	if (it == occurrences_.end() || !it->second.startRequested) {
		return false;
	}
	if (liveId_ == id) {
		return true;
	}
	return nowMs && nowMs() - it->second.startRequestedAtMs < kStartInFlightMs;
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
	// The live one, then the one that asked to start, then the soonest merely armed
	// -- which is the entry a broadcast the user started by hand belongs to.
	std::string id = liveId_;
	if (id.empty()) {
		id = startingId_.empty() ? ImminentArmedId() : startingId_;
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

void ScheduleRunner::NoteWentLive()
{
	if (!store_ || !store_->IsAttached()) {
		return;
	}
	// The entry that asked for this broadcast owns the edge; only when none did --
	// the user pressed go live during an armed window -- does a merely-armed one
	// claim it.
	const std::string id = startingId_.empty() ? ImminentArmedId() : startingId_;
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
