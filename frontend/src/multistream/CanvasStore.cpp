#include "CanvasStore.hpp"

#include "StorePaths.hpp"

#include "uuid_util.hpp"

std::string CanvasStore::FilePath()
{
	return MultistreamBasicPath("canvases.json");
}

nlohmann::json CanvasStore::ToJson() const
{
	OBSDataArrayAutoRelease arr = obs_data_array_create();
	for (const CanvasDefinition &def : definitions) {
		OBSDataAutoRelease item = def.ToData();
		obs_data_array_push_back(arr, item);
	}
	return StoreJsonFromArray("canvases", arr);
}

void CanvasStore::FromJson(const nlohmann::json &j)
{
	definitions.clear();

	OBSDataArrayAutoRelease arr = StoreArrayFromJson(j, "canvases");
	const size_t count = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease item = obs_data_array_item(arr, i);
		definitions.push_back(CanvasDefinition::FromData(item));
	}

	EnsureDefault();
}

void CanvasStore::Load()
{
	FromJson(LoadStoreJson(FilePath()));
}

bool CanvasStore::Save() const
{
	return SaveStoreJson(ToJson(), FilePath());
}

void CanvasStore::EnsureDefault()
{
	for (CanvasDefinition &def : definitions) {
		if (def.isDefault) {
			// Base canvas name is immutable and not user-editable; force it so
			// collections persisted under the old "Default Canvas" label flip to "Main".
			def.name = "Main";
			return;
		}
	}
	CanvasDefinition def;
	def.isDefault = true;
	def.name = "Main";
	def.uuid = UuidUtil::New();
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
		def.uuid = UuidUtil::New();
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

void CanvasStore::Reorder(const std::vector<std::string> &order)
{
	ReorderByUuid(definitions, order);
}
