#include "youtube_goal.hpp"

#include <utility>

#include "util/json_util.hpp"

namespace YouTubeGoal {

using json = nlohmann::json;

namespace {

constexpr const char *kGoalPayload = "creatorGoalEntity";

struct PhaseName {
	const char *state;
	const char *phase;
};

// creatorGoalState -> display phase. Adding a state YouTube turns out to send is one row.
const PhaseName kPhases[] = {
	{"CREATOR_GOAL_STATE_ACTIVE", "active"},
	{"CREATOR_GOAL_STATE_COMPLETE", "achieved"},
	{"CREATOR_GOAL_STATE_NOT_ACHIEVED", "ended"},
};

// A text field of the entity: an attributed string `{content, styleRuns}`. nullopt when the
// field is absent or carries no string content, so a merge keeps what it held.
std::optional<std::string> Content(const json &entity, const char *key)
{
	const json &node = JsonUtil::Obj(entity, key);
	const json &content = JsonUtil::Obj(node, "content");
	if (!content.is_string()) {
		return std::nullopt;
	}
	return content.get<std::string>();
}

std::optional<std::string> String(const json &entity, const char *key)
{
	const json &value = JsonUtil::Obj(entity, key);
	if (!value.is_string()) {
		return std::nullopt;
	}
	return value.get<std::string>();
}

MutationKind KindOf(const std::string &type)
{
	if (type == "ENTITY_MUTATION_TYPE_REPLACE") {
		return MutationKind::Replace;
	}
	if (type == "ENTITY_MUTATION_TYPE_DELETE") {
		return MutationKind::Delete;
	}
	// UPDATE, and any type not seen yet: merging is the reading that cannot lose a field.
	return MutationKind::Update;
}

json CountJson(const std::optional<int64_t> &count)
{
	return count ? json(*count) : json(nullptr);
}

} // namespace

std::vector<Patch> ReadBatch(const json &entityBatchUpdate, std::vector<std::string> *otherPayloads)
{
	std::vector<Patch> out;
	const json &mutations = JsonUtil::Obj(entityBatchUpdate, "mutations");
	if (!mutations.is_array()) {
		return out;
	}
	for (const json &mutation : mutations) {
		const MutationKind kind = KindOf(JsonUtil::Str(mutation, "type"));
		const std::string entityKey = JsonUtil::Str(mutation, "entityKey");
		const json &payload = JsonUtil::Obj(mutation, "payload");
		const json &entity = JsonUtil::Obj(payload, kGoalPayload);

		if (!entity.is_object()) {
			if (kind == MutationKind::Delete && !entityKey.empty()) {
				Patch patch;
				patch.key = entityKey;
				patch.kind = kind;
				out.push_back(std::move(patch));
			} else if (otherPayloads && payload.is_object()) {
				for (const auto &item : payload.items()) {
					otherPayloads->push_back(item.key());
				}
			}
			continue;
		}

		Patch patch;
		patch.key = JsonUtil::Str(entity, "key");
		if (patch.key.empty()) {
			patch.key = entityKey;
		}
		if (patch.key.empty()) {
			continue;
		}
		patch.kind = kind;
		patch.state = String(entity, "creatorGoalState");
		patch.current = JsonUtil::Count(entity, "currentGoalCount");
		patch.total = JsonUtil::Count(entity, "totalGoalCount");
		patch.description = Content(entity, "goalDescription");
		patch.target = Content(entity, "goalTargetText");
		patch.headline = Content(entity, "goalHeadlineText");
		out.push_back(std::move(patch));
	}
	return out;
}

void Apply(Goal &goal, const Patch &patch)
{
	if (patch.kind == MutationKind::Delete) {
		return;
	}
	if (patch.kind == MutationKind::Replace) {
		goal = Goal();
	}
	goal.key = patch.key;
	if (patch.state) {
		goal.state = *patch.state;
	}
	if (patch.current) {
		goal.current = patch.current;
	}
	if (patch.total) {
		goal.total = patch.total;
	}
	if (patch.description) {
		goal.description = *patch.description;
	}
	if (patch.target) {
		goal.target = *patch.target;
	}
	if (patch.headline) {
		goal.headline = *patch.headline;
	}
}

const char *Phase(const std::string &state)
{
	for (const PhaseName &entry : kPhases) {
		if (state == entry.state) {
			return entry.phase;
		}
	}
	return "unknown";
}

json ToJson(const Goal &goal)
{
	return json{{"key", goal.key},
		    {"state", goal.state},
		    {"phase", Phase(goal.state)},
		    {"description", goal.description},
		    {"target", goal.target},
		    {"headline", goal.headline},
		    {"current", CountJson(goal.current)},
		    {"total", CountJson(goal.total)}};
}

} // namespace YouTubeGoal
