#include "ScheduleRunner.hpp"

namespace History {

void ScheduleRunner::Attach(ScheduleStore *store)
{
	store_ = store;
	occurrences_.clear();
	armedId_.clear();
	liveId_.clear();
	appliedId_.clear();
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
		StartIfDue(row, now);
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
// enough that the chip no longer needs the explanation. Also disarms the row this
// runner armed if it was rescheduled out of reach of its own arm window.
bool ScheduleRunner::PruneStale(int64_t now)
{
	bool changed = false;
	for (auto it = occurrences_.begin(); it != occurrences_.end();) {
		const std::unique_ptr<ScheduleEntry> entry = store_->Get(it->first);
		const bool keep = entry && entry->startsAt - now <= kArmLeadMs &&
				  now - entry->startsAt <= kOccurrenceRetentionMs;
		if (keep) {
			++it;
			continue;
		}
		changed = changed || it->second.canceled || !it->second.blockReason.empty();
		it = occurrences_.erase(it);
	}

	// The armed row is tracked separately because an entry can be armed without ever
	// accumulating occurrence state -- one that is not auto-start and has nothing
	// wrong with it never touches the map.
	if (!armedId_.empty()) {
		const std::unique_ptr<ScheduleEntry> armed = store_->Get(armedId_);
		if (!armed || armed->state != ScheduleState::kArmed) {
			armedId_.clear();
		} else if (DisarmIfOutOfWindow(*armed, now)) {
			changed = true;
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
	if (!store_->SetState(entry.id, ScheduleState::kPlanned)) {
		return false;
	}
	Log("[schedule] disarmed '" + entry.title + "' (" + entry.id + "): it was moved out of its arm window");
	occurrences_.erase(entry.id);
	if (armedId_ == entry.id) {
		armedId_.clear();
	}
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
	}
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

	// Arming is not a check that the entry could go live -- it is loading it in.
	// A previous occurrence's routing is put back first so two applications cannot
	// stack and leave the second one unable to restore what was really there.
	RevertApplied();
	armedId_ = entry.id;
	appliedId_ = entry.id;
	if (applyEntry) {
		applyEntry(row.destinations);
	}
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
	std::string reason;
	return SetBlockReason(row.entry.id, Armable(row, reason) ? std::string() : reason);
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

void ScheduleRunner::StartIfDue(const ScheduleEntryWithDestinations &row, int64_t now)
{
	const ScheduleEntry &entry = row.entry;
	if (entry.state != ScheduleState::kArmed || entry.autoStart == 0 || now < entry.startsAt) {
		return;
	}
	const auto it = occurrences_.find(entry.id);
	if (it != occurrences_.end() && (it->second.canceled || it->second.startRequested)) {
		return;
	}
	// Broadcasting to nowhere is worse than not broadcasting. RefreshArmability has
	// already asked this tick and keeps asking through the grace window, so a
	// destination that comes back fifteen seconds late still gets to go live.
	if (!BlockReason(entry.id).empty()) {
		return;
	}
	occurrences_[entry.id].startRequested = true;
	Log("[schedule] auto-starting '" + entry.title + "' (" + entry.id + ")");
	if (goLive) {
		goLive();
	}
	// The state stays `armed` until the outputs actually come up. A request is not
	// a broadcast, and an entry that reads `live` while nothing is streaming is the
	// one claim this feature must never make.
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
void ScheduleRunner::SettleApplied()
{
	if (appliedId_.empty()) {
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
	if (!store_->SetState(id, ScheduleState::kPlanned)) {
		error = "failed to disarm the entry: " + store_->LastError();
		return false;
	}
	Occurrence &occurrence = occurrences_[id];
	occurrence.canceled = true;
	occurrence.countdownOpen = false;
	occurrence.startRequested = false;
	occurrence.blockReason.clear();
	if (armedId_ == id) {
		armedId_.clear();
	}
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

std::string ScheduleRunner::BlockReason(const std::string &id) const
{
	const auto it = occurrences_.find(id);
	return it == occurrences_.end() ? std::string() : it->second.blockReason;
}

const std::string &ScheduleRunner::ActiveEntryId() const
{
	return liveId_.empty() ? armedId_ : liveId_;
}

void ScheduleRunner::NoteWentLive()
{
	if (!store_ || !store_->IsAttached() || armedId_.empty()) {
		return;
	}
	const std::unique_ptr<ScheduleEntry> entry = store_->Get(armedId_);
	if (!entry || entry->state != ScheduleState::kArmed) {
		armedId_.clear();
		return;
	}
	if (!store_->SetState(armedId_, ScheduleState::kLive)) {
		return;
	}
	liveId_ = armedId_;
	armedId_.clear();
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
