#include "scene_collections.hpp"

#include "bridge.hpp"
#include "log.hpp"
#include "multistream/MultistreamEngine.hpp"
#include "multistream/OutputBindingStore.hpp"
#include "multistream/SceneLinkStore.hpp"
#include "multistream/StorePaths.hpp"
#include "obs_bootstrap.hpp"
#include "scene_persistence.hpp"
#include "transitions.hpp"

#include <obs.h>
#include <obs.hpp>
#include <util/platform.h>
#include <util/util.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace {

std::string NewUuid()
{
	BPtr<char> id = os_generate_uuid();
	return std::string(id.Get());
}

// Lowercase, collapse non-alphanumeric runs to single hyphens, trim leading/
// trailing hyphens. Empty result falls back to "collection".
std::string Slugify(const std::string &name)
{
	std::string slug;
	slug.reserve(name.size());
	bool lastHyphen = false;
	for (unsigned char c : name) {
		if (std::isalnum(c)) {
			slug.push_back(char(std::tolower(c)));
			lastHyphen = false;
		} else if (!lastHyphen) {
			slug.push_back('-');
			lastHyphen = true;
		}
	}
	while (!slug.empty() && slug.front() == '-') {
		slug.erase(slug.begin());
	}
	while (!slug.empty() && slug.back() == '-') {
		slug.pop_back();
	}
	return slug.empty() ? std::string("collection") : slug;
}

// The output-bindings file is a sibling of the scene file: strip a trailing
// ".json" and append ".output_bindings.json" (scenes/<slug>.json ->
// scenes/<slug>.output_bindings.json). Derived from sceneFile so no index
// migration is needed when bindings became per-collection.
std::string BindingsRelFor(const std::string &sceneRel)
{
	const std::string suffix = ".json";
	std::string base = sceneRel;
	if (base.size() >= suffix.size() && base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0) {
		base.erase(base.size() - suffix.size());
	}
	return base + ".output_bindings.json";
}

// The scene-links file is a sibling of the scene file: strip a trailing ".json"
// and append ".scene_links.json" (scenes/<slug>.json ->
// scenes/<slug>.scene_links.json). Derived from sceneFile so no index migration
// is needed -- mirrors BindingsRelFor exactly.
std::string SceneLinksRelFor(const std::string &sceneRel)
{
	const std::string suffix = ".json";
	std::string base = sceneRel;
	if (base.size() >= suffix.size() && base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0) {
		base.erase(base.size() - suffix.size());
	}
	return base + ".scene_links.json";
}

} // namespace

std::string SceneCollections::IndexPath()
{
	return MultistreamBasicPath("scene_collections.json");
}

const SceneCollectionRecord *SceneCollections::Active() const
{
	for (const SceneCollectionRecord &c : collections_) {
		if (c.id == activeId_) {
			return &c;
		}
	}
	return collections_.empty() ? nullptr : &collections_.front();
}

std::string SceneCollections::ActiveScenePath() const
{
	const SceneCollectionRecord *active = Active();
	return MultistreamBasicPath(active ? active->sceneFile.c_str() : "scene_collection.json");
}

std::string SceneCollections::ActiveBindingsPath() const
{
	const SceneCollectionRecord *active = Active();
	const std::string sceneRel = active ? active->sceneFile : std::string("scene_collection.json");
	return MultistreamBasicPath(BindingsRelFor(sceneRel).c_str());
}

std::string SceneCollections::ActiveSceneLinksPath() const
{
	const SceneCollectionRecord *active = Active();
	const std::string sceneRel = active ? active->sceneFile : std::string("scene_collection.json");
	return MultistreamBasicPath(SceneLinksRelFor(sceneRel).c_str());
}

std::string SceneCollections::UniqueSceneFileForName(const std::string &name) const
{
	const std::string slug = Slugify(name);
	for (int suffix = 0;; ++suffix) {
		std::string rel = "scenes/" + slug + (suffix ? "-" + std::to_string(suffix + 1) : "") + ".json";

		bool clashes = false;
		for (const SceneCollectionRecord &c : collections_) {
			if (c.sceneFile == rel) {
				clashes = true;
				break;
			}
		}
		if (!clashes && os_file_exists(MultistreamBasicPath(rel.c_str()).c_str())) {
			clashes = true;
		}
		if (!clashes) {
			return rel;
		}
	}
}

void SceneCollections::Load()
{
	collections_.clear();
	activeId_.clear();
	indexCorrupt_ = false;

	const std::string path = IndexPath();
	OBSDataAutoRelease root = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!root) {
		// A genuinely-absent index is first run -> the caller seeds. A present-but-
		// unparseable one (its .bak also failed) must NOT be re-seeded: a fresh seed
		// would drop the index's references to existing scenes/*.json. Flag it so the
		// caller skips the seed and the on-disk collections are preserved for repair.
		if (os_file_exists(path.c_str()) || os_file_exists((path + ".bak").c_str())) {
			indexCorrupt_ = true;
			HostLog("[scene] scene_collections.json present but unreadable; refusing to "
				"re-seed (existing scene files preserved on disk)");
		}
		return;
	}

	OBSDataArrayAutoRelease arr = obs_data_get_array(root, "collections");
	const size_t count = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease item = obs_data_array_item(arr, i);
		SceneCollectionRecord rec;
		rec.id = obs_data_get_string(item, "id");
		rec.name = obs_data_get_string(item, "name");
		rec.sceneFile = obs_data_get_string(item, "sceneFile");
		if (!rec.id.empty() && !rec.sceneFile.empty()) {
			collections_.push_back(std::move(rec));
		}
	}

	activeId_ = obs_data_get_string(root, "active");
	// Repair a dangling/absent active pointer so Active() is always meaningful.
	if (!collections_.empty()) {
		bool found = false;
		for (const SceneCollectionRecord &c : collections_) {
			if (c.id == activeId_) {
				found = true;
				break;
			}
		}
		if (!found) {
			activeId_ = collections_.front().id;
		}
	}
}

void SceneCollections::Save() const
{
	OBSDataAutoRelease root = obs_data_create();
	OBSDataArrayAutoRelease arr = obs_data_array_create();
	for (const SceneCollectionRecord &c : collections_) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "id", c.id.c_str());
		obs_data_set_string(item, "name", c.name.c_str());
		obs_data_set_string(item, "sceneFile", c.sceneFile.c_str());
		obs_data_array_push_back(arr, item);
	}
	obs_data_set_array(root, "collections", arr);
	obs_data_set_string(root, "active", activeId_.c_str());

	const std::string path = IndexPath();
	std::filesystem::path dir = std::filesystem::u8path(path).parent_path();
	os_mkdirs(dir.u8string().c_str());

	if (!obs_data_save_json_pretty_safe(root, path.c_str(), "tmp", "bak")) {
		HostLog("[scene] failed to save scene-collection index to " + path);
	}
}

const SceneCollectionRecord &SceneCollections::SeedExisting(const std::string &name, const std::string &existingRelFile)
{
	SceneCollectionRecord rec;
	rec.id = NewUuid();
	rec.name = name;
	rec.sceneFile = existingRelFile;
	collections_.push_back(std::move(rec));
	activeId_ = collections_.back().id;
	Save();
	return collections_.back();
}

const SceneCollectionRecord &SceneCollections::Create(const std::string &name)
{
	SceneCollectionRecord rec;
	rec.id = NewUuid();
	rec.name = name;
	rec.sceneFile = UniqueSceneFileForName(name);
	collections_.push_back(std::move(rec));
	Save();
	return collections_.back();
}

bool SceneCollections::Rename(const std::string &id, const std::string &name)
{
	for (SceneCollectionRecord &c : collections_) {
		if (c.id == id) {
			c.name = name;
			Save();
			return true;
		}
	}
	return false;
}

const SceneCollectionRecord *SceneCollections::Duplicate(const std::string &sourceId, const std::string &name,
							 std::string &error)
{
	auto src = std::find_if(collections_.begin(), collections_.end(),
				[&](const SceneCollectionRecord &c) { return c.id == sourceId; });
	if (src == collections_.end()) {
		error = "no scene collection with id '" + sourceId + "'";
		return nullptr;
	}

	// Capture source rel paths before Create() mutates collections_ (which can
	// reallocate the vector and invalidate `src`).
	const std::string srcSceneRel = src->sceneFile;
	const std::string srcBindingsRel = BindingsRelFor(srcSceneRel);
	const std::string srcLinksRel = SceneLinksRelFor(srcSceneRel);

	// The active collection's scenes/bindings/links live in memory and may be dirty;
	// flush them to disk so the copy captures the current state (mirrors Switch's
	// save-before-teardown). Resolved while activeId_ still names the source so the
	// Active*Path() helpers address its files.
	if (sourceId == activeId_) {
		SceneCollection::Save(ActiveScenePath());
		ObsBootstrap::OutputBindings().Save(ActiveBindingsPath());
		ObsBootstrap::SceneLinks().Save(ActiveSceneLinksPath());
	}

	// Mint the new record (fresh uuid + unique scenes/<slug>.json) and persist the
	// index, without switching the active collection. `src` is invalidated after this.
	const SceneCollectionRecord &added = Create(name);
	const std::string dstSceneRel = added.sceneFile;

	// Byte-copy the scene file and each present sibling to the new slug's paths. The
	// new slug is unique, so the destinations are guaranteed absent (os_copyfile
	// refuses to overwrite). An absent source sibling is simply skipped.
	auto copyRel = [](const std::string &fromRel, const std::string &toRel) {
		const std::string from = MultistreamBasicPath(fromRel.c_str());
		const std::string to = MultistreamBasicPath(toRel.c_str());
		if (!os_file_exists(from.c_str())) {
			return;
		}
		std::filesystem::path dir = std::filesystem::u8path(to).parent_path();
		os_mkdirs(dir.u8string().c_str());
		if (os_copyfile(from.c_str(), to.c_str()) != 0) {
			HostLog("[scene] duplicate: failed to copy " + from + " -> " + to);
		}
	};
	copyRel(srcSceneRel, dstSceneRel);
	copyRel(srcBindingsRel, BindingsRelFor(dstSceneRel));
	copyRel(srcLinksRel, SceneLinksRelFor(dstSceneRel));

	HostLog("[scene] duplicated collection '" + name + "' file=" + MultistreamBasicPath(dstSceneRel.c_str()));
	return &added;
}

bool SceneCollections::Switch(const std::string &id, std::string &error)
{
	if (id == activeId_) {
		return true; // already active; nothing to swap
	}

	auto it = std::find_if(collections_.begin(), collections_.end(),
			       [&](const SceneCollectionRecord &c) { return c.id == id; });
	if (it == collections_.end()) {
		error = "no scene collection with id '" + id + "'";
		return false;
	}

	// The scene world backs every canvas's live encoder; swapping it under a running
	// output would free sources out from under the encoder (UAF). Refuse while live.
	if (ObsBootstrap::Multistream().AnyLive()) {
		error = "cannot switch scene collection while live";
		return false;
	}

	// Persist the outgoing collection's scenes + its output bindings (per-collection)
	// before tearing them down. Done while activeId_ still points at the outgoing
	// collection so both paths resolve to its files.
	SceneCollection::Save(ActiveScenePath());
	ObsBootstrap::OutputBindings().Save(ActiveBindingsPath());
	ObsBootstrap::SceneLinks().Save(ActiveSceneLinksPath());

	// Tear the outgoing scene world down leak-safely: unbind + destroy the channel-0
	// transition (releasing its wrapped scene), release the boot placeholder scene if
	// one is tracked (g_scene; no-op on the Load path), then remove the collection's
	// scenes + inputs and drain the destroy queue.
	Transitions::Shutdown();
	ObsBootstrap::TeardownScene();
	SceneCollection::ClearCurrent();

	// Commit the new active pointer + persist the index before loading, so
	// ActiveScenePath() resolves the target file.
	activeId_ = id;
	Save();

	// Load the target collection's scenes; a never-saved collection (no file yet)
	// comes up with a fresh placeholder Default scene instead of an empty world.
	if (!SceneCollection::Load(ActiveScenePath())) {
		ObsBootstrap::CreateDefaultSceneDetached();
	}

	// Re-wrap the now-current scene on channel 0 with the program transition, exactly
	// as boot does after the initial Load.
	Transitions::Init();

	// Load the incoming collection's output bindings (per-collection). A missing file
	// (a never-saved or freshly-created collection has none) loads as empty -- no
	// crash; the preview-gating + Multistream dock simply see zero bindings. Done
	// after activeId_ flipped above so ActiveBindingsPath() resolves the target.
	ObsBootstrap::OutputBindings().Load(ActiveBindingsPath());
	ObsBootstrap::SceneLinks().Load(ActiveSceneLinksPath());

	// Re-apply scene links to the new collection's restored program scene so
	// "following" canvases come up on their linked scene rather than their own
	// saved current scene.
	{
		OBSSourceAutoRelease program = Transitions::GetProgramScene();
		if (program) {
			const char *pu = obs_source_get_uuid(program);
			if (pu) {
				ObsBootstrap::ApplyCanvasSceneLinks(pu);
			}
		}
	}

	// Resync every window: the active collection, its scene list, and the transition
	// (re-created above) all changed; the bindings swap re-decides preview-gating
	// (outputBinding.changed) and refreshes the Multistream dock (multistream.changed).
	Bridge::EmitEvent("collections.changed", nlohmann::json::object());
	Bridge::EmitEvent("scenes.changed", nlohmann::json{{"canvas", nullptr}});
	Bridge::EmitEvent("transitions.changed", nlohmann::json::object());
	Bridge::EmitEvent("outputBinding.changed", nlohmann::json::object());
	Bridge::EmitEvent("sceneLink.changed", nlohmann::json::object());
	Bridge::EmitMultistreamChanged();

	HostLog("[scene] switched to collection '" + it->name + "' file=" + ActiveScenePath());
	return true;
}

bool SceneCollections::Remove(const std::string &id, std::string &error)
{
	auto it = std::find_if(collections_.begin(), collections_.end(),
			       [&](const SceneCollectionRecord &c) { return c.id == id; });
	if (it == collections_.end()) {
		error = "no scene collection with id '" + id + "'";
		return false;
	}
	if (collections_.size() == 1) {
		error = "cannot remove the last remaining scene collection";
		return false;
	}

	// The active collection's scene world is live on channel 0; switch away before
	// deleting it. Switch propagates the while-live refusal, so a live output blocks
	// the removal too.
	if (id == activeId_) {
		std::string target;
		for (const SceneCollectionRecord &c : collections_) {
			if (c.id != id) {
				target = c.id;
				break;
			}
		}
		if (!Switch(target, error)) {
			return false;
		}
		// Switch persisted the index but left collections_ intact; re-find by id.
		it = std::find_if(collections_.begin(), collections_.end(),
				  [&](const SceneCollectionRecord &c) { return c.id == id; });
		if (it == collections_.end()) {
			error = "no scene collection with id '" + id + "'";
			return false;
		}
	}

	// Delete the per-collection scene file (best-effort; an absent file is fine).
	std::error_code ec;
	std::filesystem::remove(std::filesystem::u8path(MultistreamBasicPath(it->sceneFile.c_str())), ec);

	collections_.erase(it);
	Save();
	return true;
}
