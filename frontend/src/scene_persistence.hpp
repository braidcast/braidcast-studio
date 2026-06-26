#ifndef OBS_MULTISTREAM_FRONTEND_SCENE_PERSISTENCE_HPP_
#define OBS_MULTISTREAM_FRONTEND_SCENE_PERSISTENCE_HPP_

#include <string>

// Save/load of the main-canvas scene collection -- scenes, their sources, and
// the full per-scene item layout -- to a per-collection JSON file under the
// shared obs-multistream/basic config dir. The new CEF frontend otherwise
// rebuilds a placeholder scene every boot and never persists user content, so
// anything added vanished on restart.
//
// Excluded from the save (restored separately or deferred): the channel 1-6
// global audio sources (re-seeded from audio_devices.json) and additional-canvas
// scoped sources (per-canvas persistence is a later phase).
//
// The no-arg Save()/Load() target the ACTIVE scene collection's file (resolved
// through the SceneCollections registry); the explicit-path overloads operate on
// a specific file (used by the registry/switch machinery).
namespace SceneCollection {

// Persist the active collection. No-op-safe; logs on failure.
void Save();
// Persist to an explicit file path.
void Save(const std::string &path);

// Restore the active collection at boot and bind channel 0 to the saved current
// scene (falling back to the first loaded scene). Returns true when a collection
// was loaded and a scene bound; false when no file exists or it holds no scenes,
// in which case the caller builds the placeholder default scene.
bool Load();
// Restore from an explicit file path.
bool Load(const std::string &path);

} // namespace SceneCollection

#endif // OBS_MULTISTREAM_FRONTEND_SCENE_PERSISTENCE_HPP_
