#include "CanvasSceneLink.hpp"

std::string CanvasSceneLink::Resolve(const std::string &mainSceneUuid, const std::string &canvasUuid) const
{
	auto it = map.find(mainSceneUuid);
	if (it == map.end()) {
		return {};
	}
	auto jt = it->second.find(canvasUuid);
	return jt == it->second.end() ? std::string{} : jt->second;
}

void CanvasSceneLink::Set(const std::string &mainSceneUuid, const std::string &canvasUuid,
			  const std::string &canvasSceneUuid)
{
	map[mainSceneUuid][canvasUuid] = canvasSceneUuid;
}

void CanvasSceneLink::Unset(const std::string &mainSceneUuid, const std::string &canvasUuid)
{
	auto it = map.find(mainSceneUuid);
	if (it == map.end()) {
		return;
	}
	it->second.erase(canvasUuid);
	if (it->second.empty()) {
		map.erase(it);
	}
}

void CanvasSceneLink::UnsetByCanvasScene(const std::string &canvasUuid, const std::string &canvasSceneUuid)
{
	for (auto it = map.begin(); it != map.end();) {
		auto jt = it->second.find(canvasUuid);
		if (jt != it->second.end() && jt->second == canvasSceneUuid) {
			it->second.erase(jt);
		}
		if (it->second.empty()) {
			it = map.erase(it);
		} else {
			++it;
		}
	}
}

OBSDataArrayAutoRelease CanvasSceneLink::ToDataArray() const
{
	OBSDataArrayAutoRelease arr = obs_data_array_create();
	for (const auto &[mainUuid, perCanvas] : map) {
		for (const auto &[canvasUuid, canvasSceneUuid] : perCanvas) {
			OBSDataAutoRelease entry = obs_data_create();
			obs_data_set_string(entry, "main_scene", mainUuid.c_str());
			obs_data_set_string(entry, "canvas", canvasUuid.c_str());
			obs_data_set_string(entry, "canvas_scene", canvasSceneUuid.c_str());
			obs_data_array_push_back(arr, entry);
		}
	}
	return arr;
}

CanvasSceneLink CanvasSceneLink::FromDataArray(obs_data_array_t *arr)
{
	CanvasSceneLink link;
	const size_t count = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease entry = obs_data_array_item(arr, i);
		std::string mainUuid = obs_data_get_string(entry, "main_scene");
		std::string canvasUuid = obs_data_get_string(entry, "canvas");
		std::string canvasScene = obs_data_get_string(entry, "canvas_scene");
		if (!mainUuid.empty() && !canvasUuid.empty() && !canvasScene.empty()) {
			link.Set(mainUuid, canvasUuid, canvasScene);
		}
	}
	return link;
}
