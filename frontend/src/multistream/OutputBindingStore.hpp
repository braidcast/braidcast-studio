#pragma once

#include "OutputBinding.hpp"

#include <nlohmann/json.hpp>

#include <string>

// Holds the OutputBindings (profile x canvas routing edges) for the new frontend
// and round-trips them to a per-scene-collection JSON sibling of the collection's
// scene file (scenes/<slug>.json -> scenes/<slug>.output_bindings.json) under the
// shared <config>/braidcast/basic directory.
//
// Bindings are PER scene-collection (each show routes to its own destinations);
// canvases + stream profiles stay global. The no-arg Load()/Save() target the
// ACTIVE collection's bindings file (resolved through SceneCollections::
// ActiveBindingsPath); the explicit-path overloads operate on a specific file
// (used by the switch machinery to save the outgoing / load the incoming
// collection). The wire format is the SAME "output_bindings" array the legacy
// OutputBindings::ToDataArray/FromDataArray produce.
class OutputBindingStore {
public:
	void Load();                        // read the ACTIVE collection's bindings (empty if absent)
	void Load(const std::string &path); // read from an explicit file (empty if absent)
	void Save() const;                  // write the ACTIVE collection's bindings
	void Save(const std::string &path) const;

	// The whole model as JSON, in the SAME shape output_bindings.json holds (the
	// single serializer; Load/Save route through it). FromJson replaces contents,
	// mirroring Load(). Used by settings.snapshot/settings.restore for the
	// transactional Settings footer.
	nlohmann::json ToJson() const;
	void FromJson(const nlohmann::json &j);

	OutputBindings &Bindings() { return bindings; }
	const OutputBindings &Bindings() const { return bindings; }

	// Drop all bindings for teardown leak measurement.
	void Clear() { bindings.bindings.clear(); }

	// The legacy single global file, <config>/braidcast/basic/output_bindings.json.
	// Pre-6a bindings lived here; now used only as the first-run migration source.
	static std::string FilePath();

private:
	OutputBindings bindings;
};
