#include "scene_persistence.hpp"

#include "log.hpp"
#include "main_channel.hpp"
#include "obs_bootstrap.hpp"
#include "scene_collections.hpp"
#include "transitions.hpp"

#include "multistream/CanvasRuntime.hpp"
#include "multistream/CanvasStore.hpp"
#include "multistream/StorePaths.hpp"

#include <CanvasDefinition.hpp>

#include <obs.h>
#include <obs.hpp>
#include <util/platform.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace SceneCollection {

namespace {

// Every scene list's user-defined order (uuids), keyed by canvas uuid -- the EMPTY
// string is the Default canvas, whose order is persisted as the root "scene_order"
// array; every other key is an additional canvas, persisted together under
// "canvas_scene_order". Populated on Load(), mutated by ReorderScene(), and
// serialized back by Save(). libobs has no scene-ordering primitive (unlike scene
// items), so this map is the only record of the user's chosen order --
// obs_enum_scenes and obs_canvas_enum_scenes both yield creation order.
std::map<std::string, std::vector<std::string>> g_sceneOrder;

// Collect a scene's uuid into the std::vector<std::string> passed as `param`. The
// callback shape both obs_enum_scenes and obs_canvas_enum_scenes take.
bool CollectSceneUuid(void *param, obs_source_t *source)
{
	auto *out = static_cast<std::vector<std::string> *>(param);
	const char *uuid = obs_source_get_uuid(source);
	if (uuid) {
		out->push_back(uuid);
	}
	return true;
}

// Rebuild one canvas's tracked order to exactly match the scenes that currently
// exist on it: previously-tracked uuids that still resolve keep their relative
// order, then any scene not yet tracked (new since the last reconcile, or first run
// before any order was ever saved) is appended in enumeration order. Cheap enough
// (scene counts are small) to call before every read, so SceneOrder() and
// ReorderScene() are self-healing without needing a hook in every scene-mutating
// handler.
//
// An additional canvas that no longer resolves in the runtime is treated as having
// NO live scenes (its entry reconciles to empty) rather than falling back to the
// main canvas: a silent fallback there would let a canvas reorder rewrite the
// Default canvas's order.
void ReconcileSceneOrder(const std::string &canvasUuid)
{
	std::vector<std::string> live;
	if (canvasUuid.empty()) {
		obs_enum_scenes(CollectSceneUuid, &live);
	} else if (obs_canvas_t *canvas = ObsBootstrap::CanvasRuntime().Find(canvasUuid)) {
		obs_canvas_enum_scenes(canvas, CollectSceneUuid, &live);
	}

	std::vector<std::string> &tracked = g_sceneOrder[canvasUuid];
	std::vector<std::string> reconciled;
	reconciled.reserve(live.size());
	for (const std::string &uuid : tracked) {
		if (std::find(live.begin(), live.end(), uuid) != live.end()) {
			reconciled.push_back(uuid);
		}
	}
	for (const std::string &uuid : live) {
		if (std::find(reconciled.begin(), reconciled.end(), uuid) == reconciled.end()) {
			reconciled.push_back(uuid);
		}
	}
	tracked = std::move(reconciled);
}

// Drop order entries for canvases the model no longer knows about, so a removed
// canvas's uuid neither lingers in memory nor gets written back out forever. Keyed
// on the persisted CanvasStore definitions because THOSE are the record of which
// canvases exist: a runtime entry can be absent for a canvas the user still has
// (obs_load_canvas failing inside EnsureCanvas leaves the definition behind), and
// keying on the runtime would silently delete that canvas's order. The Default
// canvas's entry (the empty key) always survives.
void PruneSceneOrder()
{
	for (auto it = g_sceneOrder.begin(); it != g_sceneOrder.end();) {
		if (!it->first.empty() && !ObsBootstrap::Canvases().Find(it->first)) {
			it = g_sceneOrder.erase(it);
		} else {
			++it;
		}
	}
}

// Per-save context carrying the channel 1-6 global audio sources to exclude; the
// audio mixer persists those separately via audio_devices.json.
struct SaveContext {
	obs_source_t *audio[6] = {};
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

// Seed a default scene for any additional canvas left empty, then bind each to its
// saved active scene. Idempotent; safe to call after any Load outcome. Runs on the
// live CanvasRuntime, which exists before any Load on every reachable path.
void RestoreCanvasScenes(const std::map<std::string, std::string> &current)
{
	::CanvasRuntime &runtime = ObsBootstrap::CanvasRuntime();
	runtime.EnsureScenes();
	for (const auto &[uuid, sceneName] : current) {
		runtime.SetCurrentScene(uuid, sceneName); // no-op if unresolved
	}
}

} // namespace

const std::vector<std::string> &SceneOrder(const std::string &canvasUuid)
{
	ReconcileSceneOrder(canvasUuid);
	return g_sceneOrder[canvasUuid];
}

bool ReorderScene(const std::string &canvasUuid, const std::string &sceneUuid, const std::string &direction)
{
	ReconcileSceneOrder(canvasUuid);
	std::vector<std::string> &order = g_sceneOrder[canvasUuid];
	auto it = std::find(order.begin(), order.end(), sceneUuid);
	if (it == order.end()) {
		return false;
	}
	const size_t idx = static_cast<size_t>(std::distance(order.begin(), it));
	if (direction == "up" && idx > 0) {
		std::swap(order[idx], order[idx - 1]);
	} else if (direction == "down" && idx + 1 < order.size()) {
		std::swap(order[idx], order[idx + 1]);
	} else if (direction == "top" && idx > 0) {
		std::string uuid = std::move(order[idx]);
		order.erase(order.begin() + static_cast<std::ptrdiff_t>(idx));
		order.insert(order.begin(), std::move(uuid));
	} else if (direction == "bottom" && idx + 1 < order.size()) {
		std::string uuid = std::move(order[idx]);
		order.erase(order.begin() + static_cast<std::ptrdiff_t>(idx));
		order.push_back(std::move(uuid));
	}
	// Already at the relevant edge: no-op success, matching sceneItems.reorder.
	return true;
}

bool MoveSceneToIndex(const std::string &canvasUuid, const std::string &sceneUuid, int index)
{
	ReconcileSceneOrder(canvasUuid);
	std::vector<std::string> &order = g_sceneOrder[canvasUuid];
	auto it = std::find(order.begin(), order.end(), sceneUuid);
	if (it == order.end()) {
		return false;
	}
	std::string uuid = std::move(*it);
	order.erase(it);

	if (index < 0) {
		index = 0;
	}
	const int maxIndex = static_cast<int>(order.size());
	if (index > maxIndex) {
		index = maxIndex;
	}
	order.insert(order.begin() + index, std::move(uuid));
	return true;
}

namespace {

// The order to persist for one canvas. Deliberately NOT SceneOrder(): that call
// reconciles, and reconciling a canvas whose runtime entry is missing reduces its
// order to empty -- which the non-empty guard in Save then turns into the key being
// dropped from the file as well, destroying the record in memory and on disk at
// once. A save must never be the thing that discards state, so it reconciles only
// when the canvas actually resolves and otherwise persists what is still tracked,
// verbatim. Stale entries are dropped by PruneSceneOrder, which keys on the
// definitions rather than on the runtime.
const std::vector<std::string> &SceneOrderToPersist(const std::string &canvasUuid)
{
	if (ObsBootstrap::CanvasRuntime().Find(canvasUuid)) {
		return SceneOrder(canvasUuid);
	}
	return g_sceneOrder[canvasUuid];
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

	OBSDataArrayAutoRelease sources = obs_save_sources_filtered(SaveFilter, &ctx);

	OBSSourceAutoRelease current = Transitions::GetProgramScene(); // addref'd; unwraps the ch0 transition
	const char *currentName = current ? obs_source_get_name(current) : nullptr;

	OBSDataAutoRelease root = obs_data_create();
	obs_data_set_array(root, "sources", sources);
	obs_data_set_string(root, "current_scene", currentName ? currentName : "");

	// Persist each additional canvas's active scene (its channel-0 binding), keyed
	// by canvas uuid. Per-collection, mirroring current_scene for the main canvas.
	OBSDataAutoRelease canvasCurrent = obs_data_create();
	// Each additional canvas's own scene order, same canvas-uuid-keyed shape. Sibling
	// of "scene_order" below rather than part of it, so an existing collection (which
	// only ever carried the main array) keeps loading unchanged.
	OBSDataAutoRelease canvasSceneOrder = obs_data_create();
	::CanvasRuntime &runtime = ObsBootstrap::CanvasRuntime();
	PruneSceneOrder(); // the canvas walk below is where dead uuids are dropped
	for (const CanvasDefinition &def : ObsBootstrap::Canvases().Definitions()) {
		if (def.isDefault) {
			continue;
		}
		OBSSourceAutoRelease cur = runtime.CurrentScene(def.uuid); // addref'd or null
		const char *name = cur ? obs_source_get_name(cur) : nullptr;
		if (name && *name) {
			obs_data_set_string(canvasCurrent, def.uuid.c_str(), name);
		}
		OBSDataArrayAutoRelease order = obs_data_array_create();
		for (const std::string &uuid : SceneOrderToPersist(def.uuid)) {
			OBSDataAutoRelease item = obs_data_create();
			obs_data_set_string(item, "uuid", uuid.c_str());
			obs_data_array_push_back(order, item);
		}
		if (obs_data_array_count(order) > 0) {
			obs_data_set_array(canvasSceneOrder, def.uuid.c_str(), order);
		}
	}
	obs_data_set_obj(root, "canvas_current", canvasCurrent);
	obs_data_set_obj(root, "canvas_scene_order", canvasSceneOrder);

	// Persist the main-canvas scene list's user-defined order (uuids, reconciled
	// against the scenes just saved above) so it survives a restart -- libobs has
	// no scene-ordering primitive, so this array is the only record of it.
	OBSDataArrayAutoRelease sceneOrder = obs_data_array_create();
	for (const std::string &uuid : SceneOrder(std::string())) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "uuid", uuid.c_str());
		obs_data_array_push_back(sceneOrder, item);
	}
	obs_data_set_array(root, "scene_order", sceneOrder);

	ReportSaveResult(SaveJsonAtomic(root, path), path);
}

bool Load()
{
	return Load(ObsBootstrap::SceneCollections().ActiveScenePath());
}

bool Load(const std::string &path)
{
	// Reset first: a collection switch loads a different file, and a stale order
	// from the outgoing collection must never leak into the incoming one.
	g_sceneOrder.clear();

	OBSDataAutoRelease root = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!root) {
		RestoreCanvasScenes({}); // never-saved collection: still seed empty additional canvases
		return false;
	}

	OBSDataArrayAutoRelease sources = obs_data_get_array(root, "sources");
	if (!sources || obs_data_array_count(sources) == 0) {
		RestoreCanvasScenes({}); // no main scenes, but additional canvases still need seeding
		return false;
	}

	obs_load_sources(sources, nullptr, nullptr);

	// Restore the saved scene order (uuids), then reconcile against what was
	// actually loaded -- drops uuids for scenes that failed to load and appends
	// any loaded scene the saved order didn't know about (e.g. an older save from
	// before scene_order existed), so the order is always consistent from here on.
	auto readOrder = [](obs_data_array_t *array, std::vector<std::string> &out) {
		const size_t count = obs_data_array_count(array);
		for (size_t i = 0; i < count; i++) {
			OBSDataAutoRelease item = obs_data_array_item(array, i);
			const char *uuid = obs_data_get_string(item, "uuid");
			if (uuid && *uuid) {
				out.push_back(uuid);
			}
		}
	};
	if (OBSDataArrayAutoRelease sceneOrder = obs_data_get_array(root, "scene_order")) {
		readOrder(sceneOrder, g_sceneOrder[std::string()]);
	}
	ReconcileSceneOrder(std::string());

	// Each additional canvas's scene order, keyed by canvas uuid (sibling of the
	// main "scene_order" array above). Left un-reconciled here: the canvases' scenes
	// are only fully in place once RestoreCanvasScenes below has seeded the empty
	// ones, and every read reconciles anyway. A key for a canvas that no longer
	// exists reconciles to empty on first read and is pruned by the next Save.
	OBSDataAutoRelease canvasSceneOrder = obs_data_get_obj(root, "canvas_scene_order");
	if (canvasSceneOrder) {
		for (obs_data_item_t *item = obs_data_first(canvasSceneOrder); item; obs_data_item_next(&item)) {
			const char *uuid = obs_data_item_get_name(item);
			OBSDataArrayAutoRelease array = obs_data_item_get_array(item);
			if (uuid && *uuid && array) {
				readOrder(array, g_sceneOrder[uuid]);
			}
		}
	}

	// Additional-canvas active scenes { canvas uuid -> scene name }, restored after
	// the main channel-0 bind below via RestoreCanvasScenes.
	std::map<std::string, std::string> canvasCurrent;
	OBSDataAutoRelease canvasCurrentObj = obs_data_get_obj(root, "canvas_current");
	if (canvasCurrentObj) {
		for (obs_data_item_t *item = obs_data_first(canvasCurrentObj); item; obs_data_item_next(&item)) {
			const char *uuid = obs_data_item_get_name(item);
			const char *scene = obs_data_item_get_string(item);
			if (uuid && *uuid && scene && *scene) {
				canvasCurrent[uuid] = scene;
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
		RestoreCanvasScenes(canvasCurrent); // main canvas had no scene, but additional canvases still restore
		return false;
	}

	MainChannel::Set(scene);
	HostLog(std::string("[scene] loaded collection, current='") + obs_source_get_name(scene) + "'");
	RestoreCanvasScenes(canvasCurrent);
	return true;
}

void ClearCurrent()
{
	// Build the same exclude context Save uses, so the remove boundary stays in
	// lockstep with the keep boundary (SaveFilter): the channel 1-6 global audio
	// sources are preserved; main-canvas scenes + plain inputs are removed here.
	// Additional-canvas scenes are removed too, by the explicit per-canvas sweep at
	// the end of this function -- obs_enum_scenes below is main-canvas-scoped and
	// never reaches them.
	SaveContext ctx;
	OBSSourceAutoRelease audioRefs[6];
	for (uint32_t ch = 1; ch <= 6; ch++) {
		audioRefs[ch - 1] = obs_get_output_source(ch); // addref'd; may be null
		ctx.audio[ch - 1] = audioRefs[ch - 1];
	}

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

	// obs_enum_scenes is main-canvas-scoped; also sweep each additional canvas's
	// scenes so a collection switch tears down ALL scene content symmetrically.
	for (const CanvasDefinition &def : ObsBootstrap::Canvases().Definitions()) {
		if (def.isDefault) {
			continue;
		}
		if (obs_canvas_t *canvas = ObsBootstrap::CanvasRuntime().Find(def.uuid)) {
			obs_canvas_set_channel(canvas, 0, nullptr); // drop the stale current before removing scenes
		}
		for (const CanvasRuntime::SceneInfo &s : ObsBootstrap::CanvasRuntime().Scenes(def.uuid)) {
			OBSSourceAutoRelease scene = obs_get_source_by_uuid(s.uuid.c_str());
			if (scene) {
				obs_source_remove(scene);
			}
		}
	}

	// Deferred source destruction can cascade across the destruction-task thread;
	// drain in a loop until no more work is spawned, mirroring shutdown's teardown.
	while (obs_wait_for_destroy_queue()) {
	}
}

} // namespace SceneCollection
