#include "scene_persistence.hpp"

#include "log.hpp"
#include "obs_bootstrap.hpp"
#include "scene_collections.hpp"
#include "transitions.hpp"

#include "multistream/CanvasRuntime.hpp"
#include "multistream/CanvasStore.hpp"

#include <CanvasDefinition.hpp>

#include <obs.h>
#include <obs.hpp>
#include <util/platform.h>

#include <filesystem>
#include <string>

namespace SceneCollection {

namespace {

// Per-save context carrying the channel 1-6 global audio sources to exclude (the
// audio mixer persists those separately via audio_devices.json) plus the main
// canvas (the Default canvas), so its scenes are KEPT while additional-canvas
// scenes are dropped.
struct SaveContext {
	obs_source_t *audio[6] = {};
	obs_canvas_t *mainCanvas = nullptr; // borrowed; lifetime spans the save call
};

// obs_save_sources_filtered predicate: keep public, global, non-audio sources.
// libobs already excludes filters, removed sources, and private sources before
// invoking this; we additionally drop only the global audio channel sources. Both
// main-canvas and additional-canvas scoped sources are kept -- libobs stamps each
// scene's canvas_uuid on save and rebinds it on load (the canvas already exists by
// then), so per-canvas scenes round-trip. Plain inputs (color/browser/etc.) are
// always kept.
bool SaveFilter(void *data, obs_source_t *source)
{
	const auto *ctx = static_cast<const SaveContext *>(data);
	// The active program transition lives on channel 0 wrapping the current scene;
	// it is restored from transitions.json, not the scene collection (same as the
	// global audio channels above).
	if (obs_source_get_type(source) == OBS_SOURCE_TYPE_TRANSITION) {
		return false;
	}
	for (obs_source_t *audio : ctx->audio) {
		if (audio && source == audio) {
			return false;
		}
	}
	return true;
}

} // namespace

void Save()
{
	Save(ObsBootstrap::SceneCollections().ActiveScenePath());
}

void Save(const std::string &path)
{
	SaveContext ctx;
	OBSSourceAutoRelease audioRefs[6];
	for (uint32_t ch = 1; ch <= 6; ch++) {
		audioRefs[ch - 1] = obs_get_output_source(ch); // addref'd; may be null
		ctx.audio[ch - 1] = audioRefs[ch - 1];
	}
	OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas(); // addref'd; the Default canvas
	ctx.mainCanvas = mainCanvas;

	OBSDataArrayAutoRelease sources = obs_save_sources_filtered(SaveFilter, &ctx);

	OBSSourceAutoRelease current = Transitions::GetProgramScene(); // addref'd; unwraps the ch0 transition
	const char *currentName = current ? obs_source_get_name(current) : nullptr;

	OBSDataAutoRelease root = obs_data_create();
	obs_data_set_array(root, "sources", sources);
	obs_data_set_string(root, "current_scene", currentName ? currentName : "");

	// Persist each additional canvas's active scene (its channel-0 binding), keyed
	// by canvas uuid. Per-collection, mirroring current_scene for the main canvas.
	OBSDataAutoRelease canvasCurrent = obs_data_create();
	::CanvasRuntime &runtime = ObsBootstrap::CanvasRuntime();
	for (const CanvasDefinition &def : ObsBootstrap::Canvases().Definitions()) {
		if (def.isDefault) {
			continue;
		}
		OBSSourceAutoRelease cur = runtime.CurrentScene(def.uuid); // addref'd or null
		const char *name = cur ? obs_source_get_name(cur) : nullptr;
		if (name && *name) {
			obs_data_set_string(canvasCurrent, def.uuid.c_str(), name);
		}
	}
	obs_data_set_obj(root, "canvas_current", canvasCurrent);

	std::filesystem::path dir = std::filesystem::u8path(path).parent_path();
	os_mkdirs(dir.u8string().c_str());

	if (!obs_data_save_json_pretty_safe(root, path.c_str(), "tmp", "bak")) {
		HostLog("[scene] failed to save collection to " + path);
	}
}

bool Load(std::map<std::string, std::string> *outCanvasCurrent)
{
	return Load(ObsBootstrap::SceneCollections().ActiveScenePath(), outCanvasCurrent);
}

bool Load(const std::string &path, std::map<std::string, std::string> *outCanvasCurrent)
{
	OBSDataAutoRelease root = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!root) {
		return false;
	}

	OBSDataArrayAutoRelease sources = obs_data_get_array(root, "sources");
	if (!sources || obs_data_array_count(sources) == 0) {
		return false;
	}

	obs_load_sources(sources, nullptr, nullptr);

	if (outCanvasCurrent) {
		outCanvasCurrent->clear();
		OBSDataAutoRelease canvasCurrent = obs_data_get_obj(root, "canvas_current");
		if (canvasCurrent) {
			for (obs_data_item_t *item = obs_data_first(canvasCurrent); item; obs_data_item_next(&item)) {
				const char *uuid = obs_data_item_get_name(item);
				const char *scene = obs_data_item_get_string(item);
				if (uuid && *uuid && scene && *scene) {
					(*outCanvasCurrent)[uuid] = scene;
				}
			}
		}
	}

	// Bind channel 0 to the saved current scene; fall back to the first scene.
	const char *savedName = obs_data_get_string(root, "current_scene");
	OBSSourceAutoRelease scene;
	if (savedName && savedName[0] != '\0') {
		OBSSourceAutoRelease byName = obs_get_source_by_name(savedName);
		if (byName && obs_scene_from_source(byName)) {
			scene = std::move(byName);
		}
	}
	if (!scene) {
		obs_source_t *first = nullptr;
		obs_enum_scenes(
			[](void *param, obs_source_t *source) -> bool {
				obs_source_get_ref(source); // keep for the binder below
				*static_cast<obs_source_t **>(param) = source;
				return false; // first scene only
			},
			&first);
		scene = first; // takes ownership of the ref the enum added
	}
	if (!scene) {
		return false;
	}

	obs_set_output_source(0, scene);
	HostLog(std::string("[scene] loaded collection, current='") + obs_source_get_name(scene) + "'");
	return true;
}

void ClearCurrent()
{
	// Build the same exclude context Save uses, so the remove boundary stays in
	// lockstep with the keep boundary (SaveFilter): the channel 1-6 audio sources and
	// any additional-canvas sources are preserved; main-canvas scenes + plain inputs
	// are removed.
	SaveContext ctx;
	OBSSourceAutoRelease audioRefs[6];
	for (uint32_t ch = 1; ch <= 6; ch++) {
		audioRefs[ch - 1] = obs_get_output_source(ch); // addref'd; may be null
		ctx.audio[ch - 1] = audioRefs[ch - 1];
	}
	OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas(); // addref'd; the Default canvas
	ctx.mainCanvas = mainCanvas;

	// SaveFilter returns true for sources this collection owns; remove exactly those.
	// libobs hands the same source to obs_enum_scenes (scenes) and obs_enum_sources
	// (inputs); a transition would be skipped by SaveFilter, but the caller already
	// destroyed it.
	auto removeIfOwned = [](void *data, obs_source_t *source) -> bool {
		if (SaveFilter(data, source)) {
			obs_source_remove(source);
		}
		return true;
	};
	obs_enum_scenes(removeIfOwned, &ctx);
	obs_enum_sources(removeIfOwned, &ctx);

	// Deferred source destruction can cascade across the destruction-task thread;
	// drain in a loop until no more work is spawned, mirroring shutdown's teardown.
	while (obs_wait_for_destroy_queue()) {
	}
}

} // namespace SceneCollection
