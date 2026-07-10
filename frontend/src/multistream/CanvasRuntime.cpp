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

void CanvasRuntime::EnsureScenes()
{
	for (const Entry &e : canvases) {
		EnsureScene(e.canvas); // no-op when the canvas already has a scene
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

obs_source_t *CanvasRuntime::CurrentScene(const std::string &uuid) const
{
	obs_canvas_t *canvas = Find(uuid);
	if (!canvas) {
		return nullptr;
	}
	// obs_canvas_get_channel hands back an addref'd source; pass that ownership
	// straight to the caller (matches the bridge's addref'd scene-source contract).
	obs_source_t *cur = obs_canvas_get_channel(canvas, 0);
	if (cur && !obs_scene_from_source(cur)) {
		obs_source_release(cur); // channel 0 holds a non-scene source
		return nullptr;
	}
	return cur;
}

std::vector<CanvasRuntime::SceneInfo> CanvasRuntime::Scenes(const std::string &uuid) const
{
	std::vector<SceneInfo> scenes;
	obs_canvas_t *canvas = Find(uuid);
	if (!canvas) {
		return scenes;
	}

	// The current scene (channel 0) flags which entry is current. Hold its name
	// while enumerating; release the addref'd source afterward.
	OBSSourceAutoRelease cur = obs_canvas_get_channel(canvas, 0);
	const char *curName = cur ? obs_source_get_name(cur) : nullptr;
	const std::string currentName = curName ? curName : std::string();

	struct Ctx {
		std::vector<SceneInfo> *out;
		const std::string *current;
	} ctx{&scenes, &currentName};

	// The enum callback's source is owned by the canvas for the call's duration;
	// read its name/uuid in-place (no extra ref needed, mirroring EnsureScene).
	obs_canvas_enum_scenes(
		canvas,
		[](void *param, obs_source_t *scene) -> bool {
			auto *c = static_cast<Ctx *>(param);
			const char *name = obs_source_get_name(scene);
			const char *sceneUuid = obs_source_get_uuid(scene);
			if (name) {
				c->out->push_back(SceneInfo{name, sceneUuid ? sceneUuid : std::string(),
							    !c->current->empty() && *c->current == name});
			}
			return true; // keep enumerating
		},
		&ctx);

	return scenes;
}

std::string CanvasRuntime::SceneNameForUuid(const std::string &uuid, const std::string &sceneUuid) const
{
	for (const SceneInfo &s : Scenes(uuid)) {
		if (s.uuid == sceneUuid) {
			return s.name;
		}
	}
	return std::string();
}

bool CanvasRuntime::SetCurrentScene(const std::string &uuid, const std::string &sceneName)
{
	obs_canvas_t *canvas = Find(uuid);
	if (!canvas) {
		return false;
	}
	OBSSourceAutoRelease scene = obs_canvas_get_source_by_name(canvas, sceneName.c_str()); // addref'd
	if (!scene || !obs_scene_from_source(scene)) {
		return false;
	}
	obs_canvas_set_channel(canvas, 0, scene);
	return true;
}

obs_source_t *CanvasRuntime::CreateScene(const std::string &uuid, const std::string &name)
{
	obs_canvas_t *canvas = Find(uuid);
	if (!canvas) {
		return nullptr;
	}
	// Reject a duplicate name within this canvas (mirrors the global scenes.create
	// guard, but scoped to the canvas's own source namespace).
	OBSSourceAutoRelease clash = obs_canvas_get_source_by_name(canvas, name.c_str());
	if (clash) {
		return nullptr;
	}
	obs_scene_t *scene = obs_canvas_scene_create(canvas, name.c_str());
	if (!scene) {
		return nullptr;
	}
	// obs_canvas_scene_create returns the scene with the creation ref still owned by
	// the caller, while the canvas holds its OWN strong ref (obs_canvas_insert_source
	// addref's under SCENE_REF). Hand that creation ref straight to the caller -- it
	// already matches the bridge's addref'd scene-source contract (caller releases),
	// so do NOT add another ref or the creation ref leaks.
	return obs_scene_get_source(scene);
}

bool CanvasRuntime::RemoveScene(const std::string &uuid, const std::string &sceneName)
{
	obs_canvas_t *canvas = Find(uuid);
	if (!canvas) {
		return false;
	}

	// Count this canvas's scenes and find a fallback (any scene but the target) so we
	// can move channel 0 off the target before removing it; refuse the last scene.
	struct Ctx {
		std::string target;
		int count = 0;
		obs_source_t *fallback = nullptr; // addref'd
	} ctx;
	ctx.target = sceneName;
	obs_canvas_enum_scenes(
		canvas,
		[](void *param, obs_source_t *scene) -> bool {
			auto *c = static_cast<Ctx *>(param);
			c->count++;
			const char *n = obs_source_get_name(scene);
			if (n && c->target != n && !c->fallback) {
				obs_source_get_ref(scene); // keep for fallback past enum
				c->fallback = scene;
			}
			return true;
		},
		&ctx);

	if (ctx.count <= 1) {
		if (ctx.fallback) {
			obs_source_release(ctx.fallback);
		}
		return false;
	}

	OBSSourceAutoRelease target = obs_canvas_get_source_by_name(canvas, sceneName.c_str()); // addref'd
	if (!target || !obs_scene_from_source(target)) {
		if (ctx.fallback) {
			obs_source_release(ctx.fallback);
		}
		return false;
	}

	// If the target is the canvas's current scene, switch channel 0 to the fallback
	// before removing it so the mix never points at a removed scene.
	OBSSourceAutoRelease current = obs_canvas_get_channel(canvas, 0);
	if (current && current.Get() == target.Get() && ctx.fallback) {
		obs_canvas_set_channel(canvas, 0, ctx.fallback);
	}

	obs_canvas_scene_remove(obs_scene_from_source(target));
	if (ctx.fallback) {
		obs_source_release(ctx.fallback);
	}
	return true;
}

bool CanvasRuntime::RenameScene(const std::string &uuid, const std::string &from, const std::string &to)
{
	obs_canvas_t *canvas = Find(uuid);
	if (!canvas) {
		return false;
	}
	if (from == to) {
		return true;
	}
	OBSSourceAutoRelease clash = obs_canvas_get_source_by_name(canvas, to.c_str());
	if (clash) {
		return false; // name already taken within this canvas
	}
	OBSSourceAutoRelease scene = obs_canvas_get_source_by_name(canvas, from.c_str()); // addref'd
	if (!scene || !obs_scene_from_source(scene)) {
		return false;
	}
	obs_source_set_name(scene, to.c_str());
	return true;
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
