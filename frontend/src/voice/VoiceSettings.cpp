#include "voice/VoiceSettings.hpp"

#include "multistream/StorePaths.hpp"

#include <obs.hpp>

#include <iterator>

namespace {

constexpr const char *kVoiceFile = "voice.json";
constexpr size_t kModelIdMaxLen = 64;

constexpr SettingsFields::BoolField<VoiceSettings> kBools[] = {
	{"enabled", "enabled", &VoiceSettings::enabled},
	{"logTranscripts", "log_transcripts", &VoiceSettings::logTranscripts},
};

constexpr SettingsFields::StringField<VoiceSettings> kStrings[] = {
	{"model", "model", &VoiceSettings::model, nullptr, 0, kModelIdMaxLen},
};

constexpr SettingsFields::DoubleField<VoiceSettings> kDoubles[] = {
	{"cueVolume", "cue_volume", &VoiceSettings::cueVolume, 0.0, 1.0},
};

constexpr SettingsFields::Table<VoiceSettings> kTable = {
	kBools, std::size(kBools), kStrings, std::size(kStrings), kDoubles, std::size(kDoubles),
};

} // namespace

const SettingsFields::Table<VoiceSettings> &VoiceSettingsTable()
{
	return kTable;
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
