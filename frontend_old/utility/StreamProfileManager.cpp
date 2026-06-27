#include "StreamProfileManager.hpp"

#include <OBSApp.hpp>

#include <util/platform.h>
#include <util/util.hpp>

#include <filesystem>

static constexpr std::string_view StreamsRelPath = "/obs-multistream/basic/streams.json";

std::string StreamProfileManager::FilePath()
{
	std::filesystem::path p = std::filesystem::u8path(App()->userProfilesLocation.u8string());
	p += std::filesystem::u8path(std::string(StreamsRelPath));
	return p.u8string();
}

void StreamProfileManager::Load()
{
	profiles.clear();

	OBSDataAutoRelease root = obs_data_create_from_json_file_safe(FilePath().c_str(), "bak");
	if (root) {
		OBSDataArrayAutoRelease arr = obs_data_get_array(root, "profiles");
		const size_t count = arr ? obs_data_array_count(arr) : 0;
		for (size_t i = 0; i < count; i++) {
			OBSDataAutoRelease item = obs_data_array_item(arr, i);
			profiles.push_back(StreamProfile::FromData(item));
		}
	}

	/* Guarantee exactly one primary if any profiles exist. */
	if (!profiles.empty()) {
		bool hasPrimary = false;
		for (const StreamProfile &p : profiles) {
			if (p.isPrimary) {
				hasPrimary = true;
				break;
			}
		}
		if (!hasPrimary) {
			profiles.front().isPrimary = true;
		}
	}
}

void StreamProfileManager::Save() const
{
	OBSDataAutoRelease root = obs_data_create();
	OBSDataArrayAutoRelease arr = obs_data_array_create();
	for (const StreamProfile &p : profiles) {
		OBSDataAutoRelease item = p.ToData();
		obs_data_array_push_back(arr, item);
	}
	obs_data_set_array(root, "profiles", arr);

	std::filesystem::path dir = std::filesystem::u8path(FilePath()).parent_path();
	os_mkdirs(dir.u8string().c_str());

	obs_data_save_json_pretty_safe(root, FilePath().c_str(), "tmp", "bak");
}

StreamProfile *StreamProfileManager::Primary()
{
	for (StreamProfile &p : profiles) {
		if (p.isPrimary) {
			return &p;
		}
	}
	return nullptr;
}

StreamProfile *StreamProfileManager::Find(const std::string &uuid)
{
	for (StreamProfile &p : profiles) {
		if (p.uuid == uuid) {
			return &p;
		}
	}
	return nullptr;
}

StreamProfile &StreamProfileManager::Add(StreamProfile p)
{
	if (p.uuid.empty()) {
		BPtr<char> id = os_generate_uuid();
		p.uuid = id.Get();
	}
	if (profiles.empty()) {
		p.isPrimary = true; // first profile is always primary
	}
	profiles.push_back(std::move(p));
	return profiles.back();
}

void StreamProfileManager::Remove(const std::string &uuid)
{
	for (auto it = profiles.begin(); it != profiles.end(); ++it) {
		if (it->uuid == uuid) {
			bool wasPrimary = it->isPrimary;
			profiles.erase(it);
			if (wasPrimary && !profiles.empty()) {
				profiles.front().isPrimary = true;
			}
			return;
		}
	}
}
