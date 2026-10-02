#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_GOAL_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_GOAL_HPP_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// The pure half of YouTube creator goals (the Super Chat / gift goal a creator starts in
// YouTube Studio). Braidcast only reads them: the Data API has no goal type, so the anonymous
// InnerTube chat read is the one place they appear. No I/O, so the self-test drives it offline.
//
// A goal is a `creatorGoalEntity` carried by an entity batch update, keyed by the entity's
// `key`. The first mutation of a goal is a REPLACE carrying every field; the ones that follow
// as gifts arrive are UPDATEs carrying only the fields that changed (the count, the state and
// the headline). So an UPDATE merges into the goal held under that key and never replaces it,
// while a REPLACE starts the goal over -- YouTube reuses one key per broadcast, so a second
// goal started later in the same stream arrives as a REPLACE of the first.
//
// States seen in replay captures (2026-10-02): CREATOR_GOAL_STATE_ACTIVE ("Goal in progress"),
// CREATOR_GOAL_STATE_COMPLETE ("Goal achieved") and CREATOR_GOAL_STATE_NOT_ACHIEVED ("Goal
// ended"). Any other state reads as Phase "unknown" and is kept, never dropped.
namespace YouTubeGoal {

enum class MutationKind { Replace, Update, Delete };

// One mutation of a goal entity: the fields it carried and nothing else. A field the mutation
// did not carry -- or carried in a form that does not read -- is nullopt, so a merge leaves the
// held value alone. Counts arrive as digit strings and are read with JsonUtil::Count.
struct Patch {
	std::string key;
	MutationKind kind = MutationKind::Update;
	std::optional<std::string> state;       // creatorGoalState, exactly as sent
	std::optional<int64_t> current;         // currentGoalCount
	std::optional<int64_t> total;           // totalGoalCount
	std::optional<std::string> description; // goalDescription.content
	std::optional<std::string> target;      // goalTargetText.content, e.g. "50 gifts"
	std::optional<std::string> headline;    // goalHeadlineText.content, e.g. "Goal in progress"
};

// A goal as merged from its mutations so far.
struct Goal {
	std::string key;
	std::string state;
	std::optional<int64_t> current;
	std::optional<int64_t> total;
	std::string description;
	std::string target;
	std::string headline;
};

// The goal mutations in one entityBatchUpdate -- the `entityUpdateCommand.entityBatchUpdate` of
// a chat action, or a response's `frameworkUpdates.entityBatchUpdate` -- in order. A
// creatorGoalEntity payload yields its patch, keyed by its own `key` (the mutation's
// `entityKey` when that is missing); a DELETE yields a Delete patch for its `entityKey` whatever
// its payload, since a delete may carry none. Mutations of other entity types are skipped, and
// their payload names are appended to `otherPayloads` when it is given, for the diagnostic log.
// Never throws on any shape.
std::vector<Patch> ReadBatch(const nlohmann::json &entityBatchUpdate,
			     std::vector<std::string> *otherPayloads = nullptr);

// Lay `patch` onto `goal`: a Replace clears the goal first, an Update overwrites only the fields
// the patch carries. A Delete is the caller's to act on (it removes the goal) and changes
// nothing here.
void Apply(Goal &goal, const Patch &patch);

// The display phase of a creatorGoalState: "active", "achieved", "ended", or "unknown" for a
// state not seen yet.
const char *Phase(const std::string &state);

// {key, state, phase, description, target, headline, current, total}; a count that never read is
// null. The neutral part of the goals.* wire shape (GoalRegistry adds who and when).
nlohmann::json ToJson(const Goal &goal);

} // namespace YouTubeGoal

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_YOUTUBE_GOAL_HPP_
