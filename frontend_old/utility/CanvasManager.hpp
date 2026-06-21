#pragma once

#include "CanvasDefinition.hpp"

#include <string>
#include <vector>

/* Owns the global list of canvas definitions, persisted to a standalone
 * canvases.json (parallel to scene-collection JSON, NOT a config section).
 * Loaded once at startup; survives profile and scene-collection switches.
 * Always contains exactly one isDefault==true definition. */
class CanvasManager {
public:
	/* Seeds the Default up front so the "exactly one Default" invariant holds from
	 * construction — Default()/Definitions() are valid even before Load(). */
	CanvasManager() { EnsureDefault(); }

	void Load();       // read canvases.json (replaces contents; re-seeds Default if absent)
	void Save() const; // write canvases.json atomically

	const std::vector<CanvasDefinition> &Definitions() const { return definitions; }
	const CanvasDefinition &Default() const; // always present (see invariant above)

	/* Seed the Default canvas's stream encoders if unset. Call AFTER modules load
	 * (obs_encoder_defaults needs registered encoders). Returns true if it changed
	 * anything (caller should Save()). */
	bool EnsureDefaultEncoders();

	/* The returned reference/pointer is invalidated by any subsequent Add/Remove/Load. */
	CanvasDefinition *Find(const std::string &uuid);
	CanvasDefinition &Add(CanvasDefinition def); // assigns uuid if empty
	void Remove(const std::string &uuid);        // no-op for the Default

private:
	static std::string FilePath(); // <userProfilesLocation>/obs-multistream/basic/canvases.json
	void EnsureDefault();          // append a 1080p60 Default if none present

	std::vector<CanvasDefinition> definitions;
};
