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
	OBSDataAutoRelease root = obs_data_create();
	OBSDataArrayAutoRelease arr = bindings.ToDataArray();
	obs_data_set_array(root, "output_bindings", arr);

	const char *js = obs_data_get_json(root);
	return js ? nlohmann::json::parse(js) : nlohmann::json::object();
}

void OutputBindingStore::FromJson(const nlohmann::json &j)
{
	bindings = OutputBindings{};

	if (j.is_object()) {
		OBSDataAutoRelease root = obs_data_create_from_json(j.dump().c_str());
		if (root) {
			OBSDataArrayAutoRelease arr = obs_data_get_array(root, "output_bindings");
			bindings = OutputBindings::FromDataArray(arr);
		}
	}
}

void OutputBindingStore::Load()
{
	Load(ObsBootstrap::SceneCollections().ActiveBindingsPath());
}

void OutputBindingStore::Load(const std::string &path)
{
	OBSDataAutoRelease root = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	const char *js = root ? obs_data_get_json(root) : nullptr;
	FromJson(js ? nlohmann::json::parse(js) : nlohmann::json::object());
}

void OutputBindingStore::Save() const
{
	Save(ObsBootstrap::SceneCollections().ActiveBindingsPath());
}

void OutputBindingStore::Save(const std::string &path) const
{
	OBSDataAutoRelease root = obs_data_create_from_json(ToJson().dump().c_str());
	SaveJsonAtomic(root, path);
}
