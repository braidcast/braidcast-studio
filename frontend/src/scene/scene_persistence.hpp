#ifndef OBS_MULTISTREAM_FRONTEND_SCENE_PERSISTENCE_HPP_
#define OBS_MULTISTREAM_FRONTEND_SCENE_PERSISTENCE_HPP_

#include <string>
#include <vector>

// Save/load of the main-canvas scene collection -- scenes, their sources, and
// the full per-scene item layout -- to a per-collection JSON file under the
// shared braidcast/basic config dir. The new CEF frontend otherwise
// rebuilds a placeholder scene every boot and never persists user content, so
// anything added vanished on restart.
//
// Excluded from the save (restored separately): only the channel 1-6 global
// audio sources (re-seeded from audio_devices.json) and the channel-0 program
// transition (re-seeded from transitions.json). Additional-canvas scoped sources
// ARE included -- SaveFilter keeps them, obs_load_sources rebinds them to their
// canvas via the canvas_uuid libobs stamps on save, so per-canvas scenes round-
// trip through this same file.
//
// The no-arg Save()/Load() target the ACTIVE scene collection's file (resolved
// through the SceneCollections registry); the explicit-path overloads operate on
// a specific file (used by the registry/switch machinery).
namespace SceneCollection {

// The main-canvas scene list's user-defined order, as scene uuids. Self-healing:
// always reconciled against the scenes that currently exist (stale uuids
// dropped, untracked scenes appended in creation order) before it is returned, so
// callers never see a stale list. Empty only when the collection has no scenes.
const std::vector<std::string> &SceneOrder();

// Move the scene named by `sceneUuid` one slot toward `direction`
// ("up"|"down"), or to the relevant edge ("top"|"bottom"), within SceneOrder().
// Returns false if the uuid isn't a known main-canvas scene; a move already at
// the relevant edge is a no-op success (matches sceneItems' boundary behavior).
// Does not save -- the caller persists via Save().
bool ReorderScene(const std::string &sceneUuid, const std::string &direction);

// Move the scene named by `sceneUuid` to an absolute position within
// SceneOrder() (the drag-and-drop counterpart to ReorderScene's relative
// moves). `index` is clamped to [0, SceneOrder().size() - 1]. Returns false
// only if the uuid isn't a known main-canvas scene. Does not save -- the
// caller persists via Save().
bool MoveSceneToIndex(const std::string &sceneUuid, int index);

// Persist the active collection. No-op-safe; logs on failure.
void Save();
// Persist to an explicit file path.
void Save(const std::string &path);

// Restore the active collection and bind channel 0 to the saved current scene
// (falling back to the first loaded scene). Returns true when a collection was
// loaded and a scene bound; false when no file exists or it holds no scenes, in
// which case the caller builds the placeholder default scene. Either outcome also
// seeds + re-binds every additional canvas's scene internally (idempotent), so all
// callers -- boot and collection switch alike -- restore canvas scenes for free.
bool Load();
// Restore from an explicit file path.
bool Load(const std::string &path);

// Remove the active collection's scene world from libobs -- main-canvas scenes +
// plain inputs (exactly the set Save persists) -- preserving the channel 1-6 global
// audio sources and any additional-canvas sources, then drain the deferred-destroy
// queue. The caller must unbind/destroy the channel-0 transition first
// (Transitions::Shutdown). Used by the scene-collection switch to tear the outgoing
// collection down leak-safely (mirrors shutdown's ClearSceneData drain).
void ClearCurrent();

} // namespace SceneCollection

#endif // OBS_MULTISTREAM_FRONTEND_SCENE_PERSISTENCE_HPP_
