#include "StreamState.hpp"

#include <optional>
#include <utility>

using json = nlohmann::json;

json StreamStateJson(const std::vector<MultistreamEngine::OutputStats> &outputs, bool anyLive, int64_t nowMs)
{
	json destinations = json::array();
	std::optional<int64_t> startedAt;
	for (const MultistreamEngine::OutputStats &s : outputs) {
		if (!MultistreamEngine::IsActiveState(s.state)) {
			continue; // idle or dead: not somewhere this broadcast is going out
		}
		std::optional<int64_t> destStartedAt;
		if (s.started) {
			destStartedAt = nowMs - static_cast<int64_t>(s.uptimeMs);
			if (!startedAt || *destStartedAt < *startedAt) {
				startedAt = destStartedAt; // the broadcast began when its FIRST output did
			}
		}
		destinations.push_back(json{
			{"bindingUuid", s.bindingUuid},
			{"platform", s.platformKey},
			// The raw label, not DisplayName(): a widget draws its own platform mark
			// and would otherwise print the platform twice. Null when the profile
			// carries no label, so a widget prints the platform alone rather than a
			// blank line.
			{"name", s.profileName.empty() ? json(nullptr) : json(s.profileName)},
			{"canvasName", s.canvasName},
			{"state", MultistreamEngine::StateName(s.state)},
			{"startedAt", destStartedAt ? json(*destStartedAt) : json(nullptr)},
		});
	}
	return json{
		{"active", anyLive},
		{"startedAt", startedAt ? json(*startedAt) : json(nullptr)},
		{"destinations", std::move(destinations)},
	};
}
