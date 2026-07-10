#pragma once

#include <obs.hpp>

#include <string>
#include <vector>

class CanvasStore;
struct CanvasDefinition;

// Owns the live obs_canvas_t mixes for non-Default canvases. The Default canvas
// is NOT here -- it uses the global obs_get_video() pipeline. uuids are preserved
// (via obs_load_canvas) so MultistreamEngine's resolver can match a binding's
// canvasUuid to the right mix. Created from CanvasStore at startup, kept in sync
// by the canvas CRUD bridge methods, and torn down before obs_shutdown.
class CanvasRuntime {
public:
	explicit CanvasRuntime(CanvasStore &defs);
	~CanvasRuntime();

	void SyncFromDefinitions();                     // create any missing non-Default canvas (idempotent)
	void EnsureCanvas(const CanvasDefinition &def); // create one if absent (no scene; see EnsureScenes)
	void RemoveCanvas(const std::string &uuid);     // obs_canvas_remove + release; no-op if absent
	bool ResetVideo(const CanvasDefinition &def);   // obs_canvas_reset_video to def res/fps

	obs_canvas_t *Find(const std::string &uuid) const; // null for Default/unknown
	video_t *VideoFor(const std::string &uuid) const;  // obs_canvas_get_video or null

	// Per-canvas scene access, mirroring the legacy OBSBasic per-canvas scene API
	// (obs_canvas_get/set_channel(canvas, 0, ...) for "current", obs_canvas_*scene*
	// for CRUD). All resolve the live obs_canvas_t for `uuid`; an unknown/Default
	// uuid (no runtime mix) yields the empty/false result, so callers fall back to
	// the global channel-0 path. Refcounts follow the bridge's existing contracts.

	// One scene's name + uuid, as listed by Scenes().
	struct SceneInfo {
		std::string name;
		std::string uuid;
		bool current = false; // bound to the canvas's channel 0
	};

	// The canvas's current scene source (its channel-0 binding), addref'd; caller
	// releases. null for an unknown/Default canvas or an unbound channel.
	obs_source_t *CurrentScene(const std::string &uuid) const;
	std::vector<SceneInfo> Scenes(const std::string &uuid) const; // enum the canvas's scenes
	// The name of the scene with `sceneUuid` in canvas `uuid`, or empty if none
	// matches (unknown/Default canvas, or a stale/deleted scene uuid).
	std::string SceneNameForUuid(const std::string &uuid, const std::string &sceneUuid) const;
	bool SetCurrentScene(const std::string &uuid, const std::string &sceneName);
	// Create a scene in the canvas (addref'd scene source, caller releases) or null
	// on failure / unknown canvas. Does NOT bind it to channel 0.
	obs_source_t *CreateScene(const std::string &uuid, const std::string &name);
	bool RemoveScene(const std::string &uuid, const std::string &sceneName);
	bool RenameScene(const std::string &uuid, const std::string &from, const std::string &to);

	// Seed a default scene (bound to channel 0) for any live canvas that currently
	// has none. Idempotent: a canvas whose scenes were already restored is skipped.
	// Call AFTER SceneCollection::Load so restored canvas scenes are preserved and
	// only genuinely-empty canvases (first run / brand-new) get a placeholder.
	void EnsureScenes();

	void ClearAll(); // destroy all live canvases (teardown, before obs_shutdown)

private:
	void EnsureScene(obs_canvas_t *canvas);    // obs_canvas_scene_create + set_channel(0) if empty
	void DestroyCanvas(obs_canvas_t *canvas);  // detach scenes, obs_canvas_remove + release
	// Build an obs_video_info for a definition, filling graphics_module/adapter
	// from the running pipeline (CanvasDefinition::ToVideoInfo leaves them null).
	void BuildVideoInfo(const CanvasDefinition &def, obs_video_info &ovi) const;

	struct Entry {
		std::string uuid;
		obs_canvas_t *canvas; // owned: obs_canvas_remove + obs_canvas_release on teardown
	};

	CanvasStore &defs;
	std::vector<Entry> canvases;
};
