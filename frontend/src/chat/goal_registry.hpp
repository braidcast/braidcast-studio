#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_GOAL_REGISTRY_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_GOAL_REGISTRY_HPP_

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "../oauth/provider.hpp" // OAuth::DestinationId
#include "util/time_util.hpp"
#include "youtube_goal.hpp"

// The creator goals the destinations' chats are showing right now (goals.list), each keyed by
// destination + the platform's goal key, so two broadcasts' goals never merge. Read-only: a
// goal is started on the platform, and this only mirrors what the chat read reports. In memory
// only -- a goal lives inside one broadcast.
//
// A goal belongs to the chat read session that reported it. When that session ends (the chat
// transport stopping, a chat restart, or the free read handing over to one that cannot see
// goals) its goals are HELD for kHeldGraceMs rather than removed: a mid-stream chat restart
// cancels the old read at once, but the new one fetches emotes and resolves /next before its
// first chat response, so the old session usually ends first. The next session of the same
// destination that reports a held goal adopts it by key -- an UPDATE merges into the held
// description and total -- so a restart shows no gap, no duplicate row and no lost fields. A
// held goal nothing adopts within the grace is removed (the UI hides it at heldUntilMs, the
// registry drops it on its next mutation). The stream stopping is not a restart: Clear removes
// that destination's goals at once and retires its sessions, so a read still unwinding cannot
// put them back.
//
// Each Apply is one chat response's mutations, judged by their net effect: a history replay
// that lands on the state already held emits nothing, `endedAtMs` is stamped only when a goal
// moves from active to over, and a goal first seen already over (a late join) is not shown at
// all -- the entity carries no finish time, only its creation (serverTimestampMs, REPLACE only),
// so how long ago it finished cannot be told.
//
// Mutex-guarded because each destination's chat read runs on its own worker thread while
// goals.list runs on the UI thread. Every change pushes `goals.changed` with the full
// {goals:[...]} list through AsyncTask::QueueOnUi, in mutation order (see sink_); the state
// lock is never held across that emit, and the registry does no I/O.
//
// Wire shape of one goal:
//   {id, platform, accountId, profileUuid, key, state, phase:"active"|"achieved"|"ended"|"unknown",
//    description, target, headline, current:number|null, total:number|null,
//    endedAtMs:number|null, heldUntilMs:number|null}
// `endedAtMs` is when this registry saw the goal leave the active phase; `heldUntilMs` is set
// while its session has ended and no other has adopted it (both host clock, epoch ms).
namespace Chat {

class GoalRegistry {
public:
	// Where each changed list goes. Goals() hands it to the UI as goals.changed; the self-test
	// keeps it, so its fixture goals never reach the dock.
	using Sink = std::function<void(const nlohmann::json &list)>;
	// Epoch ms. Injected so the self-test can step past the grace without sleeping.
	using Clock = std::function<int64_t()>;

	// How long an ended session's goals wait for the next session to adopt them. Covers a slow
	// but normal restart: the emote fetches (5 s each), /next (15 s request timeout) and the
	// first chat response. The stream stopping clears at once instead (Clear), so this never
	// keeps a stopped stream's goal on screen.
	static constexpr int64_t kHeldGraceMs = 30'000;

	explicit GoalRegistry(Sink sink, Clock clock = TimeUtil::NowMs)
		: sink_(std::move(sink)),
		  clock_(std::move(clock))
	{
	}
	GoalRegistry(const GoalRegistry &) = delete;
	GoalRegistry &operator=(const GoalRegistry &) = delete;

	// A fresh token for one chat read session of `dest`. Never 0.
	uint64_t BeginSession(const OAuth::DestinationId &dest);

	// Lay one response's `patches`, read by `session`, onto its destination's goals, and emit if
	// the shown list changed. A Delete removes the goal; an Update of a goal not held yet starts
	// one from what it carries. Ignored for a session that ended or was cleared.
	void Apply(uint64_t session, const std::vector<YouTubeGoal::Patch> &patches);

	// `session` ended: hold every goal it still owns for kHeldGraceMs.
	void EndSession(uint64_t session);

	// The stream stopped for `dest` (every destination when empty): remove its goals now, and
	// retire its live sessions so their late mutations are ignored.
	void Clear(const std::optional<OAuth::DestinationId> &dest);

	// {goals:[...]}, in the order they were first seen, without held goals past their grace.
	nlohmann::json List() const;

private:
	struct Entry {
		std::string id; // DestinationKey(dest) + "|" + key
		OAuth::DestinationId dest;
		uint64_t session = 0; // 0 while held
		YouTubeGoal::Goal goal;
		std::optional<int64_t> endedAtMs;
		std::optional<int64_t> heldUntilMs;
	};
	struct Session {
		OAuth::DestinationId dest;
		bool retired = false; // Clear ran while it was live
	};

	static nlohmann::json ToJson(const Entry &entry);
	nlohmann::json ListLocked(int64_t now) const;
	bool PurgeExpiredLocked(int64_t now);

	// Every mutator takes `emitMutex_` before the state lock, releases the state lock, then
	// calls the sink still holding `emitMutex_`. With Goals()'s sink always queueing (see
	// PollRegistry::EmitChanged), snapshots reach the UI in mutation order whichever thread
	// mutated.
	const Sink sink_;
	const Clock clock_;
	std::mutex emitMutex_;
	mutable std::mutex mutex_;
	uint64_t nextSession_ = 1;
	std::unordered_map<uint64_t, Session> sessions_; // live sessions only
	std::vector<Entry> goals_;                       // in the order first seen
};

GoalRegistry &Goals();

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_GOAL_REGISTRY_HPP_
