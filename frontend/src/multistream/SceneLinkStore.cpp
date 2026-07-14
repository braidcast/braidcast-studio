#include "SceneLinkStore.hpp"

#include "StorePaths.hpp"
#include "../obs_bootstrap.hpp"
#include "scene/scene_collections.hpp"

#include <util/platform.h>
#include <util/util.hpp>

nlohmann::json SceneLinkStore::ToJson() const
{
	OBSDataAutoRelease root = obs_data_create();
	OBSDataArrayAutoRelease arr = links.ToDataArray();
	obs_data_set_array(root, "canvas_scene_links", arr);

	const char *js = obs_data_get_json(root);
	return js ? nlohmann::json::parse(js) : nlohmann::json::object();
}

void SceneLinkStore::FromJson(const nlohmann::json &j)
{
	links = CanvasSceneLink{};

	if (j.is_object()) {
		OBSDataAutoRelease root = obs_data_create_from_json(j.dump().c_str());
		if (root) {
			OBSDataArrayAutoRelease arr = obs_data_get_array(root, "canvas_scene_links");
			links = CanvasSceneLink::FromDataArray(arr);
		}
	}
}

void SceneLinkStore::Load()
{
	Load(ObsBootstrap::SceneCollections().ActiveSceneLinksPath());
}

void SceneLinkStore::Load(const std::string &path)
{
	OBSDataAutoRelease root = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	const char *js = root ? obs_data_get_json(root) : nullptr;
	FromJson(js ? nlohmann::json::parse(js) : nlohmann::json::object());
}

void SceneLinkStore::Save() const
{
	Save(ObsBootstrap::SceneCollections().ActiveSceneLinksPath());
}

void SceneLinkStore::Save(const std::string &path) const
{
	OBSDataAutoRelease root = obs_data_create_from_json(ToJson().dump().c_str());
	SaveJsonAtomic(root, path);
}
