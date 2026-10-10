#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_BRIDGE_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_BRIDGE_HPP_

#include <nlohmann/json.hpp>

#include <string>

// The voice control bridge methods. Separate from bridge.cpp, which is already very
// large; registered in its method table beside settings.getGeneral.
namespace Voice::BridgeMethods {

using json = nlohmann::json;

// settings.getVoice: the VoicePayload (settings, model statuses, CPU verdict, and the
// cueWarning when viewers would hear the command cues).
bool SettingsGetVoice(const json &params, json &result, std::string &error);
// settings.setVoice: any subset of the settings; answers the full VoicePayload.
bool SettingsSetVoice(const json &params, json &result, std::string &error);
// voice.state: the engine's VoiceState, the same payload the voice.state event carries.
bool VoiceState(const json &params, json &result, std::string &error);
// voice.model.status: {models: [VoiceModelStatus]}.
bool ModelStatus(const json &params, json &result, std::string &error);
// voice.model.download {id}: {started, models}; progress arrives as voice.model.status.
bool ModelDownload(const json &params, json &result, std::string &error);
// voice.model.cancel {id}: {cancelled, models}; refused when no download is running.
bool ModelCancel(const json &params, json &result, std::string &error);
// voice.confirm {id}: runs the pending command, as a spoken yes would; answers
// voice.state. `id` is required and must be the pending action's (voice.state's
// pending.id): refused when nothing is pending, when no id is given, or when another
// action is pending by now.
bool Confirm(const json &params, json &result, std::string &error);
// voice.cancel {id?}: drops the pending command or the segment in flight, as the cancel
// key does; answers voice.state. Refused when there is nothing to cancel, or when `id`
// is given and is not the pending action's. Without an id it drops whatever is in
// flight, which fails safe: nothing is sent or run.
bool CancelCommand(const json &params, json &result, std::string &error);

} // namespace Voice::BridgeMethods

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_BRIDGE_HPP_
