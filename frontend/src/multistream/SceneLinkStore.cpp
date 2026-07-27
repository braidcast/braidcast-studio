#include "SceneLinkStore.hpp"

#include "StorePaths.hpp"
#include "../obs_bootstrap.hpp"
#include "scene/scene_collections.hpp"

#include <util/platform.h>
#include <util/util.hpp>

nlohmann::json SceneLinkStore::ToJson() const
{
	OBSDataArrayAutoRelease arr = links.ToDataArray();
	return StoreJsonFromArray("canvas_scene_links", arr);
}

void SceneLinkStore::FromJson(const nlohmann::json &j)
{
	OBSDataArrayAutoRelease arr = StoreArrayFromJson(j, "canvas_scene_links");
	links = CanvasSceneLink::FromDataArray(arr);
}

void SceneLinkStore::Load()
{
	Load(ObsBootstrap::SceneCollections().ActiveSceneLinksPath());
}

void SceneLinkStore::Load(const std::string &path)
{
	FromJson(LoadStoreJson(path));
}

bool SceneLinkStore::Save() const
{
	return Save(ObsBootstrap::SceneCollections().ActiveSceneLinksPath());
}

bool SceneLinkStore::Save(const std::string &path) const
{
	return SaveStoreJson(ToJson(), path);
}
