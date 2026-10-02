#include "goal_registry.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>

#include "../bridge.hpp"
#include "../event_names.hpp"
#include "util/async_task.hpp"
#include "util/time_util.hpp"

namespace Chat {

using json = nlohmann::json;

namespace {

// Goals reach the dock only from the YouTube chat read today; the wire names the platform so a
// second one is a new producer, not a new shape.
constexpr const char *kPlatform = "youtube";

std::string GoalId(const OAuth::DestinationId &dest, const std::string &key)
{
	return OAuth::DestinationKey(dest) + "|" + key;
}

// Achieved and ended are over; active and a state not seen yet are still showing.
bool IsOver(const YouTubeGoal::Goal &goal)
{
	const char *phase = YouTubeGoal::Phase(goal.state);
	return std::strcmp(phase, "achieved") == 0 || std::strcmp(phase, "ended") == 0;
}

} // namespace

uint64_t GoalRegistry::BeginSession()
{
	const std::lock_guard<std::mutex> lock(mutex_);
	return nextSession_++;
}

json GoalRegistry::ToJson(const Entry &entry)
{
	json out = YouTubeGoal::ToJson(entry.goal);
	out["id"] = entry.id;
	out["platform"] = kPlatform;
	out["accountId"] = entry.dest.accountId;
	out["profileUuid"] = entry.dest.profileUuid;
	out["updatedAtMs"] = entry.updatedAtMs;
	out["endedAtMs"] = entry.endedAtMs ? json(*entry.endedAtMs) : json(nullptr);
	return out;
}

json GoalRegistry::ListLocked() const
{
	json rows = json::array();
	for (const Entry &entry : goals_) {
		rows.push_back(ToJson(entry));
	}
	return json{{"goals", std::move(rows)}};
}

void GoalRegistry::Apply(const OAuth::DestinationId &dest, uint64_t session,
			 const std::vector<YouTubeGoal::Patch> &patches)
{
	if (patches.empty()) {
		return;
	}
	const std::lock_guard<std::mutex> emitLock(emitMutex_);
	json list;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		bool changed = false;
		const int64_t now = TimeUtil::NowMs();
		for (const YouTubeGoal::Patch &patch : patches) {
			const std::string id = GoalId(dest, patch.key);
			auto it = std::find_if(goals_.begin(), goals_.end(),
					       [&id](const Entry &e) { return e.id == id; });
			if (patch.kind == YouTubeGoal::MutationKind::Delete) {
				if (it != goals_.end()) {
					goals_.erase(it);
					changed = true;
				}
				continue;
			}
			if (it == goals_.end()) {
				Entry entry;
				entry.id = id;
				entry.dest = dest;
				goals_.push_back(std::move(entry));
				it = std::prev(goals_.end());
			}
			// Taking a goal over is not a visible change; only what the goal says is.
			it->session = session;
			const json before = YouTubeGoal::ToJson(it->goal);
			const bool fresh = it->goal.key.empty();
			YouTubeGoal::Apply(it->goal, patch);
			if (!fresh && YouTubeGoal::ToJson(it->goal) == before) {
				continue;
			}
			changed = true;
			it->updatedAtMs = now;
			if (!IsOver(it->goal)) {
				it->endedAtMs.reset();
			} else if (!it->endedAtMs) {
				it->endedAtMs = now;
			}
		}
		if (!changed) {
			return;
		}
		list = ListLocked();
	}
	sink_(list);
}

void GoalRegistry::EndSession(const OAuth::DestinationId &dest, uint64_t session)
{
	const std::lock_guard<std::mutex> emitLock(emitMutex_);
	json list;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		const auto owned = [&dest, session](const Entry &e) {
			return e.session == session && e.dest == dest;
		};
		const auto first = std::remove_if(goals_.begin(), goals_.end(), owned);
		if (first == goals_.end()) {
			return;
		}
		goals_.erase(first, goals_.end());
		list = ListLocked();
	}
	sink_(list);
}

json GoalRegistry::List() const
{
	const std::lock_guard<std::mutex> lock(mutex_);
	return ListLocked();
}

GoalRegistry &Goals()
{
	// Queued even on the UI thread, as PollRegistry::EmitChanged explains.
	static GoalRegistry registry([](const json &list) {
		AsyncTask::QueueOnUi([list] { Bridge::EmitEvent(EventNames::kGoalsChanged, list); });
	});
	return registry;
}

} // namespace Chat
