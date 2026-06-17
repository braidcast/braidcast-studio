#pragma once

#include <obs.hpp>
#include <string>
#include <unordered_map>

/* Per-scene-collection map: when a main (program) scene activates, each listed
 * canvas switches to its mapped scene. Persisted inside the scene-collection
 * JSON alongside canvas_bindings. Pure data — no libobs/Qt side effects. */
struct CanvasSceneLink {
	/* mainSceneUuid -> (canvasUuid -> canvasSceneUuid) */
	std::unordered_map<std::string, std::unordered_map<std::string, std::string>> map;

	/* Look up the canvas scene a given canvas should show for a main scene.
	 * Returns empty string when unlinked. */
	std::string Resolve(const std::string &mainSceneUuid, const std::string &canvasUuid) const;
	void Set(const std::string &mainSceneUuid, const std::string &canvasUuid, const std::string &canvasSceneUuid);
	void Unset(const std::string &mainSceneUuid, const std::string &canvasUuid);
	/* Drop every mapping that points a given canvas at one of its scenes, across
	 * all main scenes. Used when a canvas scene's "follows" target is changed, so
	 * it follows at most one main scene. */
	void UnsetByCanvasScene(const std::string &canvasUuid, const std::string &canvasSceneUuid);

	/* Serialize to / from an obs_data array named "canvas_scene_links". */
	[[nodiscard]] OBSDataArrayAutoRelease ToDataArray() const;
	static CanvasSceneLink FromDataArray(obs_data_array_t *arr);
};
