#include "CanvasStore.hpp"

#include "StorePaths.hpp"

#include <util/platform.h>
#include <util/util.hpp>

std::string CanvasStore::FilePath()
{
	return MultistreamBasicPath("canvases.json");
}

nlohmann::json CanvasStore::ToJson() const
{
	OBSDataAutoRelease root = obs_data_create();
	OBSDataArrayAutoRelease arr = obs_data_array_create();
	for (const CanvasDefinition &def : definitions) {
		OBSDataAutoRelease item = def.ToData();
		obs_data_array_push_back(arr, item);
	}
	obs_data_set_array(root, "canvases", arr);

	const char *js = obs_data_get_json(root);
	return js ? nlohmann::json::parse(js) : nlohmann::json::object();
}

void CanvasStore::FromJson(const nlohmann::json &j)
{
	definitions.clear();

	if (j.is_object()) {
		OBSDataAutoRelease root = obs_data_create_from_json(j.dump().c_str());
		if (root) {
			OBSDataArrayAutoRelease arr = obs_data_get_array(root, "canvases");
			const size_t count = arr ? obs_data_array_count(arr) : 0;
			for (size_t i = 0; i < count; i++) {
				OBSDataAutoRelease item = obs_data_array_item(arr, i);
				definitions.push_back(CanvasDefinition::FromData(item));
			}
		}
	}

	EnsureDefault();
}

void CanvasStore::Load()
{
	OBSDataAutoRelease root = obs_data_create_from_json_file_safe(FilePath().c_str(), "bak");
	const char *js = root ? obs_data_get_json(root) : nullptr;
	FromJson(js ? nlohmann::json::parse(js) : nlohmann::json::object());
}

void CanvasStore::Save() const
{
	OBSDataAutoRelease root = obs_data_create_from_json(ToJson().dump().c_str());
	SaveJsonAtomic(root, FilePath());
}

void CanvasStore::EnsureDefault()
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

bool CanvasStore::EnsureDefaultEncoders()
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

const CanvasDefinition &CanvasStore::Default() const
{
	for (const CanvasDefinition &def : definitions) {
		if (def.isDefault) {
			return def;
		}
	}
	// Unreachable: the constructor seeds a Default and Remove() never erases it,
	// so a Default always exists and the loop above always returns.
	return definitions.front();
}

CanvasDefinition *CanvasStore::Find(const std::string &uuid)
{
	for (CanvasDefinition &def : definitions) {
		if (def.uuid == uuid) {
			return &def;
		}
	}
	return nullptr;
}

CanvasDefinition &CanvasStore::Add(CanvasDefinition def)
{
	if (def.uuid.empty()) {
		BPtr<char> id = os_generate_uuid();
		def.uuid = id.Get();
	}
	definitions.push_back(std::move(def));
	return definitions.back();
}

void CanvasStore::Remove(const std::string &uuid)
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
