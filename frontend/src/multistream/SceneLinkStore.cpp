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
	bool unusable = false;
	FromJson(LoadStoreJson(path, OnUnusable::Keep, &unusable));
	// The switch flush and the scene and canvas prunes save with no change of their own.
	hold.AfterLoad(unusable, ToJson().dump());
}

bool SceneLinkStore::Save() const
{
	return Save(ObsBootstrap::SceneCollections().ActiveSceneLinksPath());
}

bool SceneLinkStore::Save(const std::string &path) const
{
	const nlohmann::json root = ToJson();
	if (hold.Skips(root.dump())) {
		return true;
	}
	return SaveStoreJson(root, path);
}
