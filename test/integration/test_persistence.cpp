// Headless persistence round-trip tests for the fixes that just landed. Each test
// drives the REAL SceneCollection::Save/Load and the REAL multistream stores, so
// each would FAIL if its guarded fix were reverted (noted per test).

#include "harness.hpp"

// cmocka requires these in this order before cmocka.h. Wrap cmocka.h in extern "C"
// so its symbols get C linkage in this C++ TU (its own guard does not apply here).
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
extern "C" {
#include <cmocka.h>
}

#include "scene_persistence.hpp"

#include "multistream/StorePaths.hpp"

#include <obs.h>
#include <obs.hpp>

#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace {

std::string SceneUuid(obs_source_t *scene)
{
	const char *u = obs_source_get_uuid(scene);
	return u ? std::string(u) : std::string();
}

// The set of source "name" values in a saved collection file, read straight from
// the on-disk JSON (so a test can assert what Save actually persisted).
std::set<std::string> SavedSourceNames(const std::string &path)
{
	std::set<std::string> names;
	OBSDataAutoRelease root = obs_data_create_from_json_file(path.c_str());
	if (!root) {
		return names;
	}
	OBSDataArrayAutoRelease arr = obs_data_get_array(root, "sources");
	const size_t n = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < n; i++) {
		OBSDataAutoRelease item = obs_data_array_item(arr, i);
		const char *nm = obs_data_get_string(item, "name");
		if (nm && *nm) {
			names.insert(nm);
		}
	}
	return names;
}

struct ItemScan {
	int count = 0;
	obs_sceneitem_t *first = nullptr;
};

bool CountItemsCb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *scan = static_cast<ItemScan *>(param);
	scan->count++;
	if (!scan->first) {
		scan->first = item;
	}
	return true;
}

bool CanvasHasSceneNamed(const std::string &canvasUuid, const char *name)
{
	for (const std::string &sceneName : Harness::CanvasSceneNames(canvasUuid)) {
		if (sceneName == name) {
			return true;
		}
	}
	return false;
}

} // namespace

// --- Fix 1: scene order ------------------------------------------------------
//
// Guards scene_persistence.cpp writing/reading the "scene_order" array. Revert it
// (Save omits scene_order, Load ignores it) and reload falls back to creation
// order [A,B,C], so the [C,A,B] assertion fails.
static void test_scene_order_roundtrip(void **)
{
	obs_source_t *a = Harness::CreateMainScene("A"); // borrowed; harness owns
	obs_source_t *b = Harness::CreateMainScene("B");
	obs_source_t *c = Harness::CreateMainScene("C");
	assert_non_null(a);
	assert_non_null(b);
	assert_non_null(c);

	const std::string ua = SceneUuid(a);
	const std::string ub = SceneUuid(b);
	const std::string uc = SceneUuid(c);

	// Creation order is [A,B,C]; move C to the front -> [C,A,B].
	assert_true(SceneCollection::ReorderScene(uc, "up"));
	assert_true(SceneCollection::ReorderScene(uc, "up"));

	const std::vector<std::string> expected = {uc, ua, ub};
	assert_true(SceneCollection::SceneOrder() == expected);

	Harness::Save();
	Harness::TeardownWorld();
	assert_null(obs_get_source_by_name("A")); // world really cleared before reload

	assert_true(Harness::Load());

	// The user-defined order survived a full teardown + reload.
	assert_true(SceneCollection::SceneOrder() == expected);
	OBSSourceAutoRelease ra = obs_get_source_by_name("A");
	OBSSourceAutoRelease rb = obs_get_source_by_name("B");
	OBSSourceAutoRelease rc = obs_get_source_by_name("C");
	assert_non_null(ra.Get());
	assert_non_null(rb.Get());
	assert_non_null(rc.Get());
}

// --- Fix 1 (degrade): old collection with no scene_order ---------------------
//
// A pre-fix collection file has no "scene_order" key. Load must reconcile to
// creation order and drop no scenes.
static void test_scene_order_legacy_missing_key(void **)
{
	assert_non_null(Harness::CreateMainScene("A"));
	assert_non_null(Harness::CreateMainScene("B"));
	assert_non_null(Harness::CreateMainScene("C"));

	Harness::Save();

	// Strip scene_order from the on-disk file to mimic a pre-fix save, exercising
	// Load's absent-key branch against the REAL file format.
	{
		OBSDataAutoRelease root = obs_data_create_from_json_file_safe(Harness::ScenePath().c_str(), "bak");
		assert_non_null(root.Get());
		obs_data_erase(root, "scene_order");
		assert_true(SaveJsonAtomic(root, Harness::ScenePath()));
	}

	Harness::TeardownWorld();
	assert_true(Harness::Load());

	// No scene vanished; the order self-heals to creation order (all three present).
	assert_int_equal(static_cast<int>(SceneCollection::SceneOrder().size()), 3);
	OBSSourceAutoRelease ra = obs_get_source_by_name("A");
	OBSSourceAutoRelease rb = obs_get_source_by_name("B");
	OBSSourceAutoRelease rc = obs_get_source_by_name("C");
	assert_non_null(ra.Get());
	assert_non_null(rb.Get());
	assert_non_null(rc.Get());
}

// --- Fix 2: audio state save + global-channel exclusion ----------------------
//
// PersistSourceState routes a scene source's audio edit to SceneCollection::Save()
// but a global audio channel (1..6) to GlobalAudioChannels instead -- SaveFilter
// EXCLUDES the channel sources from the collection. This asserts both boundaries:
// a scene source's mute/volume round-trips, and a channel-1 source is not written
// into the collection. Revert the audio persistence and the mute/volume assertion
// fails; revert the SaveFilter channel exclusion and the exclusion assertion fails.
static void test_audio_state_roundtrip(void **)
{
	obs_source_t *audio = Harness::CreateMainScene("Audio"); // borrowed; harness owns
	assert_non_null(audio);
	obs_source_set_muted(audio, true);
	obs_source_set_volume(audio, 0.25f);

	// Bind a source to a global audio channel; SaveFilter must drop it.
	obs_source_t *chan = Harness::CreateMainScene("Chan1");
	assert_non_null(chan);
	obs_set_output_source(1, chan);

	Harness::Save();

	const std::set<std::string> saved = SavedSourceNames(Harness::ScenePath());
	assert_true(saved.count("Audio") == 1); // scene source persisted
	assert_true(saved.count("Chan1") == 0); // global-channel source excluded

	// Unbind channel 1 before teardown so ClearCurrent (which is channel-exempt)
	// no longer sees Chan1 as a protected channel source and cleans it up.
	obs_set_output_source(1, nullptr);
	Harness::TeardownWorld();
	assert_true(Harness::Load());

	OBSSourceAutoRelease restored = obs_get_source_by_name("Audio");
	assert_non_null(restored.Get());
	assert_true(obs_source_muted(restored));
	assert_true(std::fabs(obs_source_get_volume(restored) - 0.25f) < 1e-4f);
}

// --- Fix 3: additional-canvas scene item -------------------------------------
//
// SaveFilter now KEEPS additional-canvas-scoped sources and PersistSourceState no
// longer skips the save for additional-canvas mutations. Create a non-default
// canvas, a scene on it holding one (hidden) item, save, tear the scene world
// down (the canvas survives), reload. Revert the SaveFilter change and the canvas
// scene is not written -> Load seeds a fresh empty scene instead, so the "CScene"
// + item assertions fail.
static void test_additional_canvas_item_roundtrip(void **)
{
	// The item's source: a plain (nested) scene, so the slice needs no plugins.
	obs_source_t *itemSrc = Harness::CreateMainScene("ItemSrc"); // borrowed; harness owns
	assert_non_null(itemSrc);

	const std::string canvasUuid = Harness::AddCanvas(1280, 720);
	assert_true(!canvasUuid.empty());

	obs_source_t *canvasScene = Harness::CreateCanvasScene(canvasUuid, "CScene");
	assert_non_null(canvasScene);

	obs_scene_t *scene = obs_scene_from_source(canvasScene);
	assert_non_null(scene);
	obs_sceneitem_t *item = obs_scene_add(scene, itemSrc);
	assert_non_null(item);
	obs_sceneitem_set_visible(item, false); // the mutation we expect to persist

	Harness::Save();
	Harness::TeardownWorld();
	// Canvas scenes live in the canvas's OWN namespace (obs_get_source_by_name never
	// sees them), so verify the teardown via the canvas API.
	assert_false(CanvasHasSceneNamed(canvasUuid, "CScene")); // scene world cleared, canvas kept

	assert_true(Harness::Load());

	// The additional canvas's scene round-tripped back into the canvas.
	assert_true(CanvasHasSceneNamed(canvasUuid, "CScene"));

	// Fetch it via the canvas namespace, then prove its hidden ITEM survived too --
	// the actual additional-canvas content the fix must persist.
	OBSSourceAutoRelease reloaded = Harness::CanvasSceneSource(canvasUuid, "CScene");
	assert_non_null(reloaded.Get());
	obs_scene_t *reloadedScene = obs_scene_from_source(reloaded);
	assert_non_null(reloadedScene);

	ItemScan scan;
	obs_scene_enum_items(reloadedScene, CountItemsCb, &scan);
	assert_int_equal(scan.count, 1);
	assert_non_null(scan.first);
	assert_false(obs_sceneitem_visible(scan.first)); // visibility mutation persisted
	obs_source_t *itemSource = obs_sceneitem_get_source(scan.first);
	assert_string_equal(obs_source_get_name(itemSource), "ItemSrc");
}

// --- fixtures ----------------------------------------------------------------

static int group_setup(void **)
{
	return Harness::BootObs() ? 0 : -1;
}

static int group_teardown(void **)
{
	Harness::ShutdownObs();
	return 0;
}

static int test_setup(void **)
{
	Harness::BeginTest();
	return 0;
}

static int test_teardown(void **)
{
	Harness::EndTest();
	return 0;
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup_teardown(test_scene_order_roundtrip, test_setup, test_teardown),
		cmocka_unit_test_setup_teardown(test_scene_order_legacy_missing_key, test_setup, test_teardown),
		cmocka_unit_test_setup_teardown(test_audio_state_roundtrip, test_setup, test_teardown),
		cmocka_unit_test_setup_teardown(test_additional_canvas_item_roundtrip, test_setup, test_teardown),
	};
	return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
