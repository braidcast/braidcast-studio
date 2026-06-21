#include "CanvasManager.hpp"

#include <OBSApp.hpp>

#include <util/platform.h>
#include <util/util.hpp>

#include <filesystem>

static constexpr std::string_view CanvasesRelPath = "/obs-multistream/basic/canvases.json";

std::string CanvasManager::FilePath()
{
	std::filesystem::path p = std::filesystem::u8path(App()->userProfilesLocation.u8string());
	p += std::filesystem::u8path(std::string(CanvasesRelPath));
	return p.u8string();
}

void CanvasManager::Load()
{
	definitions.clear();

	OBSDataAutoRelease root = obs_data_create_from_json_file_safe(FilePath().c_str(), "bak");
	if (root) {
		OBSDataArrayAutoRelease arr = obs_data_get_array(root, "canvases");
		const size_t count = arr ? obs_data_array_count(arr) : 0;
		for (size_t i = 0; i < count; i++) {
			OBSDataAutoRelease item = obs_data_array_item(arr, i);
			definitions.push_back(CanvasDefinition::FromData(item));
		}
	}

	EnsureDefault();
}

void CanvasManager::Save() const
{
	OBSDataAutoRelease root = obs_data_create();
	OBSDataArrayAutoRelease arr = obs_data_array_create();
	for (const CanvasDefinition &def : definitions) {
		OBSDataAutoRelease item = def.ToData();
		obs_data_array_push_back(arr, item);
	}
	obs_data_set_array(root, "canvases", arr);

	/* Ensure the parent dir exists (it does once a profile is created, but be safe). */
	std::filesystem::path dir = std::filesystem::u8path(FilePath()).parent_path();
	os_mkdirs(dir.u8string().c_str());

	obs_data_save_json_pretty_safe(root, FilePath().c_str(), "tmp", "bak");
}

void CanvasManager::EnsureDefault()
{
	for (const CanvasDefinition &def : definitions) {
		if (def.isDefault) {
			return;
		}
	}
	CanvasDefinition def;
	def.isDefault = true;
	def.name = "Default Canvas";
	def.uuid = []() {
		BPtr<char> id = os_generate_uuid();
		return std::string(id.Get());
	}();
	definitions.insert(definitions.begin(), std::move(def));
}

bool CanvasManager::EnsureDefaultEncoders()
{
	CanvasDefinition *def = nullptr;
	for (CanvasDefinition &d : definitions) {
		if (d.isDefault) {
			def = &d;
			break;
		}
	}
	if (!def) {
		return false;
	}

	bool changed = false;
	if (def->video.id.empty()) {
		OBSDataAutoRelease s = obs_encoder_defaults("obs_x264");
		if (s) {
			def->video.id = "obs_x264";
			obs_data_set_int(s, "bitrate", 6000);
			obs_data_set_string(s, "rate_control", "CBR");
			def->video.settings = std::move(s);
			changed = true;
		} else {
			blog(LOG_WARNING, "EnsureDefaultEncoders: video encoder 'obs_x264' is not registered; "
					  "leaving default canvas video encoder unset");
		}
	}
	if (def->audio.id.empty()) {
		OBSDataAutoRelease s = obs_encoder_defaults("ffmpeg_aac");
		if (s) {
			def->audio.id = "ffmpeg_aac";
			obs_data_set_int(s, "bitrate", 160);
			def->audio.settings = std::move(s);
			changed = true;
		} else {
			blog(LOG_WARNING, "EnsureDefaultEncoders: audio encoder 'ffmpeg_aac' is not registered; "
					  "leaving default canvas audio encoder unset");
		}
	}
	return changed;
}

const CanvasDefinition &CanvasManager::Default() const
{
	for (const CanvasDefinition &def : definitions) {
		if (def.isDefault) {
			return def;
		}
	}
	/* Unreachable: the constructor seeds a Default and Remove() never erases it,
	 * so a Default always exists and the loop above always returns. */
	return definitions.front();
}

CanvasDefinition *CanvasManager::Find(const std::string &uuid)
{
	for (CanvasDefinition &def : definitions) {
		if (def.uuid == uuid) {
			return &def;
		}
	}
	return nullptr;
}

CanvasDefinition &CanvasManager::Add(CanvasDefinition def)
{
	if (def.uuid.empty()) {
		BPtr<char> id = os_generate_uuid();
		def.uuid = id.Get();
	}
	definitions.push_back(std::move(def));
	return definitions.back();
}

void CanvasManager::Remove(const std::string &uuid)
{
	for (auto it = definitions.begin(); it != definitions.end(); ++it) {
		if (it->uuid == uuid) {
			if (it->isDefault) {
				return; // Default is not removable
			}
			definitions.erase(it);
			return;
		}
	}
}
