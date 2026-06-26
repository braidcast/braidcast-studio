#include "scene_collections.hpp"

#include "log.hpp"
#include "multistream/StorePaths.hpp"

#include <obs.h>
#include <obs.hpp>
#include <util/platform.h>
#include <util/util.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace {

std::string NewUuid()
{
	BPtr<char> id = os_generate_uuid();
	return std::string(id.Get());
}

// Lowercase, collapse non-alphanumeric runs to single hyphens, trim leading/
// trailing hyphens. Empty result falls back to "collection".
std::string Slugify(const std::string &name)
{
	std::string slug;
	slug.reserve(name.size());
	bool lastHyphen = false;
	for (unsigned char c : name) {
		if (std::isalnum(c)) {
			slug.push_back(char(std::tolower(c)));
			lastHyphen = false;
		} else if (!lastHyphen) {
			slug.push_back('-');
			lastHyphen = true;
		}
	}
	while (!slug.empty() && slug.front() == '-') {
		slug.erase(slug.begin());
	}
	while (!slug.empty() && slug.back() == '-') {
		slug.pop_back();
	}
	return slug.empty() ? std::string("collection") : slug;
}

} // namespace

std::string SceneCollections::IndexPath()
{
	return MultistreamBasicPath("scene_collections.json");
}

const SceneCollectionRecord *SceneCollections::Active() const
{
	for (const SceneCollectionRecord &c : collections_) {
		if (c.id == activeId_) {
			return &c;
		}
	}
	return collections_.empty() ? nullptr : &collections_.front();
}

std::string SceneCollections::ActiveScenePath() const
{
	const SceneCollectionRecord *active = Active();
	return MultistreamBasicPath(active ? active->sceneFile.c_str() : "scene_collection.json");
}

std::string SceneCollections::UniqueSceneFileForName(const std::string &name) const
{
	const std::string slug = Slugify(name);
	for (int suffix = 0;; ++suffix) {
		std::string rel = "scenes/" + slug + (suffix ? "-" + std::to_string(suffix + 1) : "") + ".json";

		bool clashes = false;
		for (const SceneCollectionRecord &c : collections_) {
			if (c.sceneFile == rel) {
				clashes = true;
				break;
			}
		}
		if (!clashes && os_file_exists(MultistreamBasicPath(rel.c_str()).c_str())) {
			clashes = true;
		}
		if (!clashes) {
			return rel;
		}
	}
}

void SceneCollections::Load()
{
	collections_.clear();
	activeId_.clear();

	OBSDataAutoRelease root = obs_data_create_from_json_file_safe(IndexPath().c_str(), "bak");
	if (!root) {
		return;
	}

	OBSDataArrayAutoRelease arr = obs_data_get_array(root, "collections");
	const size_t count = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease item = obs_data_array_item(arr, i);
		SceneCollectionRecord rec;
		rec.id = obs_data_get_string(item, "id");
		rec.name = obs_data_get_string(item, "name");
		rec.sceneFile = obs_data_get_string(item, "sceneFile");
		if (!rec.id.empty() && !rec.sceneFile.empty()) {
			collections_.push_back(std::move(rec));
		}
	}

	activeId_ = obs_data_get_string(root, "active");
	// Repair a dangling/absent active pointer so Active() is always meaningful.
	if (!collections_.empty()) {
		bool found = false;
		for (const SceneCollectionRecord &c : collections_) {
			if (c.id == activeId_) {
				found = true;
				break;
			}
		}
		if (!found) {
			activeId_ = collections_.front().id;
		}
	}
}

void SceneCollections::Save() const
{
	OBSDataAutoRelease root = obs_data_create();
	OBSDataArrayAutoRelease arr = obs_data_array_create();
	for (const SceneCollectionRecord &c : collections_) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "id", c.id.c_str());
		obs_data_set_string(item, "name", c.name.c_str());
		obs_data_set_string(item, "sceneFile", c.sceneFile.c_str());
		obs_data_array_push_back(arr, item);
	}
	obs_data_set_array(root, "collections", arr);
	obs_data_set_string(root, "active", activeId_.c_str());

	const std::string path = IndexPath();
	std::filesystem::path dir = std::filesystem::u8path(path).parent_path();
	os_mkdirs(dir.u8string().c_str());

	if (!obs_data_save_json_pretty_safe(root, path.c_str(), "tmp", "bak")) {
		HostLog("[scene] failed to save scene-collection index to " + path);
	}
}

const SceneCollectionRecord &SceneCollections::SeedExisting(const std::string &name, const std::string &existingRelFile)
{
	SceneCollectionRecord rec;
	rec.id = NewUuid();
	rec.name = name;
	rec.sceneFile = existingRelFile;
	collections_.push_back(std::move(rec));
	activeId_ = collections_.back().id;
	Save();
	return collections_.back();
}

const SceneCollectionRecord &SceneCollections::Create(const std::string &name)
{
	SceneCollectionRecord rec;
	rec.id = NewUuid();
	rec.name = name;
	rec.sceneFile = UniqueSceneFileForName(name);
	collections_.push_back(std::move(rec));
	Save();
	return collections_.back();
}

bool SceneCollections::Rename(const std::string &id, const std::string &name)
{
	for (SceneCollectionRecord &c : collections_) {
		if (c.id == id) {
			c.name = name;
			Save();
			return true;
		}
	}
	return false;
}

bool SceneCollections::Remove(const std::string &id, std::string &error)
{
	auto it = std::find_if(collections_.begin(), collections_.end(),
			       [&](const SceneCollectionRecord &c) { return c.id == id; });
	if (it == collections_.end()) {
		error = "no scene collection with id '" + id + "'";
		return false;
	}
	if (collections_.size() == 1) {
		error = "cannot remove the last remaining scene collection";
		return false;
	}
	if (id == activeId_) {
		error = "cannot remove the active scene collection";
		return false;
	}

	// Delete the per-collection scene file (best-effort; an absent file is fine).
	std::error_code ec;
	std::filesystem::remove(std::filesystem::u8path(MultistreamBasicPath(it->sceneFile.c_str())), ec);

	collections_.erase(it);
	Save();
	return true;
}
