#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_SETTINGS_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_SETTINGS_HPP_

#include "settings/SettingsFields.hpp"
#include "voice/VoiceP0Defaults.hpp"

#include <string>
#include <vector>

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
	// How a command starts:
	//   "ptt"  - hold the hotkey (the default: nothing is transcribed until you do)
	//   "wake" - listen continuously, and act only on what follows the wake phrase
	std::string triggerMode = "ptt";
	// What has to be said first in "wake" mode. Matched fuzzily, so near misses still
	// work, which is also why it should be distinctive. Not "chat": streamers say that
	// to their viewers all the time.
	std::string wakePhrase = "Braidcast";
	// Speak a short confirmation after a command runs (monitoring device only).
	bool readBack = false;
	// The recognition language. "en" uses the English-only models; anything else needs
	// the multilingual model, which the Voice tab offers once a language is picked.
	std::string language = "en";

	void Load();
	bool Save() const;
};

const SettingsFields::Table<VoiceSettings> &VoiceSettingsTable();

// The recognition languages settings.setVoice accepts, as whisper language codes.
const std::vector<std::string> &VoiceLanguages();

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_SETTINGS_HPP_
