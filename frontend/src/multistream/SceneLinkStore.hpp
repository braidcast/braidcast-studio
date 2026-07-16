#pragma once

#include "CanvasSceneLink.hpp"

#include <nlohmann/json.hpp>

#include <string>

// Holds the CanvasSceneLink (main-scene -> per-canvas scene activation map) for
// the new frontend and round-trips it to a per-scene-collection JSON sibling of
// the collection's scene file (scenes/<slug>.json -> scenes/<slug>.scene_links.json)
// under the shared <config>/braidcast/basic directory. Mirrors
// OutputBindingStore exactly; the wire format is the SAME "canvas_scene_links"
// array CanvasSceneLink::ToDataArray/FromDataArray produce.
//
// THREADING: UI-thread-only, exactly like its siblings CanvasStore /
// OutputBindingStore. Every accessor runs on the CEF UI thread (bridge sceneLink.*
// methods, bootstrap Start/Stop, scene-collection switch); no chat/events/overlay
// worker touches it, so it carries no lock. Do NOT call into it from a worker
// thread -- marshal onto the UI thread first (AsyncTask::PostToUi).
class SceneLinkStore {
public:
	void Load();                        // read the ACTIVE collection's links (empty if absent)
	void Load(const std::string &path); // read from an explicit file (empty if absent)
	bool Save() const;                  // write the ACTIVE collection's links; false on failure (logged)
	bool Save(const std::string &path) const;

	nlohmann::json ToJson() const;
	void FromJson(const nlohmann::json &j);

	CanvasSceneLink &Links() { return links; }
	const CanvasSceneLink &Links() const { return links; }

	void Clear() { links.map.clear(); }

private:
	CanvasSceneLink links;
};
