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

// A held goal whose grace ran out without a session adopting it.
bool Expired(const std::optional<int64_t> &heldUntilMs, int64_t now)
{
	return heldUntilMs && *heldUntilMs <= now;
}

} // namespace

uint64_t GoalRegistry::BeginSession(const OAuth::DestinationId &dest)
{
	const std::lock_guard<std::mutex> lock(mutex_);
	const uint64_t token = nextSession_++;
	sessions_.emplace(token, Session{dest});
	return token;
}

json GoalRegistry::ToJson(const Entry &entry)
{
	json out = YouTubeGoal::ToJson(entry.goal);
	out["id"] = entry.id;
	out["platform"] = kPlatform;
	out["accountId"] = entry.dest.accountId;
	out["profileUuid"] = entry.dest.profileUuid;
	out["endedAtMs"] = entry.endedAtMs ? json(*entry.endedAtMs) : json(nullptr);
	out["heldUntilMs"] = entry.heldUntilMs ? json(*entry.heldUntilMs) : json(nullptr);
	return out;
}

json GoalRegistry::ListLocked(int64_t now) const
{
	json rows = json::array();
	for (const Entry &entry : goals_) {
		if (!Expired(entry.heldUntilMs, now)) {
			rows.push_back(ToJson(entry));
		}
	}
	return json{{"goals", std::move(rows)}};
}

bool GoalRegistry::PurgeExpiredLocked(int64_t now)
{
	const auto first = std::remove_if(goals_.begin(), goals_.end(),
					  [now](const Entry &e) { return Expired(e.heldUntilMs, now); });
	const bool purged = first != goals_.end();
	goals_.erase(first, goals_.end());
	return purged;
}

void GoalRegistry::Apply(uint64_t session, const std::vector<YouTubeGoal::Patch> &patches)
{
	if (patches.empty()) {
		return;
	}
	const std::lock_guard<std::mutex> emitLock(emitMutex_);
	json list;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		const auto live = sessions_.find(session);
		if (live == sessions_.end() || live->second.retired) {
			return;
		}
		const OAuth::DestinationId dest = live->second.dest;
		const int64_t now = clock_();
		bool changed = PurgeExpiredLocked(now);
		const auto find = [this](const std::string &id) {
			return std::find_if(goals_.begin(), goals_.end(), [&id](const Entry &e) { return e.id == id; });
		};

		// Each touched goal as it was before this response, so it is judged on the net change.
		struct Before {
			std::string id;
			bool existed = false;
			bool wasOver = false;
			json goal;
		};
		std::vector<Before> touched;
		for (const YouTubeGoal::Patch &patch : patches) {
			const std::string id = GoalId(dest, patch.key);
			auto it = find(id);
			if (std::none_of(touched.begin(), touched.end(),
					 [&id](const Before &b) { return b.id == id; })) {
				const bool existed = it != goals_.end();
				touched.push_back(Before{id, existed, existed && IsOver(it->goal),
							 existed ? YouTubeGoal::ToJson(it->goal) : json()});
			}
			if (patch.kind == YouTubeGoal::MutationKind::Delete) {
				if (it != goals_.end()) {
					goals_.erase(it);
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
			YouTubeGoal::Apply(it->goal, patch);
		}

		for (const Before &before : touched) {
			const auto it = find(before.id);
			if (it == goals_.end()) {
				changed = changed || before.existed;
				continue;
			}
			const bool over = IsOver(it->goal);
			if (!before.existed && over) {
				goals_.erase(it); // a late join: when it finished cannot be told, so it is not shown
				continue;
			}
			it->session = session;
			if (it->heldUntilMs) {
				it->heldUntilMs.reset(); // adopted from an ended session
				changed = true;
			}
			if (!before.existed || YouTubeGoal::ToJson(it->goal) != before.goal) {
				changed = true;
			}
			if (!over) {
				it->endedAtMs.reset();
			} else if (!before.wasOver) {
				it->endedAtMs = now;
			}
		}
		if (!changed) {
			return;
		}
		list = ListLocked(now);
	}
	sink_(list);
}

void GoalRegistry::EndSession(uint64_t session)
{
	const std::lock_guard<std::mutex> emitLock(emitMutex_);
	json list;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		if (sessions_.erase(session) == 0) {
			return;
		}
		const int64_t now = clock_();
		bool changed = PurgeExpiredLocked(now);
		for (Entry &entry : goals_) {
			if (entry.session == session) {
				entry.session = 0;
				entry.heldUntilMs = now + kHeldGraceMs;
				changed = true;
			}
		}
		if (!changed) {
			return;
		}
		list = ListLocked(now);
	}
	sink_(list);
}

void GoalRegistry::Clear(const std::optional<OAuth::DestinationId> &dest)
{
	const std::lock_guard<std::mutex> emitLock(emitMutex_);
	json list;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		const auto matches = [&dest](const OAuth::DestinationId &d) {
			return !dest || d == *dest;
		};
		for (auto &live : sessions_) {
			if (matches(live.second.dest)) {
				live.second.retired = true;
			}
		}
		const auto first = std::remove_if(goals_.begin(), goals_.end(),
						  [&matches](const Entry &e) { return matches(e.dest); });
		if (first == goals_.end()) {
			return;
		}
		goals_.erase(first, goals_.end());
		list = ListLocked(clock_());
	}
	sink_(list);
}

json GoalRegistry::List() const
{
	const std::lock_guard<std::mutex> lock(mutex_);
	return ListLocked(clock_());
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
