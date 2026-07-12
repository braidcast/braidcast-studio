#ifndef BRAIDCAST_TEST_INTEGRATION_TESTSEAM_HPP_
#define BRAIDCAST_TEST_INTEGRATION_TESTSEAM_HPP_

#include <string>

// The dependency-injection seam that lets the REAL persistence layer
// (scene_persistence.cpp + the multistream stores) link into a headless test
// binary WITHOUT dragging in obs_bootstrap.cpp (which pulls the whole CEF app).
//
// scene_persistence.cpp reaches its collaborators through three symbols defined
// in obs_bootstrap.cpp -- ObsBootstrap::Canvases(), ObsBootstrap::CanvasRuntime(),
// ObsBootstrap::SceneCollections() -- plus Transitions::GetProgramScene(). The
// test provides its OWN definitions of exactly those symbols (bootstrap_shim.cpp),
// each delegating to the test-owned instances the harness constructs here. The
// production Save/Load code path runs unchanged; only the way it locates its
// singletons is redirected. This is a test seam, not a reimplementation of
// save/load.

class CanvasStore;
class CanvasRuntime;
class SceneCollections;

namespace TestSeam {

// The single instances the shim's ObsBootstrap accessors return. Owned by the
// harness (harness.cpp), constructed after obs_reset_video, destroyed before
// obs_shutdown so CanvasRuntime's obs_canvas teardown runs while libobs is up.
CanvasStore &Canvases();
CanvasRuntime &Runtime();
SceneCollections &Collections();

// The active collection's on-disk file for THIS test -- a path under a fresh
// per-test temp dir. The shim's SceneCollections::ActiveScenePath() returns it, so
// even the no-arg SceneCollection::Save()/Load() stay isolated to the temp dir and
// never touch the user's real profile.
const std::string &ActiveScenePath();
void SetActiveScenePath(const std::string &path);

} // namespace TestSeam

#endif // BRAIDCAST_TEST_INTEGRATION_TESTSEAM_HPP_
