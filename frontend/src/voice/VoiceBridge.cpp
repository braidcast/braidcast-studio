#include "voice/VoiceBridge.hpp"

#include "audio/AudioEndpoints.hpp"
#include "bridge.hpp"
#include "event_names.hpp"
#include "util/json_util.hpp"
#include "voice/VoiceEngine.hpp"
#include "voice/VoiceModels.hpp"
#include "voice/VoiceP0Defaults.hpp"
#include "voice/VoiceSettings.hpp"

namespace Voice::BridgeMethods {

namespace {

// Whether viewers would hear the command cues, and what to do about it; "" when they
// would not. A monitor-only source still reaches the stream when a Desktop Audio
// capture records the monitoring device itself. P0::kCueLeakOnSharedDevice is the
// loopback probe's verdict on whether that happens at all (the spec's default until P0
// measures it), so a "no" there silences the warning everywhere at no cost.
std::string CueWarning()
{
	std::string capture;
	if (!P0::kCueLeakOnSharedDevice || !AudioEndpoints::MonitorSharesCapturedDevice(capture)) {
		return std::string();
	}
	return "Your monitoring device is also captured by " + capture +
	       ", so viewers will hear the command sounds. Pick a different monitoring device, stop capturing that "
	       "device, or turn the cue volume down to zero.";
}

// The settings block plus everything the Voice tab needs to render it, so the tab
// makes one call rather than three.
json VoicePayload()
{
	std::string cpuReason;
	const bool cpuOk = Engine().CanEnable(cpuReason);
	return json{
		{"settings", SettingsFields::ToJson(VoiceSettingsTable(), Engine().Settings())},
		{"models", Downloads().StatusJson()["models"]},
		{"cpu", {{"supported", cpuOk}, {"reason", cpuReason}}},
		{"cueWarning", CueWarning()},
	};
}

} // namespace

bool SettingsGetVoice(const json &, json &result, std::string &)
{
	result = VoicePayload();
	return true;
}

bool SettingsSetVoice(const json &params, json &result, std::string &error)
{
	const VoiceSettings &current = Engine().Settings();
	VoiceSettings next = current;
	if (!SettingsFields::ApplyPatch(VoiceSettingsTable(), params, next, error)) {
		return false;
	}
	// The field table accepts any string for `model` (the catalog is the list, and it
	// is not visible from the settings module); validate it here.
	if (!IsSelectableModel(next.model)) {
		error = "unknown speech model '" + next.model + "'";
		return false;
	}
	// Turning voice on is refused when it could not run: on a CPU without AVX2, with the
	// CPU's own reason, and before the chosen model is on disk, so the feature is never
	// switched on into a state where nothing happens. Switching the model while voice is
	// on is allowed; the engine reports not-ready until that model is downloaded.
	if (next.enabled && !current.enabled) {
		std::string cpuReason;
		if (!Engine().CanEnable(cpuReason)) {
			error = cpuReason;
			return false;
		}
		const ModelInfo *model = FindModel(next.model);
		if (!model || !ModelFilePresent(*model)) {
			error = "Download the speech model before turning voice control on.";
			return false;
		}
	}

	Engine().ApplySettings(next);
	const bool saved = next.Save();
	result = VoicePayload();
	Bridge::EmitEvent(EventNames::kSettingsVoiceChanged, result);
	return Bridge::PersistOrFail(saved, error);
}

bool VoiceState(const json &, json &result, std::string &)
{
	result = Engine().StateJson();
	return true;
}

bool ModelStatus(const json &, json &result, std::string &)
{
	result = Downloads().StatusJson();
	return true;
}

bool ModelDownload(const json &params, json &result, std::string &error)
{
	const std::string id = JsonUtil::Str(params, "id");
	if (!Downloads().Start(id, error)) {
		return false;
	}
	result = Downloads().StatusJson();
	result["started"] = true;
	return true;
}

bool ModelCancel(const json &params, json &result, std::string &error)
{
	const std::string id = JsonUtil::Str(params, "id");
	if (!Downloads().Cancel(id)) {
		error = "no download is running for '" + id + "'";
		return false;
	}
	result = Downloads().StatusJson();
	result["cancelled"] = true;
	return true;
}

bool Confirm(const json &, json &result, std::string &error)
{
	if (!Engine().ConfirmPending()) {
		error = "there is no command waiting for confirmation";
		return false;
	}
	result = Engine().StateJson();
	return true;
}

bool CancelCommand(const json &, json &result, std::string &error)
{
	if (!Engine().CancelPending()) {
		error = "there is nothing to cancel";
		return false;
	}
	result = Engine().StateJson();
	return true;
}

} // namespace Voice::BridgeMethods
