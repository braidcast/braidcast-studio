#include "obs_importer.hpp"
#include "event_names.hpp"

#include "bridge.hpp"
#include "log.hpp"
#include "obs_bootstrap.hpp"
#include "preview_window.hpp"
#include "scene_collections.hpp"
#include "transitions.hpp"

#include "audio/AudioMonitor.hpp"
#include "multistream/CanvasStore.hpp"
#include "multistream/GlobalAudioChannels.hpp"
#include "multistream/MultistreamEngine.hpp"
#include "multistream/StorePaths.hpp"
#include "multistream/StreamProfileStore.hpp"

#include <obs.h>
#include <obs.hpp>
#include <util/config-file.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <array>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using nlohmann::json;

namespace ObsImporter {

namespace {

// ---- read-only obs-studio path helpers -------------------------------------
//
// Every function here only ever READS from the obs-studio tree. The single
// "write" primitive used by the importer (WriteForkSceneFile) targets the fork's
// own braidcast dir via MultistreamBasicPath, never an obs-studio path.

// Resolve the obs-studio config dir. Empty `path` -> os_get_config_path.
std::string ResolveBase(const std::string &path)
{
	if (!path.empty()) {
		return path;
	}
	char buf[512];
	if (os_get_config_path(buf, sizeof(buf), "obs-studio") <= 0) {
		return std::string();
	}
	return std::string(buf);
}

std::string ScenesDir(const std::string &base)
{
	return base + "/basic/scenes";
}

std::string ProfileFile(const std::string &base, const std::string &profileDir, const char *file)
{
	if (profileDir.empty()) {
		return std::string();
	}
	return base + "/basic/profiles/" + profileDir + "/" + file;
}

// Open an ini READ-ONLY. CONFIG_OPEN_EXISTING never creates or writes the file;
// the caller must config_close the returned handle. Null on absent/unreadable.
config_t *OpenIniReadOnly(const std::string &path)
{
	if (path.empty()) {
		return nullptr;
	}
	config_t *cfg = nullptr;
	if (config_open(&cfg, path.c_str(), CONFIG_OPEN_EXISTING) != CONFIG_SUCCESS) {
		return nullptr;
	}
	return cfg;
}

// The active profile's dir + display name, read from the root user.ini (OBS 31+)
// or, failing that, global.ini. Falls back to the first directory under
// basic/profiles when neither names a ProfileDir. `dir` empty => no profile.
void ActiveProfile(const std::string &base, std::string &dir, std::string &name)
{
	for (const char *fname : {"user.ini", "global.ini"}) {
		config_t *cfg = OpenIniReadOnly(base + "/" + fname);
		if (!cfg) {
			continue;
		}
		const char *d = config_get_string(cfg, "Basic", "ProfileDir");
		const char *n = config_get_string(cfg, "Basic", "Profile");
		if (d && *d) {
			dir = d;
		}
		if (n && *n) {
			name = n;
		}
		config_close(cfg);
		if (!dir.empty()) {
			return;
		}
	}

	// No active pointer: adopt the first profile dir present, so a single-profile
	// install (or one with a stale root ini) still resolves video/audio/service.
	os_dir_t *od = os_opendir((base + "/basic/profiles").c_str());
	if (od) {
		struct os_dirent *ent = nullptr;
		while ((ent = os_readdir(od)) != nullptr) {
			if (ent->directory && ent->d_name[0] != '.') {
				dir = ent->d_name;
				break;
			}
		}
		os_closedir(od);
	}
}

// ---- field mappings --------------------------------------------------------

// basic.ini [Video] FPSCommon stores the combo label, not an index. Resolve the
// standard labels (incl. the NTSC fractional rates) to a single num/den.
struct FpsCommon {
	const char *label;
	uint32_t num;
	uint32_t den;
};
const FpsCommon kFpsCommon[] = {
	{"10", 10, 1},
	{"20", 20, 1},
	{"24", 24, 1},
	{"24 NTSC", 24000, 1001},
	{"23.976", 24000, 1001},
	{"25", 25, 1},
	{"29.97", 30000, 1001},
	{"30", 30, 1},
	{"48", 48, 1},
	{"50", 50, 1},
	{"59.94", 60000, 1001},
	{"60", 60, 1},
	{"119.88", 120000, 1001},
	{"120", 120, 1},
	{"144", 144, 1},
	{"240", 240, 1},
};

void ResolveFpsCommon(const char *label, uint32_t &num, uint32_t &den)
{
	if (label && *label) {
		for (const FpsCommon &f : kFpsCommon) {
			if (astrcmpi(label, f.label) == 0) {
				num = f.num;
				den = f.den;
				return;
			}
		}
		// Unrecognized label: an integer like "75" maps to 75/1; otherwise default.
		const int v = atoi(label);
		if (v > 0) {
			num = uint32_t(v);
			den = 1;
			return;
		}
	}
	num = 30;
	den = 1;
}

struct VideoInfo {
	uint32_t baseW = 0;
	uint32_t baseH = 0;
	uint32_t outW = 0;
	uint32_t outH = 0;
	uint32_t fpsNum = 30;
	uint32_t fpsDen = 1;
};

// Read [Video] from the active profile's basic.ini READ-ONLY. Returns false (no
// video to import) when the ini/section is absent or carries no base resolution.
bool ParseVideo(const std::string &base, const std::string &profileDir, VideoInfo &out)
{
	config_t *cfg = OpenIniReadOnly(ProfileFile(base, profileDir, "basic.ini"));
	if (!cfg) {
		return false;
	}
	out.baseW = uint32_t(config_get_uint(cfg, "Video", "BaseCX"));
	out.baseH = uint32_t(config_get_uint(cfg, "Video", "BaseCY"));
	out.outW = uint32_t(config_get_uint(cfg, "Video", "OutputCX"));
	out.outH = uint32_t(config_get_uint(cfg, "Video", "OutputCY"));

	const uint64_t fpsType = config_get_uint(cfg, "Video", "FPSType");
	if (fpsType == 1) {
		out.fpsNum = uint32_t(config_get_uint(cfg, "Video", "FPSInt"));
		out.fpsDen = 1;
	} else if (fpsType == 2) {
		out.fpsNum = uint32_t(config_get_uint(cfg, "Video", "FPSNum"));
		out.fpsDen = uint32_t(config_get_uint(cfg, "Video", "FPSDen"));
	} else {
		ResolveFpsCommon(config_get_string(cfg, "Video", "FPSCommon"), out.fpsNum, out.fpsDen);
	}
	config_close(cfg);

	if (out.baseW == 0 || out.baseH == 0) {
		return false; // no usable [Video]
	}
	if (out.outW == 0 || out.outH == 0) {
		out.outW = out.baseW;
		out.outH = out.baseH;
	}
	if (out.fpsNum == 0) {
		out.fpsNum = 30;
		out.fpsDen = 1;
	}
	if (out.fpsDen == 0) {
		out.fpsDen = 1;
	}
	return true;
}

struct ChannelMap {
	const char *setup; // basic.ini ChannelSetup string
	speaker_layout layout;
};
const ChannelMap kChannelSetups[] = {
	{"Mono", SPEAKERS_MONO},   {"Stereo", SPEAKERS_STEREO}, {"2.1", SPEAKERS_2POINT1}, {"4.0", SPEAKERS_4POINT0},
	{"4.1", SPEAKERS_4POINT1}, {"5.1", SPEAKERS_5POINT1},   {"7.1", SPEAKERS_7POINT1},
};

struct AudioInfo {
	uint32_t sampleRate = 0;
	std::string channelSetup;
	speaker_layout layout = SPEAKERS_STEREO;
	bool layoutKnown = true;
};

// Read [Audio] from the active profile's basic.ini READ-ONLY. Returns false (no
// audio to import) when the ini/section is absent or carries no SampleRate.
bool ParseAudio(const std::string &base, const std::string &profileDir, AudioInfo &out)
{
	config_t *cfg = OpenIniReadOnly(ProfileFile(base, profileDir, "basic.ini"));
	if (!cfg) {
		return false;
	}
	out.sampleRate = uint32_t(config_get_uint(cfg, "Audio", "SampleRate"));
	const char *setup = config_get_string(cfg, "Audio", "ChannelSetup");
	out.channelSetup = (setup && *setup) ? setup : "Stereo";
	config_close(cfg);

	if (out.sampleRate == 0) {
		return false; // no usable [Audio]
	}
	out.layoutKnown = false;
	for (const ChannelMap &c : kChannelSetups) {
		if (astrcmpi(out.channelSetup.c_str(), c.setup) == 0) {
			out.layout = c.layout;
			out.layoutKnown = true;
			break;
		}
	}
	return true;
}

// Build a (move-only) StreamProfile from the active profile's service.json
// READ-ONLY. serviceId <- "type"; settings <- "settings" blob verbatim; label <-
// the OBS profile display name (gives a meaningful, dedup-friendly DisplayName).
// Returns false (present=false) when service.json is absent/unreadable.
bool LoadServiceProfile(const std::string &base, const std::string &profileDir, const std::string &profileName,
			StreamProfile &out)
{
	const std::string path = ProfileFile(base, profileDir, "service.json");
	if (path.empty()) {
		return false;
	}
	OBSDataAutoRelease root = obs_data_create_from_json_file(path.c_str()); // read-only
	if (!root) {
		return false;
	}
	const char *type = obs_data_get_string(root, "type");
	if (type && *type) {
		out.serviceId = type;
	}
	out.settings = obs_data_get_obj(root, "settings"); // owning ref or null
	out.label = profileName;                           // may be empty -> DisplayName == platform
	return true;
}

// Mirror of bridge StreamProfileConflicts: a clash is another profile sharing a
// non-empty stream key, OR a case-insensitive identical "{platform} - {label}"
// display name. Skips the candidate's own uuid (unused here; imports are fresh).
bool ProfileConflicts(const StreamProfile &candidate, std::string &clashName)
{
	const std::string candKey = candidate.Key();
	const std::string candName = candidate.DisplayName();
	for (const StreamProfile &other : ObsBootstrap::StreamProfiles().Profiles()) {
		if (!candKey.empty() && candKey == other.Key()) {
			clashName = other.DisplayName();
			return true;
		}
		if (astrcmpi(candName.c_str(), other.DisplayName().c_str()) == 0) {
			clashName = other.DisplayName();
			return true;
		}
	}
	return false;
}

// ---- scene collection reading + dependency closure -------------------------

bool HasJsonExt(const char *name)
{
	const size_t n = strlen(name);
	return n > 5 && astrcmpi(name + n - 5, ".json") == 0;
}

// The scene-type source names in a loaded collection, in file order. Used for the
// per-scene import UI. (Groups have id "group" and are intentionally excluded.)
json SceneNames(obs_data_array_t *sources)
{
	json names = json::array();
	const size_t count = sources ? obs_data_array_count(sources) : 0;
	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease src = obs_data_array_item(sources, i);
		const char *id = obs_data_get_string(src, "id");
		if (id && strcmp(id, "scene") == 0) {
			const char *name = obs_data_get_string(src, "name");
			names.push_back(name ? name : "");
		}
	}
	return names;
}

// Build the filtered "sources" array for a collection import. When `selected` is
// empty every source is kept (the original array, addref'd). Otherwise the result
// is the selected scenes plus their transitive DEPENDENCY CLOSURE: BFS over each
// scene/group's settings.items[].name, then emit the original source objects whose
// name was reached, preserving file order. Referenced-but-missing names are simply
// not emitted (a source can't be included if the collection doesn't define it).
OBSDataArrayAutoRelease BuildFilteredSources(obs_data_array_t *sources, const std::unordered_set<std::string> &selected)
{
	const size_t count = sources ? obs_data_array_count(sources) : 0;
	if (selected.empty()) {
		obs_data_array_addref(sources);
		return OBSDataArrayAutoRelease(sources);
	}

	std::vector<OBSDataAutoRelease> items;
	items.reserve(count);
	std::unordered_map<std::string, size_t> byName;
	for (size_t i = 0; i < count; i++) {
		items.emplace_back(obs_data_array_item(sources, i));
		const char *name = obs_data_get_string(items.back(), "name");
		if (name && *name) {
			byName.emplace(name, i); // first definition wins on duplicate names
		}
	}

	std::unordered_set<std::string> visited;
	std::queue<std::string> q;
	for (const std::string &s : selected) {
		q.push(s);
	}
	while (!q.empty()) {
		const std::string name = std::move(q.front());
		q.pop();
		if (visited.count(name)) {
			continue;
		}
		auto it = byName.find(name);
		if (it == byName.end()) {
			continue; // referenced but not defined here; nothing to include
		}
		visited.insert(name);

		OBSDataAutoRelease settings = obs_data_get_obj(items[it->second], "settings");
		if (!settings) {
			continue;
		}
		OBSDataArrayAutoRelease refItems = obs_data_get_array(settings, "items");
		const size_t refCount = refItems ? obs_data_array_count(refItems) : 0;
		for (size_t j = 0; j < refCount; j++) {
			OBSDataAutoRelease item = obs_data_array_item(refItems, j);
			const char *refName = obs_data_get_string(item, "name");
			if (refName && *refName) {
				q.push(refName);
			}
		}
	}

	OBSDataArrayAutoRelease out = obs_data_array_create();
	for (size_t i = 0; i < count; i++) {
		const char *name = obs_data_get_string(items[i], "name");
		if (name && visited.count(name)) {
			obs_data_array_push_back(out, items[i]);
		}
	}
	return out;
}

// Pick the current scene to seed the fork file with: the OBS collection's saved
// current scene when it survived the filter, otherwise the first scene-type source
// in the filtered set, otherwise empty.
std::string PickCurrentScene(obs_data_t *root, obs_data_array_t *filtered)
{
	const size_t count = filtered ? obs_data_array_count(filtered) : 0;
	auto presentScene = [&](const char *want) -> bool {
		if (!want || !*want) {
			return false;
		}
		for (size_t i = 0; i < count; i++) {
			OBSDataAutoRelease src = obs_data_array_item(filtered, i);
			const char *id = obs_data_get_string(src, "id");
			const char *name = obs_data_get_string(src, "name");
			if (id && strcmp(id, "scene") == 0 && name && strcmp(name, want) == 0) {
				return true;
			}
		}
		return false;
	};

	for (const char *key : {"current_scene", "current_program_scene"}) {
		const char *want = obs_data_get_string(root, key);
		if (presentScene(want)) {
			return want;
		}
	}
	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease src = obs_data_array_item(filtered, i);
		const char *id = obs_data_get_string(src, "id");
		if (id && strcmp(id, "scene") == 0) {
			const char *name = obs_data_get_string(src, "name");
			if (name && *name) {
				return name;
			}
		}
	}
	return std::string();
}

// Warn for any source type in the filtered set that this build can't instantiate
// (plugin not present). The collection still imports; those sources load as
// missing, exactly as stock OBS would surface them.
void WarnUnsupportedTypes(obs_data_array_t *filtered, const std::string &collectionName, json &warnings)
{
	std::unordered_set<std::string> reported;
	const size_t count = filtered ? obs_data_array_count(filtered) : 0;
	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease src = obs_data_array_item(filtered, i);
		const char *id = obs_data_get_string(src, "id");
		if (!id || !*id || strcmp(id, "scene") == 0 || strcmp(id, "group") == 0) {
			continue;
		}
		if (reported.count(id)) {
			continue;
		}
		if (!obs_source_get_display_name(id)) {
			reported.insert(id);
			warnings.push_back("collection '" + collectionName + "': source type '" + std::string(id) +
					   "' is not available; it will load as missing");
		}
	}
}

// Write the new fork collection's scene file in the FORK format (the exact shape
// scene_persistence reads: {"sources":[...],"current_scene":"..."}). Targets the
// fork's braidcast dir only, via MultistreamBasicPath.
bool WriteForkSceneFile(const std::string &relFile, obs_data_array_t *filtered, const std::string &currentScene)
{
	OBSDataAutoRelease root = obs_data_create();
	obs_data_set_array(root, "sources", filtered);
	obs_data_set_string(root, "current_scene", currentScene.c_str());

	return SaveJsonAtomic(root, MultistreamBasicPath(relFile.c_str()));
}

// A fork collection name unique against the current registry: the requested name,
// else "<name> (Imported)", else "<name> (Imported N)".
std::string UniqueCollectionName(const std::string &name)
{
	auto clashes = [](const std::string &cand) {
		for (const SceneCollectionRecord &c : ObsBootstrap::SceneCollections().List()) {
			if (c.name == cand) {
				return true;
			}
		}
		return false;
	};
	if (!clashes(name)) {
		return name;
	}
	std::string cand = name + " (Imported)";
	if (!clashes(cand)) {
		return cand;
	}
	for (int n = 2;; ++n) {
		cand = name + " (Imported " + std::to_string(n) + ")";
		if (!clashes(cand)) {
			return cand;
		}
	}
}

// ---- video / audio apply (mirrors the settings bridge apply paths) ---------

// Apply imported [Video] to the global pipeline + the Default canvas def. Mirrors
// MethodCanvasUpdate's Default-canvas branch: reset video FIRST (rolling back on
// failure) so a failed reset leaves canvases.json untouched, then commit the def.
bool ApplyVideo(const VideoInfo &v, std::string &error)
{
	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi)) {
		error = "video not initialized";
		return false;
	}
	const obs_video_info previous = ovi;
	ovi.base_width = v.baseW;
	ovi.base_height = v.baseH;
	ovi.output_width = v.outW;
	ovi.output_height = v.outH;
	ovi.fps_num = v.fpsNum;
	ovi.fps_den = v.fpsDen;

	const bool changed = ovi.base_width != previous.base_width || ovi.base_height != previous.base_height ||
			     ovi.output_width != previous.output_width || ovi.output_height != previous.output_height ||
			     ovi.fps_num != previous.fps_num || ovi.fps_den != previous.fps_den;
	if (changed) {
		const int rv = obs_reset_video(&ovi);
		if (rv != OBS_VIDEO_SUCCESS) {
			obs_video_info restore = previous;
			obs_reset_video(&restore);
			error = "obs_reset_video failed (code " + std::to_string(rv) + ")";
			return false;
		}
		Preview::OnVideoReset();
		Transitions::OnVideoReset();
	}

	obs_video_info applied = {};
	obs_get_video_info(&applied);

	CanvasStore &canvases = ObsBootstrap::Canvases();
	if (CanvasDefinition *def = canvases.Find(canvases.Default().uuid)) {
		def->width = applied.base_width;
		def->height = applied.base_height;
		def->outputWidth = applied.output_width;
		def->outputHeight = applied.output_height;
		def->fpsNum = applied.fps_num;
		def->fpsDen = applied.fps_den;
		canvases.Save();
		// The cached encoder pair is bound to the old mix; drop it so a later
		// restart rebuilds against the new resolution (mirrors MethodCanvasUpdate).
		ObsBootstrap::Multistream().InvalidateCanvasEncoders(def->uuid);
	}
	return true;
}

// Apply imported [Audio] sample rate + channel layout. Mirrors MethodSettingsSetAudio's
// mix-reset branch (obs_reset_audio fails while audio is active; the live guard ran
// at the top of Import, so with no outputs this succeeds).
bool ApplyAudio(const AudioInfo &a, std::string &error)
{
	obs_audio_info oai = {};
	if (!obs_get_audio_info(&oai)) {
		error = "audio not initialized";
		return false;
	}
	if (a.sampleRate == 44100 || a.sampleRate == 48000) {
		oai.samples_per_sec = a.sampleRate;
	}
	oai.speakers = a.layout;
	if (!obs_reset_audio(&oai)) {
		error = "obs_reset_audio failed (audio may be active)";
		return false;
	}
	return true;
}

} // namespace

json Scan(const std::string &path)
{
	const std::string base = ResolveBase(path);
	json result = json{{"found", false}, {"path", ""}, {"collections", json::array()}};

	if (base.empty() || !os_file_exists(ScenesDir(base).c_str())) {
		return result;
	}
	result["found"] = true;
	result["path"] = base;

	// Collections: scan basic/scenes for *.json (skipping *.bak / *.v1 / non-json).
	json collections = json::array();
	os_dir_t *od = os_opendir(ScenesDir(base).c_str());
	if (od) {
		struct os_dirent *ent = nullptr;
		while ((ent = os_readdir(od)) != nullptr) {
			if (ent->directory || !HasJsonExt(ent->d_name)) {
				continue;
			}
			const std::string file = ent->d_name;
			OBSDataAutoRelease root =
				obs_data_create_from_json_file((ScenesDir(base) + "/" + file).c_str()); // read-only
			if (!root) {
				HostLog("[importer] skipping unreadable collection " + file);
				continue;
			}
			OBSDataArrayAutoRelease sources = obs_data_get_array(root, "sources");
			const char *name = obs_data_get_string(root, "name");
			std::string display = (name && *name) ? name : file;
			collections.push_back(json{{"name", display}, {"file", file}, {"scenes", SceneNames(sources)}});
		}
		os_closedir(od);
	}
	result["collections"] = std::move(collections);

	// Active profile -> service / video / audio.
	std::string profileDir, profileName;
	ActiveProfile(base, profileDir, profileName);

	StreamProfile svc;
	if (LoadServiceProfile(base, profileDir, profileName, svc)) {
		result["service"] = json{{"present", true}, {"label", svc.DisplayName()}};
	} else {
		result["service"] = nullptr;
	}

	VideoInfo v;
	if (ParseVideo(base, profileDir, v)) {
		result["video"] = json{{"baseWidth", v.baseW},
				       {"baseHeight", v.baseH},
				       {"outputWidth", v.outW},
				       {"outputHeight", v.outH},
				       {"fps", double(v.fpsNum) / double(v.fpsDen)}};
	} else {
		result["video"] = nullptr;
	}

	AudioInfo a;
	if (ParseAudio(base, profileDir, a)) {
		result["audio"] = json{{"sampleRate", a.sampleRate}, {"channels", a.channelSetup}};
	} else {
		result["audio"] = nullptr;
	}

	return result;
}

// Restore the global audio device sources (Desktop Audio / Mic-Aux) OBS stores at a
// scene-collection root as full obs_save_source blobs -- keyed DesktopAudioDevice1/2
// and AuxAudioDevice1-4 -- onto the fork's global output channels 1-6, filters and
// all. Returns how many channels were applied. Maps to the same channel layout as
// GlobalAudioChannels::Slots (desktop -> 1/2, mic -> 3/4/5/6).
int ApplyGlobalAudioDevices(obs_data_t *root)
{
	struct Map {
		const char *key;
		int channel;
	};
	static const std::array<Map, 6> kMap = {{
		{"DesktopAudioDevice1", 1},
		{"DesktopAudioDevice2", 2},
		{"AuxAudioDevice1", 3},
		{"AuxAudioDevice2", 4},
		{"AuxAudioDevice3", 5},
		{"AuxAudioDevice4", 6},
	}};

	int applied = 0;
	for (const Map &m : kMap) {
		if (!obs_data_has_user_value(root, m.key)) {
			continue;
		}
		OBSDataAutoRelease blob = obs_data_get_obj(root, m.key);
		if (!blob) {
			continue;
		}
		obs_source_t *src = obs_load_source(blob); // create-ref; restores the "filters" array
		if (!src) {
			continue;
		}
		obs_set_output_source(m.channel, src); // channel takes its own ref
		obs_source_release(src);               // drop the create-ref
		applied++;
	}
	return applied;
}

json Import(const json &params)
{
	// Refuse BEFORE any mutation while live: the scene world / video / audio mixes
	// back the running encoders and must not be swapped under them.
	if (ObsBootstrap::Multistream().AnyLive()) {
		return json{{"ok", false}, {"error", "cannot import while live"}};
	}
	if (ObsBootstrap::SceneCollections().IndexWasCorrupt()) {
		return json{{"ok", false}, {"error", "scene collection index is corrupt; cannot import"}};
	}

	const std::string base = ResolveBase(params.is_object() ? params.value("path", std::string()) : std::string());
	if (base.empty() || !os_file_exists(ScenesDir(base).c_str())) {
		return json{{"ok", false}, {"error", "no OBS Studio install found at the given path"}};
	}

	json warnings = json::array();
	int importedCollections = 0;

	// ---- scene collections (each -> a NEW fork collection) ----
	if (params.contains("collections") && params["collections"].is_array()) {
		for (const json &req : params["collections"]) {
			if (!req.is_object() || !req.contains("file") || !req["file"].is_string()) {
				continue;
			}
			const std::string file = req["file"].get<std::string>();
			OBSDataAutoRelease root =
				obs_data_create_from_json_file((ScenesDir(base) + "/" + file).c_str()); // read-only
			if (!root) {
				warnings.push_back("skipped collection '" + file + "': unreadable");
				continue;
			}
			OBSDataArrayAutoRelease sources = obs_data_get_array(root, "sources");
			if (!sources || obs_data_array_count(sources) == 0) {
				warnings.push_back("skipped collection '" + file + "': no sources");
				continue;
			}

			std::unordered_set<std::string> selected;
			if (req.contains("scenes") && req["scenes"].is_array()) {
				for (const json &s : req["scenes"]) {
					if (s.is_string()) {
						selected.insert(s.get<std::string>());
					}
				}
			}
			OBSDataArrayAutoRelease filtered = BuildFilteredSources(sources, selected);
			if (obs_data_array_count(filtered) == 0) {
				warnings.push_back("skipped collection '" + file +
						   "': selected scenes produced no sources");
				continue;
			}

			std::string display;
			if (req.contains("name") && req["name"].is_string() &&
			    !req["name"].get<std::string>().empty()) {
				display = req["name"].get<std::string>();
			} else {
				const char *nm = obs_data_get_string(root, "name");
				display = (nm && *nm) ? nm : file;
			}
			const std::string finalName = UniqueCollectionName(display);

			WarnUnsupportedTypes(filtered, finalName, warnings);

			// Create the registry record, then copy its rel path immediately -- the
			// next Create() may reallocate the vector and invalidate the reference.
			const std::string relFile = ObsBootstrap::SceneCollections().Create(finalName).sceneFile;
			const std::string currentScene = PickCurrentScene(root, filtered);
			if (!WriteForkSceneFile(relFile, filtered, currentScene)) {
				warnings.push_back("collection '" + finalName + "': failed to write scene file");
				continue;
			}
			importedCollections++;
		}
	}

	std::string profileDir, profileName;
	ActiveProfile(base, profileDir, profileName);

	// ---- stream service -> a NEW global stream profile ----
	bool serviceImported = false;
	if (params.value("importService", false)) {
		StreamProfile svc;
		if (LoadServiceProfile(base, profileDir, profileName, svc)) {
			std::string clash;
			if (ProfileConflicts(svc, clash)) {
				warnings.push_back("skipped stream profile '" + svc.DisplayName() +
						   "': already exists as '" + clash + "'");
			} else {
				StreamProfileStore &store = ObsBootstrap::StreamProfiles();
				store.Add(std::move(svc));
				store.Save();
				serviceImported = true;
			}
		} else {
			warnings.push_back("no stream service found to import");
		}
	}

	// ---- video -> Default canvas + global pipeline ----
	bool videoImported = false;
	if (params.value("importVideo", false)) {
		VideoInfo v;
		if (ParseVideo(base, profileDir, v)) {
			std::string err;
			if (ApplyVideo(v, err)) {
				videoImported = true;
			} else {
				warnings.push_back("video not imported: " + err);
			}
		} else {
			warnings.push_back("no video settings found to import");
		}
	}

	// ---- audio -> global mix ----
	bool audioImported = false;
	if (params.value("importAudio", false)) {
		AudioInfo a;
		if (ParseAudio(base, profileDir, a)) {
			if (!a.layoutKnown) {
				warnings.push_back("unknown channel setup '" + a.channelSetup +
						   "'; defaulted to stereo");
			}
			std::string err;
			if (ApplyAudio(a, err)) {
				audioImported = true;
			} else {
				warnings.push_back("audio not imported: " + err);
			}
		} else {
			warnings.push_back("no audio settings found to import");
		}
	}

	// ---- global audio devices (Desktop/Mic sources + their FILTERS) -> shared mix ----
	// OBS stores these at each scene-collection root as full obs_save_source blobs; the
	// fork keeps ONE shared global set, so import from a single collection (the first with
	// device data wins, deterministically) and persist it. This is the path that recovers
	// the filters/volume/mute the fork's own persistence never captured before.
	bool globalAudioImported = false;
	if (params.value("importGlobalAudio", false) && params.contains("collections") &&
	    params["collections"].is_array()) {
		for (const json &req : params["collections"]) {
			if (!req.is_object() || !req.contains("file") || !req["file"].is_string()) {
				continue;
			}
			const std::string file = req["file"].get<std::string>();
			OBSDataAutoRelease root =
				obs_data_create_from_json_file((ScenesDir(base) + "/" + file).c_str()); // read-only
			if (!root) {
				continue;
			}
			if (ApplyGlobalAudioDevices(root) > 0) {
				globalAudioImported = true;
				break; // first collection with global audio wins; the fork has one shared set
			}
		}
		if (globalAudioImported) {
			ObsBootstrap::GlobalAudioChannels().Persist();
			ObsBootstrap::AudioMonitor().Rebuild();
		} else {
			warnings.push_back("no global audio devices found to import");
		}
	}

	// Emit only the events whose data actually changed so each window resyncs.
	if (importedCollections > 0) {
		Bridge::EmitEvent(EventNames::kCollectionsChanged, json::object());
	}
	if (serviceImported) {
		Bridge::EmitEvent(EventNames::kStreamProfileChanged, json::object());
	}
	if (videoImported) {
		Bridge::EmitEvent(EventNames::kCanvasChanged, json::object());
		obs_video_info applied = {};
		if (obs_get_video_info(&applied)) {
			Bridge::EmitEvent(EventNames::kSettingsVideoChanged,
					  json{{"baseWidth", applied.base_width},
					       {"baseHeight", applied.base_height},
					       {"outputWidth", applied.output_width},
					       {"outputHeight", applied.output_height},
					       {"fpsNum", applied.fps_num},
					       {"fpsDen", applied.fps_den}});
		}
	}
	if (audioImported) {
		Bridge::EmitEvent(EventNames::kSettingsAudioChanged, json::object());
	}
	if (globalAudioImported) {
		Bridge::EmitEvent(EventNames::kAudioChanged, json::object());
	}

	HostLog("[importer] imported collections=" + std::to_string(importedCollections) +
		" service=" + (serviceImported ? "1" : "0") + " video=" + (videoImported ? "1" : "0") +
		" audio=" + (audioImported ? "1" : "0") + " warnings=" + std::to_string(warnings.size()));

	return json{{"ok", true},
		    {"imported", json{{"collections", importedCollections},
				      {"service", serviceImported},
				      {"video", videoImported},
				      {"audio", audioImported}}},
		    {"warnings", std::move(warnings)}};
}

} // namespace ObsImporter
