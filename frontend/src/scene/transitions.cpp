#include "transitions.hpp"

#include "bridge.hpp"
#include "log.hpp"

#include <obs.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>

namespace Transitions {

namespace {

using json = nlohmann::json;

constexpr const char *kDefaultId = "fade_transition";
constexpr uint32_t kDefaultDurationMs = 300;
constexpr uint32_t kMaxDurationMs = 20000; // a hand-edited file can't make a switch hang
constexpr const char *kStoreFile = "transitions.json";
constexpr const char *kStoreKey = "state";

// The active transition bound to channel 0, plus the configured type/duration.
OBSSourceAutoRelease g_transition;
std::string g_typeId;
uint32_t g_durationMs = kDefaultDurationMs;

// Display name for a transition type id, falling back to the id itself.
std::string DisplayName(const std::string &id)
{
	const char *name = obs_source_get_display_name(id.c_str());
	return name ? std::string(name) : id;
}

// Whether `id` is a registered transition type.
bool IsKnownType(const std::string &id)
{
	const char *typeId = nullptr;
	for (size_t i = 0; obs_enum_transition_types(i, &typeId); i++) {
		if (typeId && id == typeId) {
			return true;
		}
	}
	return false;
}

// Size a freshly created transition to the base canvas so it composites at the
// program resolution.
void SizeToBaseCanvas(obs_source_t *transition)
{
	obs_video_info ovi = {};
	if (obs_get_video_info(&ovi)) {
		obs_transition_set_size(transition, ovi.base_width, ovi.base_height);
	}
}

void Persist()
{
	const json state = json{{"id", g_typeId}, {"durationMs", g_durationMs}};
	Bridge::WriteJsonString(kStoreFile, kStoreKey, state.dump());
}

// Load the persisted {id, durationMs} into g_typeId/g_durationMs, defaulting on
// absence or any parse failure. Never throws.
void LoadPersisted()
{
	g_typeId = kDefaultId;
	g_durationMs = kDefaultDurationMs;

	const std::string raw = Bridge::ReadJsonString(kStoreFile, kStoreKey);
	if (raw.empty()) {
		return;
	}
	try {
		const json state = json::parse(raw);
		if (state.contains("id") && state["id"].is_string()) {
			g_typeId = state["id"].get<std::string>();
		}
		if (state.contains("durationMs") && state["durationMs"].is_number_integer()) {
			g_durationMs = std::min(state["durationMs"].get<uint32_t>(), kMaxDurationMs);
		}
	} catch (const std::exception &) {
		g_typeId = kDefaultId;
		g_durationMs = kDefaultDurationMs;
	}
}

} // namespace

void Init()
{
	LoadPersisted();
	if (!IsKnownType(g_typeId)) {
		g_typeId = kDefaultId;
	}

	// obs_source_create yields one ref; g_transition adopts it. Channel 0 takes
	// its own ref via obs_set_output_source, so we never release the create-ref.
	obs_source_t *transition = obs_source_create(g_typeId.c_str(), DisplayName(g_typeId).c_str(), nullptr, nullptr);
	if (!transition) {
		HostLog("[transition] failed to create '" + g_typeId + "'");
		return;
	}
	SizeToBaseCanvas(transition);

	// Wrap whatever scene Load/CreateDefaultScene bound to channel 0, with no
	// animation, then rebind the channel to the transition.
	OBSSourceAutoRelease current = obs_get_output_source(0);
	obs_transition_set(transition, current); // null-safe
	obs_set_output_source(0, transition);

	g_transition = transition; // adopt the create-ref
	HostLog("[transition] init '" + g_typeId + "' duration=" + std::to_string(g_durationMs) + "ms");
	Persist();
}

void Shutdown()
{
	if (!g_transition) {
		return;
	}
	obs_set_output_source(0, nullptr);
	g_transition = nullptr; // release -> destroy
	g_typeId.clear();
	HostLog("[transition] shutdown");
}

obs_source_t *GetProgramScene()
{
	obs_source_t *ch0 = obs_get_output_source(0); // addref'd; may be null
	if (!ch0) {
		return nullptr;
	}
	if (obs_source_get_type(ch0) == OBS_SOURCE_TYPE_TRANSITION) {
		obs_source_t *active = obs_transition_get_active_source(ch0); // addref'd; may be null
		obs_source_release(ch0);
		return active;
	}
	return ch0; // not a transition: already the scene, addref'd
}

void SetProgramScene(obs_source_t *scene, bool animate)
{
	if (g_transition) {
		if (animate) {
			obs_transition_start(g_transition, OBS_TRANSITION_MODE_AUTO, g_durationMs, scene);
		} else {
			obs_transition_set(g_transition, scene);
		}
		return;
	}
	obs_set_output_source(0, scene);
}

std::vector<std::pair<std::string, std::string>> TypeList()
{
	std::vector<std::pair<std::string, std::string>> types;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_transition_types(i, &id); i++) {
		if (id) {
			types.emplace_back(id, DisplayName(id));
		}
	}
	return types;
}

void Current(std::string &id, std::string &name, uint32_t &durationMs)
{
	id = g_typeId;
	name = DisplayName(g_typeId);
	durationMs = g_durationMs;
}

bool SetCurrentType(const std::string &id, std::string &error)
{
	if (id == g_typeId) {
		return true; // no-op
	}
	if (!IsKnownType(id)) {
		error = "unknown transition type '" + id + "'";
		return false;
	}

	obs_source_t *transition = obs_source_create(id.c_str(), DisplayName(id).c_str(), nullptr, nullptr);
	if (!transition) {
		error = "failed to create transition '" + id + "'";
		return false;
	}
	SizeToBaseCanvas(transition);

	// Carry the live program scene across the swap with no animation.
	OBSSourceAutoRelease current = GetProgramScene();
	obs_transition_set(transition, current); // null-safe
	obs_set_output_source(0, transition);

	g_transition = transition; // adopt the create-ref; releases the old transition
	g_typeId = id;
	HostLog("[transition] type -> '" + g_typeId + "'");
	Persist();
	return true;
}

void SetDuration(uint32_t durationMs)
{
	g_durationMs = std::min(durationMs, kMaxDurationMs);
	HostLog("[transition] duration -> " + std::to_string(g_durationMs) + "ms");
	Persist();
}

void OnVideoReset()
{
	if (g_transition) {
		SizeToBaseCanvas(g_transition);
	}
}

} // namespace Transitions
