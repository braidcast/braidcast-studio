#ifndef BRAIDCAST_TEST_INTEGRATION_HARNESS_HPP_
#define BRAIDCAST_TEST_INTEGRATION_HARNESS_HPP_

#include <obs.h>

#include <cstdint>
#include <string>
#include <vector>

// Headless integration harness for the Braidcast persistence layer.
//
// Boots libobs once (obs_startup + a minimal D3D11 video/audio pipeline), then
// gives each test an isolated temp scene-collection file and a clean scene world.
// Tests drive the REAL SceneCollection::Save/Load and the REAL multistream stores
// (via the TestSeam shim) so a regression in production code fails the test.
//
// GPU: obs_reset_video needs a graphics device, but D3D11 WARP satisfies it on a
// headless/CI box (no physical GPU). The pure obs_data round-trip is
// context-free; the production Save/Load path that pulls the main canvas +
// CanvasRuntime mixes is not, which is why the harness stands up video.
namespace Harness {

// --- lifecycle (cmocka group setup/teardown) --------------------------------

// obs_startup + obs_add_data_path + obs_reset_video(D3D11) + obs_reset_audio, and
// construct the TestSeam store singletons. Returns false (with a logged reason) if
// the core or the graphics device could not come up. Idempotent.
bool BootObs();
// Destroy the store singletons (CanvasRuntime tears its obs_canvas mixes down
// while libobs is still alive), then obs_shutdown.
void ShutdownObs();

// --- per-test lifecycle (cmocka setup/teardown) -----------------------------

// Fresh temp dir + collection path for this test; a clean scene world (no scenes,
// only the Default canvas). Call at the start of every test.
void BeginTest();
// Remove every scene/source this test created (SceneCollection::ClearCurrent +
// drain) and every additional canvas, then delete the temp dir. Leaves the core
// booted for the next test.
void EndTest();

// The isolated collection file for the current test.
const std::string &ScenePath();

// --- helpers ----------------------------------------------------------------

// Create a main-canvas scene (obs_scene_create). The FIRST scene created in a test
// is bound to output channel 0 so it becomes the "current" scene Save records.
// Returns a BORROWED scene source (the harness owns the lifetime and releases it in
// TeardownWorld) -- do NOT wrap it in an auto-release or use it after teardown.
obs_source_t *CreateMainScene(const char *name);

// Register an additional (non-Default) canvas of the given size and bring up its
// live obs_canvas_t mix via the real CanvasRuntime. Returns its uuid.
std::string AddCanvas(uint32_t width, uint32_t height);

// Create a scene inside an additional canvas and bind it as that canvas's current
// scene. Returns a BORROWED scene source (harness-owned; see CreateMainScene) or
// null on failure.
obs_source_t *CreateCanvasScene(const std::string &canvasUuid, const char *name);

// SceneCollection::Save/Load against this test's isolated path.
void Save();
bool Load();

// SceneCollection::ClearCurrent + drain the deferred-destroy queue.
void TeardownWorld();

// Names of the scenes the real CanvasRuntime currently holds for an additional
// canvas, for asserting a canvas's scene set after reload.
std::vector<std::string> CanvasSceneNames(const std::string &canvasUuid);

// The scene SOURCE named `name` within an additional canvas, addref'd; caller
// releases. Canvas scenes live in the canvas's OWN source namespace, so
// obs_get_source_by_name does not see them -- this is the correct way to fetch one.
// Null if the canvas or the named scene is absent.
obs_source_t *CanvasSceneSource(const std::string &canvasUuid, const char *name);

// Absolute path to the OBS rundir data dir (…/data/libobs), derived from the test
// executable's own location. Exposed for diagnostics.
std::string DataPath();

} // namespace Harness

#endif // BRAIDCAST_TEST_INTEGRATION_HARNESS_HPP_
