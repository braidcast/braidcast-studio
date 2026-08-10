#include "ScheduleRunner.hpp"

namespace History {

void ScheduleRunner::Attach(ScheduleStore *store)
{
	store_ = store;
	occurrences_.clear();
	armedId_.clear();
	liveId_.clear();
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

	if (changed && onChanged) {
		onChanged();
	}
}

// Drops the per-occurrence state of entries that are gone or have been moved far
// enough into the future to be a different occurrence, and lets go of an armed or
// live entry whose row no longer agrees. Everything else -- including a cancel on
// an entry that has since settled as missed -- is kept, because the chip still
// has to explain it.
bool ScheduleRunner::PruneStale(int64_t now)
{
	bool changed = false;
	for (auto it = occurrences_.begin(); it != occurrences_.end();) {
		const std::unique_ptr<ScheduleEntry> entry = store_->Get(it->first);
		if (!entry || entry->startsAt - now > kArmLeadMs) {
			changed = changed || it->second.canceled || !it->second.blockReason.empty();
			it = occurrences_.erase(it);
			continue;
		}
		++it;
	}
	if (!armedId_.empty()) {
		const std::unique_ptr<ScheduleEntry> armed = store_->Get(armedId_);
		if (!armed || armed->state != ScheduleState::kArmed) {
			armedId_.clear();
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
	armedId_ = entry.id;
	Log("[schedule] armed '" + entry.title + "' (" + entry.id + ")");

	// Evaluated here rather than only at T-0, so a deleted profile or a
	// disconnected account is visible for the whole five minutes it can still be
	// fixed in.
	std::string reason;
	if (!Armable(row, reason)) {
		SetBlockReason(entry.id, reason);
	}
	return true;
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
	{
		const auto it = occurrences_.find(entry.id);
		if (it != occurrences_.end() && (it->second.canceled || it->second.startRequested)) {
			return false;
		}
	}

	// Broadcasting to nowhere is worse than not broadcasting, so this refuses
	// rather than starting, and it is re-tried on every tick of the grace window:
	// an account reconnected fifteen seconds late still gets to go live.
	std::string reason;
	if (!Armable(row, reason)) {
		return SetBlockReason(entry.id, reason);
	}

	const bool cleared = SetBlockReason(entry.id, std::string());
	occurrences_[entry.id].startRequested = true;
	Log("[schedule] auto-starting '" + entry.title + "' (" + entry.id + ")");
	if (goLive) {
		goLive();
	}
	// The state stays `armed` until the outputs actually come up. A request is not
	// a broadcast, and an entry that reads `live` while nothing is streaming is the
	// one claim this feature must never make.
	return cleared;
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
	Occurrence &occurrence = occurrences_[id];
	if (occurrence.blockReason == reason) {
		return false;
	}
	occurrence.blockReason = reason;
	if (!reason.empty()) {
		Log("[schedule] " + id + " cannot go live: " + reason);
	}
	return true;
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
	Log("[schedule] '" + id + "' finished");
	if (onChanged) {
		onChanged();
	}
}

} // namespace History
