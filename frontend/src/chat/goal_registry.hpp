#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_GOAL_REGISTRY_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_GOAL_REGISTRY_HPP_

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "../oauth/provider.hpp" // OAuth::DestinationId
#include "youtube_goal.hpp"

// The creator goals the destinations' chats are showing right now (goals.list), each keyed by
// destination + the platform's goal key, so two broadcasts' goals never merge. Read-only: a
// goal is started on the platform, and this only mirrors what the chat read reports. In memory
// only -- a goal lives inside one broadcast.
//
// A goal belongs to the chat read session that reported it, and goes when that session ends:
// the chat transport stopping, the stream ending, or the free read handing over to one that
// cannot see goals. Ownership is a session token rather than the destination alone, because a
// mid-stream chat restart can start the new session before the old one has finished unwinding:
// the new session takes a goal over the moment it re-reads it, and the old session's late end
// then removes only what it still owns -- no duplicate row, and no flicker of a live goal.
//
// Mutex-guarded because each destination's chat read runs on its own worker thread while
// goals.list runs on the UI thread. Every change pushes `goals.changed` with the full
// {goals:[...]} list through AsyncTask::QueueOnUi, in mutation order (see sink_); the state
// lock is never held across that emit, and the registry does no I/O.
//
// Wire shape of one goal:
//   {id, platform, accountId, profileUuid, key, state, phase:"active"|"achieved"|"ended"|"unknown",
//    description, target, headline, current:number|null, total:number|null,
//    updatedAtMs, endedAtMs:number|null}
// `endedAtMs` is when the goal left the active phase for achieved or ended (host clock, epoch ms).
namespace Chat {

class GoalRegistry {
public:
	// Where each changed list goes. Goals() hands it to the UI as goals.changed; the self-test
	// keeps it, so its fixture goals never reach the dock.
	using Sink = std::function<void(const nlohmann::json &list)>;

	explicit GoalRegistry(Sink sink) : sink_(std::move(sink)) {}
	GoalRegistry(const GoalRegistry &) = delete;
	GoalRegistry &operator=(const GoalRegistry &) = delete;

	// A fresh token for one chat read session. Never 0.
	uint64_t BeginSession();

	// Lay `patches`, read from `dest`'s chat by `session`, onto that destination's goals. A Delete
	// removes the goal; an Update of a goal not held yet starts one from what it carries. Emits
	// only when the list changed, since the platform repeats an unchanged goal.
	void Apply(const OAuth::DestinationId &dest, uint64_t session, const std::vector<YouTubeGoal::Patch> &patches);

	// `session` ended: remove every goal of `dest` it still owns.
	void EndSession(const OAuth::DestinationId &dest, uint64_t session);

	// {goals:[...]}, in the order they were first seen.
	nlohmann::json List() const;

private:
	struct Entry {
		std::string id; // DestinationKey(dest) + "|" + key
		OAuth::DestinationId dest;
		uint64_t session = 0;
		YouTubeGoal::Goal goal;
		int64_t updatedAtMs = 0;
		std::optional<int64_t> endedAtMs;
	};

	static nlohmann::json ToJson(const Entry &entry);
	nlohmann::json ListLocked() const;

	// Every mutator takes `emitMutex_` before the state lock, releases the state lock, then
	// calls the sink still holding `emitMutex_`. With Goals()'s sink always queueing (see
	// PollRegistry::EmitChanged), snapshots reach the UI in mutation order whichever thread
	// mutated.
	const Sink sink_;
	std::mutex emitMutex_;
	mutable std::mutex mutex_;
	uint64_t nextSession_ = 1;
	std::vector<Entry> goals_; // in the order first seen
};

GoalRegistry &Goals();

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_GOAL_REGISTRY_HPP_
