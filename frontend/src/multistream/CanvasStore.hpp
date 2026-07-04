#pragma once

#include "CanvasDefinition.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

// Owns the global list of canvas definitions for the new (non-Qt) frontend,
// persisted to the SAME standalone canvases.json the legacy frontend uses
// (<config>/braidcast/basic/canvases.json). De-Qt'd port of the legacy
// CanvasManager: the only behavioral difference is FilePath() resolves the
// config dir via libobs os_get_config_path instead of OBSApp.
//
// Always contains exactly one isDefault == true definition (seeded on
// construction so Default()/Definitions() are valid even before Load()).
class CanvasStore {
public:
	CanvasStore() { EnsureDefault(); }

	void Load();       // read canvases.json (replaces contents; re-seeds Default if absent)
	void Save() const; // write canvases.json atomically

	// The whole model as JSON, in the SAME shape canvases.json holds (the single
	// serializer; Load/Save route through it). FromJson replaces contents and
	// re-seeds the Default if absent, mirroring Load(). Used by settings.snapshot/
	// settings.restore for the transactional Settings footer.
	nlohmann::json ToJson() const;
	void FromJson(const nlohmann::json &j);

	const std::vector<CanvasDefinition> &Definitions() const { return definitions; }
	const CanvasDefinition &Default() const; // always present (see invariant above)

	// Seed the Default canvas's stream encoders if unset. Call AFTER modules load
	// (obs_encoder_defaults needs registered encoders). Returns true if it changed
	// anything (caller should Save()).
	bool EnsureDefaultEncoders();

	// The returned reference/pointer is invalidated by any subsequent Add/Remove/Load.
	CanvasDefinition *Find(const std::string &uuid);
	CanvasDefinition &Add(CanvasDefinition def); // assigns uuid if empty
	void Remove(const std::string &uuid);        // no-op for the Default

	// Drop all definitions (releasing their obs_data) without re-seeding. For
	// teardown only, so the leak count is measured against an empty model.
	void Clear() { definitions.clear(); }

	// <config>/braidcast/basic/canvases.json -- identical to the legacy path.
	static std::string FilePath();

private:
	void EnsureDefault(); // append a 1080p60 Default if none present

	std::vector<CanvasDefinition> definitions;
};
