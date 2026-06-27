#pragma once

#include "CanvasSceneLink.hpp"

#include <nlohmann/json.hpp>

#include <string>

// Holds the CanvasSceneLink (main-scene -> per-canvas scene activation map) for
// the new frontend and round-trips it to a per-scene-collection JSON sibling of
// the collection's scene file (scenes/<slug>.json -> scenes/<slug>.scene_links.json)
// under the shared <config>/obs-multistream/basic directory. Mirrors
// OutputBindingStore exactly; the wire format is the SAME "canvas_scene_links"
// array CanvasSceneLink::ToDataArray/FromDataArray produce.
class SceneLinkStore {
public:
	void Load();                        // read the ACTIVE collection's links (empty if absent)
	void Load(const std::string &path); // read from an explicit file (empty if absent)
	void Save() const;                  // write the ACTIVE collection's links
	void Save(const std::string &path) const;

	nlohmann::json ToJson() const;
	void FromJson(const nlohmann::json &j);

	CanvasSceneLink &Links() { return links; }
	const CanvasSceneLink &Links() const { return links; }

	void Clear() { links.map.clear(); }

private:
	CanvasSceneLink links;
};
