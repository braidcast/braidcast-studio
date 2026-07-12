// Test-owned definitions of the four production symbols the persistence layer
// resolves its collaborators through. In the app these live in obs_bootstrap.cpp,
// which pulls the entire CEF frontend; here each simply forwards to the harness's
// test-owned instances (TestSeam). The production SceneCollection::Save/Load code
// path is unchanged -- only the lookup of its singletons is redirected -- so the
// tests exercise the real save/load logic.
//
// Enumerated against the actual references in frontend/src/scene_persistence.cpp:
//   ObsBootstrap::SceneCollections()  (no-arg Save()/Load() -> ActiveScenePath)
//   ObsBootstrap::CanvasRuntime()     (Save, Load/RestoreCanvasScenes, ClearCurrent)
//   ObsBootstrap::Canvases()          (Save, ClearCurrent)
//   Transitions::GetProgramScene()    (Save current-scene capture, Load bind)
//   SceneCollections::ActiveScenePath()
// The compiled store TUs (CanvasRuntime/CanvasStore/CanvasDefinition/StorePaths)
// reference no ObsBootstrap/Bridge/Transitions symbols, so these four close the
// link graph.

#include "testseam.hpp"

#include "obs_bootstrap.hpp"
#include "scene_collections.hpp"
#include "transitions.hpp"

#include "multistream/CanvasRuntime.hpp"
#include "multistream/CanvasStore.hpp"

#include <obs.h>

::SceneCollections &ObsBootstrap::SceneCollections()
{
	return TestSeam::Collections();
}

CanvasStore &ObsBootstrap::Canvases()
{
	return TestSeam::Canvases();
}

::CanvasRuntime &ObsBootstrap::CanvasRuntime()
{
	return TestSeam::Runtime();
}

// The fast slice binds the current scene directly to channel 0 (no program
// transition wrapper), so the real current scene IS channel 0's source. Matches
// the GetProgramScene contract: addref'd (obs_get_output_source), caller releases.
obs_source_t *Transitions::GetProgramScene()
{
	return obs_get_output_source(0);
}

// Redirect the active-collection path to the harness's per-test temp file so even
// the no-arg SceneCollection::Save()/Load() stay isolated.
std::string SceneCollections::ActiveScenePath() const
{
	return TestSeam::ActiveScenePath();
}
