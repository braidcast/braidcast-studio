#ifndef OBS_MULTISTREAM_FRONTEND_SCENE_COLLECTIONS_HPP_
#define OBS_MULTISTREAM_FRONTEND_SCENE_COLLECTIONS_HPP_

#include <string>
#include <vector>

// Registry of scene collections for the CEF frontend. A scene collection is the
// per-collection set of scenes/sources that scene_persistence captures (main
// canvas scenes + plain inputs), each stored in its own file under the shared
// obs-multistream/basic config dir. The registry itself persists to an index,
// scene_collections.json, holding the ordered list plus which one is active.
//
// Canvases (canvases.json) and stream profiles (streams.json) stay GLOBAL and
// are untouched by this registry.
//
// sceneFile is stored RELATIVE to the basic dir so the index survives a config
// dir move: the migrated collection reuses the legacy "scene_collection.json" in
// place (zero copy, zero data loss); new collections get "scenes/<slug>.json".
struct SceneCollectionRecord {
	std::string id;        // stable uuid
	std::string name;      // user-facing label
	std::string sceneFile; // path relative to <config>/obs-multistream/basic/
};

class SceneCollections {
public:
	// Read scene_collections.json. Replaces the in-memory list. No-op-safe when
	// the file is absent (leaves the list empty -> caller migrates/seeds).
	void Load();
	// Write scene_collections.json atomically (tmp + bak), creating the dir.
	void Save() const;

	const std::vector<SceneCollectionRecord> &List() const { return collections_; }

	// The active collection, or nullptr when the list is empty. The pointer is
	// invalidated by any subsequent Create/Remove/Load.
	const SceneCollectionRecord *Active() const;
	const std::string &ActiveId() const { return activeId_; }

	// Absolute path of the active collection's scene file (resolved through
	// MultistreamBasicPath). Falls back to the legacy single-file path when no
	// collection is active, so scene persistence keeps working pre-migration.
	std::string ActiveScenePath() const;

	// Seed a migration record that REUSES an existing scene file in place (no
	// copy). Used once on first run to adopt the legacy scene_collection.json.
	// Makes it active and saves the index.
	const SceneCollectionRecord &SeedExisting(const std::string &name, const std::string &existingRelFile);

	// Register a new collection with its own (initially nonexistent) scene file
	// at scenes/<slug>.json. Does NOT switch the active collection or touch any
	// scene file (switching is a later task). Saves the index. Returns the record.
	const SceneCollectionRecord &Create(const std::string &name);

	// Rename by id. Saves the index. Returns false when the id is unknown.
	bool Rename(const std::string &id, const std::string &name);

	// Remove by id, deleting its scene file on disk. Refuses to remove the last
	// remaining collection or the active one (switching is a later task). Saves
	// the index on success. Returns false (with `error`) when refused/unknown.
	bool Remove(const std::string &id, std::string &error);

	// Drop the in-memory list without touching disk. For teardown symmetry.
	void Clear() { collections_.clear(); activeId_.clear(); }

	// <config>/obs-multistream/basic/scene_collections.json.
	static std::string IndexPath();

private:
	// A basic-dir-relative scenes/<slug>.json unique against the current records
	// and any file already on disk.
	std::string UniqueSceneFileForName(const std::string &name) const;

	std::vector<SceneCollectionRecord> collections_;
	std::string activeId_;
};

#endif // OBS_MULTISTREAM_FRONTEND_SCENE_COLLECTIONS_HPP_
