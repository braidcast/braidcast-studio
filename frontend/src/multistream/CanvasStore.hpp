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

	void Load();                        // read canvases.json (replaces contents; re-seeds Default if absent)
	void Load(const std::string &path); // the same from a specific file
	bool Save() const;                  // write canvases.json atomically; false on write failure (logged)

	// Did the last Load find its file on disk but unusable (kept aside by LoadStoreJson)?
	// The model is then a seeded fallback rather than the user's canvases, so nothing may
	// write it at boot or prune bindings against it. It lasts until the next Load, i.e. for
	// the session: a user save does not lift it, and edits made on top of the fallback are
	// not pruned against either.
	bool LoadedUnusable() const { return loadedUnusable; }

	// The whole model as JSON, in the SAME shape canvases.json holds (the single
	// serializer; Load/Save route through it). FromJson replaces contents and
	// re-seeds the Default if absent, mirroring Load(). Used by settings.snapshot/
	// settings.restore for the transactional Settings footer.
	nlohmann::json ToJson() const;
	void FromJson(const nlohmann::json &j);

	const std::vector<CanvasDefinition> &Definitions() const { return definitions; }
	const CanvasDefinition &Default() const; // always present (see invariant above)

	// The single source of truth for "is this uuid the Default canvas": the empty
	// string, or the Default definition's uuid. Both the preview manager and the
	// canvas runtime route through here so the two can never disagree on Default.
	bool IsDefaultUuid(const std::string &uuid) const { return uuid.empty() || uuid == Default().uuid; }

	// Seed the Default canvas's stream encoders if unset. Call AFTER modules load
	// (obs_encoder_defaults needs registered encoders). Returns true if it changed
	// anything (caller should Save()).
	bool EnsureDefaultEncoders();

	// Give every canvas its permanent number, filling only the unassigned ones.
	// Called by FromJson, so every path that installs a model establishes the
	// invariant and no call site can forget it. Returns true if it assigned any.
	bool AssignNumbers();

	// Did the last model install have to assign numbers -- i.e. did it read a store
	// written before numbers existed? The assignment lives in memory either way; this
	// is what tells the caller to write it back, and writing it back is what stops a
	// later reorder from handing a canvas a different number than the scrollback
	// already named it by.
	bool NumbersMigrated() const { return numbersMigrated; }

	// The returned reference/pointer is invalidated by any subsequent Add/Remove/Load.
	CanvasDefinition *Find(const std::string &uuid);
	CanvasDefinition &Add(CanvasDefinition def); // assigns uuid if empty
	void Remove(const std::string &uuid);        // no-op for the Default
	// Reorder definitions to match `order`; unknown ids ignored, missing ids kept at end.
	void Reorder(const std::vector<std::string> &order);

	// Drop all definitions (releasing their obs_data) without re-seeding. For
	// teardown only, so the leak count is measured against an empty model.
	void Clear() { definitions.clear(); }

	// <config>/braidcast/basic/canvases.json -- identical to the legacy path.
	static std::string FilePath();

private:
	void EnsureDefault(); // append a 1080p60 Default if none present
	bool numbersMigrated = false;
	bool loadedUnusable = false;

	std::vector<CanvasDefinition> definitions;
};
