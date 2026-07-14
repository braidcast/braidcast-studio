#ifndef OBS_MULTISTREAM_FRONTEND_TRANSITIONS_HPP_
#define OBS_MULTISTREAM_FRONTEND_TRANSITIONS_HPP_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <obs.h>

// The program transition seam (Phase 4). Output channel 0 holds a single active
// transition source that WRAPS the current global scene; scene switches animate
// through it (Fade by default) instead of hard-cutting the scene onto channel 0.
//
// Every place that reads channel 0 to find the "current scene" (scenes.list
// current flag, the preview surface, the scene-save) goes through
// GetProgramScene(), which unwraps the transition. Every place that sets the
// current scene goes through SetProgramScene(). The transition type + duration
// persist to transitions.json (NOT the scene collection, like the global audio
// channels). Default/global program path only -- additional-canvas switches stay
// direct (CanvasRuntime owns those channel-0 binds).
namespace Transitions {

// Create the active transition (restored id / default fade@300ms), wrap whatever
// scene is currently bound to channel 0, and bind the transition to channel 0.
// Call AFTER the scene is established (Load/CreateDefaultScene) and after
// obs_reset_video (the transition is sized to the base canvas).
void Init();

// Unbind channel 0 and destroy the active transition. Call in Stop BEFORE the
// scene-removal teardown so the transition releases its wrapped scene first.
void Shutdown();

// The real current scene, addref'd (caller releases) or null. Unwraps the
// channel-0 transition; usable as a drop-in for obs_get_output_source(0) on the
// global program path.
obs_source_t *GetProgramScene();

// Make `scene` the program scene. animate => obs_transition_start over the
// configured duration; otherwise obs_transition_set (no animation). No-op-safe
// when no transition exists (falls back to a direct channel-0 bind).
void SetProgramScene(obs_source_t *scene, bool animate);

// {id, displayName} for every registered transition type.
std::vector<std::pair<std::string, std::string>> TypeList();

// The active transition's id, display name, and configured duration (ms).
void Current(std::string &id, std::string &name, uint32_t &durationMs);

// Swap the active transition to `id`, preserving the wrapped scene, and persist.
// No-op success when `id` is already active. Returns false and fills `error`
// when `id` is not a registered transition type.
bool SetCurrentType(const std::string &id, std::string &error);

// Set the transition duration (ms) and persist. 0 is allowed (cut-like); clamped
// to a sane maximum so a switch can never hang.
void SetDuration(uint32_t durationMs);

// Re-size the active transition to the (changed) base canvas. Call after a
// successful obs_reset_video so the program output renders at the new dimensions.
// No-op when no transition exists.
void OnVideoReset();

} // namespace Transitions

#endif // OBS_MULTISTREAM_FRONTEND_TRANSITIONS_HPP_
