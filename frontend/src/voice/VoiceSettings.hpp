#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_SETTINGS_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_SETTINGS_HPP_

#include "settings/SettingsFields.hpp"
#include "voice/VoiceP0Defaults.hpp"

#include <string>

// Voice control preferences, persisted to voice.json beside general.json. The field
// table (VoiceSettingsTable) is the one wire <-> file <-> member mapping. `model` is
// free text here and validated against the VoiceModels catalog by settings.setVoice,
// so the catalog stays the single list of model ids.
struct VoiceSettings {
	bool enabled = false;
	std::string model = Voice::P0::kDefaultModelId;
	// Write recognized text to the session log. Only takes effect while the `voice`
	// debug component is also on; both default off.
	bool logTranscripts = false;
	// How loud the command cues are, 0 (silent) to 1. Cues are monitored, not mixed into
	// the stream, so this is a private volume.
	double cueVolume = 0.6;
	// How a dictated chat message goes out:
	//   "countdown" - shown for countdownSec, then sent unless cancelled (the default,
	//                 because a chat message cannot be unsent)
	//   "say"       - held until the user says "send"
	//   "instant"   - sent as soon as it is recognized
	std::string sendMode = "countdown";
	// How long the countdown runs, in seconds (1 to 10).
	double countdownSec = 3.0;

	void Load();
	bool Save() const;
};

const SettingsFields::Table<VoiceSettings> &VoiceSettingsTable();

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_SETTINGS_HPP_
