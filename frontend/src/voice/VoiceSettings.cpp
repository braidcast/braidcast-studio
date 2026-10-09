#include "voice/VoiceSettings.hpp"

#include "multistream/StorePaths.hpp"

#include <obs.hpp>

#include <iterator>

namespace {

constexpr const char *kVoiceFile = "voice.json";
constexpr size_t kModelIdMaxLen = 64;
constexpr const char *kSendModes[] = {"countdown", "say", "instant"};
constexpr const char *kTriggerModes[] = {"ptt", "wake"};

// whisper's own language set is much larger; these are the ones worth offering for
// short spoken commands, and each is a code whisper_lang_id() knows. A language not on
// this list is refused rather than passed through to whisper. The Voice tab's list must
// match (VoiceTab.svelte, LANGUAGES).
constexpr const char *kLanguages[] = {"en", "de", "es", "fr", "it", "ja", "ko", "nl",
				      "pl", "pt", "ru", "sv", "tr", "uk", "zh"};

constexpr size_t kWakePhraseMaxLen = 64;

constexpr SettingsFields::BoolField<VoiceSettings> kBools[] = {
	{"enabled", "enabled", &VoiceSettings::enabled},
	{"logTranscripts", "log_transcripts", &VoiceSettings::logTranscripts},
	{"readBack", "read_back", &VoiceSettings::readBack},
};

constexpr SettingsFields::StringField<VoiceSettings> kStrings[] = {
	{"model", "model", &VoiceSettings::model, nullptr, 0, kModelIdMaxLen},
	{"sendMode", "send_mode", &VoiceSettings::sendMode, kSendModes, std::size(kSendModes), 0},
	{"triggerMode", "trigger_mode", &VoiceSettings::triggerMode, kTriggerModes, std::size(kTriggerModes), 0},
	{"wakePhrase", "wake_phrase", &VoiceSettings::wakePhrase, nullptr, 0, kWakePhraseMaxLen},
	{"language", "language", &VoiceSettings::language, kLanguages, std::size(kLanguages), 0},
};

constexpr SettingsFields::DoubleField<VoiceSettings> kDoubles[] = {
	{"cueVolume", "cue_volume", &VoiceSettings::cueVolume, 0.0, 1.0},
	{"countdownSec", "countdown_sec", &VoiceSettings::countdownSec, 1.0, 10.0},
};

constexpr SettingsFields::Table<VoiceSettings> kTable = {
	kBools, std::size(kBools), kStrings, std::size(kStrings), kDoubles, std::size(kDoubles),
};

} // namespace

const SettingsFields::Table<VoiceSettings> &VoiceSettingsTable()
{
	return kTable;
}

const std::vector<std::string> &VoiceLanguages()
{
	static const std::vector<std::string> languages(std::begin(kLanguages), std::end(kLanguages));
	return languages;
}

void VoiceSettings::Load()
{
	// No file yet keeps the struct defaults, and so does one that is there but unusable,
	// which LoadStoreData keeps aside (the same envelope general.json loads through).
	OBSDataAutoRelease root = LoadStoreData(MultistreamBasicPath(kVoiceFile), nullptr, "[voice]");
	if (!root) {
		return;
	}
	SettingsFields::Load(kTable, root, *this);
}

bool VoiceSettings::Save() const
{
	OBSDataAutoRelease root = obs_data_create();
	SettingsFields::Save(kTable, root, *this);
	const std::string path = MultistreamBasicPath(kVoiceFile);
	return ReportSaveResult(SaveJsonAtomic(root, path), path);
}
