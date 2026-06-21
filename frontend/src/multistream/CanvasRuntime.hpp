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
	void EnsureCanvas(const CanvasDefinition &def); // create one if absent (+ default scene on ch0)
	void RemoveCanvas(const std::string &uuid);     // obs_canvas_remove + release; no-op if absent
	bool ResetVideo(const CanvasDefinition &def);   // obs_canvas_reset_video to def res/fps

	obs_canvas_t *Find(const std::string &uuid) const; // null for Default/unknown
	video_t *VideoFor(const std::string &uuid) const;  // obs_canvas_get_video or null

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
