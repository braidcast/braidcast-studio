#include "OutputBindingStore.hpp"

#include "StorePaths.hpp"

#include "../obs_bootstrap.hpp"
#include "scene/scene_collections.hpp"

#include <util/platform.h>
#include <util/util.hpp>

std::string OutputBindingStore::FilePath()
{
	return MultistreamBasicPath("output_bindings.json");
}

nlohmann::json OutputBindingStore::ToJson() const
{
	OBSDataArrayAutoRelease arr = bindings.ToDataArray();
	return StoreJsonFromArray("output_bindings", arr);
}

void OutputBindingStore::FromJson(const nlohmann::json &j)
{
	OBSDataArrayAutoRelease arr = StoreArrayFromJson(j, "output_bindings");
	bindings = OutputBindings::FromDataArray(arr);
}

void OutputBindingStore::Load()
{
	Load(ObsBootstrap::SceneCollections().ActiveBindingsPath());
}

void OutputBindingStore::Load(const std::string &path)
{
	FromJson(LoadStoreJson(path));
}

bool OutputBindingStore::Save() const
{
	return Save(ObsBootstrap::SceneCollections().ActiveBindingsPath());
}

bool OutputBindingStore::Save(const std::string &path) const
{
	return SaveStoreJson(ToJson(), path);
}
