#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_BRIDGE_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_BRIDGE_HPP_

#include <nlohmann/json.hpp>

#include <string>

// The voice control bridge methods. Separate from bridge.cpp, which is already very
// large; registered in its method table beside settings.getGeneral.
namespace Voice::BridgeMethods {

using json = nlohmann::json;

// settings.getVoice: the VoicePayload (settings, model statuses, CPU verdict).
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

} // namespace Voice::BridgeMethods

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_BRIDGE_HPP_
