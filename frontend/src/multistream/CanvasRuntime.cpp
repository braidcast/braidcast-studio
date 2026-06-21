#include "CanvasRuntime.hpp"

#include "CanvasStore.hpp"

#include <CanvasDefinition.hpp>

CanvasRuntime::CanvasRuntime(CanvasStore &defs_) : defs(defs_) {}

CanvasRuntime::~CanvasRuntime()
{
	ClearAll();
}

void CanvasRuntime::BuildVideoInfo(const CanvasDefinition &def, obs_video_info &ovi) const
{
	// CanvasDefinition::ToVideoInfo leaves graphics_module/adapter null (the
	// legacy frontend filled them from App()->GetRenderModule(); this frontend
	// has no App()). Query the running pipeline instead so the canvas mix uses the
	// same graphics backend + adapter as the main video that the bootstrap reset.
	def.ToVideoInfo(ovi, &defs.Default());

	obs_video_info main_ovi = {};
	if (obs_get_video_info(&main_ovi)) {
		ovi.graphics_module = main_ovi.graphics_module;
		ovi.adapter = main_ovi.adapter;
	}
}

void CanvasRuntime::EnsureScene(obs_canvas_t *canvas)
{
	if (!canvas) {
		return;
	}

	// Find the canvas's first existing scene, if any.
	struct Ctx {
		obs_source_t *first = nullptr;
	} ctx;
	obs_canvas_enum_scenes(
		canvas,
		[](void *param, obs_source_t *scene) {
			static_cast<Ctx *>(param)->first = scene;
			return false; // first scene is enough
		},
		&ctx);

	// Create a default scene if the canvas has none. The scene's source is owned
	// by the canvas (SCENE_REF) once on a channel; the AutoRelease only drops our
	// local create-ref so teardown stays at the libobs leak baseline.
	obs_source_t *sceneSource = ctx.first;
	OBSSceneAutoRelease scene;
	if (!ctx.first) {
		scene = obs_canvas_scene_create(canvas, "Scene");
		sceneSource = obs_scene_get_source(scene);
	}

	OBSSourceAutoRelease cur = obs_canvas_get_channel(canvas, 0);
	if (!cur && sceneSource) {
		obs_canvas_set_channel(canvas, 0, sceneSource);
	}
}

void CanvasRuntime::EnsureCanvas(const CanvasDefinition &def)
{
	if (def.isDefault || def.uuid.empty()) {
		return;
	}
	if (Find(def.uuid)) {
		return;
	}

	obs_video_info ovi = {};
	BuildVideoInfo(def, ovi);

	// Preserve the definition's uuid: obs_load_canvas restores the stored uuid,
	// while obs_canvas_create would mint a fresh one each launch and break the
	// engine resolver's binding->mix match. PROGRAM = ACTIVATE | MIX_AUDIO |
	// SCENE_REF, mirroring the legacy AddCanvas flags.
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_string(data, "name", def.name.c_str());
	obs_data_set_string(data, "uuid", def.uuid.c_str());
	obs_data_set_int(data, "flags", PROGRAM);

	obs_canvas_t *canvas = obs_load_canvas(data);
	if (!canvas) {
		blog(LOG_WARNING, "CanvasRuntime: obs_load_canvas failed for '%s' (uuid %s)", def.name.c_str(),
		     def.uuid.c_str());
		return;
	}
	if (!obs_canvas_reset_video(canvas, &ovi)) {
		blog(LOG_WARNING, "CanvasRuntime: canvas '%s' (uuid %s) created without a video mix; reset_video failed",
		     def.name.c_str(), def.uuid.c_str());
	}

	EnsureScene(canvas);

	canvases.push_back(Entry{def.uuid, canvas});
}

void CanvasRuntime::SyncFromDefinitions()
{
	for (const CanvasDefinition &def : defs.Definitions()) {
		if (!def.isDefault) {
			EnsureCanvas(def);
		}
	}
}

void CanvasRuntime::RemoveCanvas(const std::string &uuid)
{
	for (auto it = canvases.begin(); it != canvases.end(); ++it) {
		if (it->uuid == uuid) {
			DestroyCanvas(it->canvas);
			canvases.erase(it);
			return;
		}
	}
}

bool CanvasRuntime::ResetVideo(const CanvasDefinition &def)
{
	obs_canvas_t *canvas = Find(def.uuid);
	if (!canvas) {
		return false;
	}
	obs_video_info ovi = {};
	BuildVideoInfo(def, ovi);
	return obs_canvas_reset_video(canvas, &ovi);
}

obs_canvas_t *CanvasRuntime::Find(const std::string &uuid) const
{
	for (const Entry &e : canvases) {
		if (e.uuid == uuid) {
			return e.canvas;
		}
	}
	return nullptr;
}

video_t *CanvasRuntime::VideoFor(const std::string &uuid) const
{
	obs_canvas_t *canvas = Find(uuid);
	return canvas ? obs_canvas_get_video(canvas) : nullptr;
}

void CanvasRuntime::DestroyCanvas(obs_canvas_t *canvas)
{
	if (!canvas) {
		return;
	}

	// Detach the canvas's scenes while the canvas is still live. obs_canvas_destroy
	// only drops the SCENE_REF strong ref on its scenes; it never calls
	// obs_canvas_remove_source, so the scene's removal from canvas->sources runs
	// later from obs_source_destroy -- by which point the canvas's strong refs are
	// already gone and obs_weak_canvas_get_canvas() returns NULL, so the cleanup is
	// skipped and the canvas->sources hash table (uthash buckets) leaks. Removing
	// the scenes here, while the weak canvas ref still resolves, empties that hash
	// table and drops the per-scene weak canvas ref before the canvas is freed.
	struct Ctx {
		std::vector<OBSSource> scenes;
	} ctx;
	obs_canvas_enum_scenes(
		canvas,
		[](void *param, obs_source_t *sceneSource) {
			// OBSSource takes a fresh strong ref so the source survives past enum
			// (which drops its own ref when the callback returns); removal happens
			// below, outside the canvas sources_mutex the enum holds.
			static_cast<Ctx *>(param)->scenes.emplace_back(sceneSource);
			return true;
		},
		&ctx);
	for (obs_source_t *sceneSource : ctx.scenes) {
		obs_canvas_scene_remove(obs_scene_from_source(sceneSource));
	}

	obs_canvas_remove(canvas);
	obs_canvas_release(canvas);
}

void CanvasRuntime::ClearAll()
{
	while (!canvases.empty()) {
		DestroyCanvas(canvases.back().canvas);
		canvases.pop_back();
	}
}
