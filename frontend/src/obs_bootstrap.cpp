#include "obs_bootstrap.hpp"

#include <obs.h>
#include <obs-frontend-internal.hpp>
#include <util/base.h>
#include <util/platform.h>

#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "audio/AudioMonitor.hpp"
#include "bridge.hpp"
#include "frontend_callbacks.hpp"
#include "log.hpp"
#include "multistream/CanvasRuntime.hpp"
#include "multistream/CanvasStore.hpp"
#include "multistream/Hotkeys.hpp"
#include "mcp/McpServer.hpp"
#include "multistream/MultistreamEngine.hpp"
#include "multistream/OutputBindingStore.hpp"
#include "multistream/SceneLinkStore.hpp"
#include "multistream/StreamProfileStore.hpp"
#include "multistream/VirtualCamManager.hpp"
#include "AdvancedSettings.hpp"
#include "GeneralSettings.hpp"
#include "paths.hpp"
#include "preview_window.hpp"
#include "projector_window.hpp"
#include "scene_collections.hpp"
#include "scene_persistence.hpp"
#include "transitions.hpp"
#include "UndoManager.hpp"

namespace {

// Qt-frontend UI helpers we never want headless. obs-websocket is a FORCED
// exclusion: spike 4.0b proved it constructs a QWidget at obs_module_load with no
// QApplication present -> instant STATUS_STACK_BUFFER_OVERRUN. The rest are pure
// Qt UI plugins with no headless value.
const std::set<std::string> kDenylist = {
	"frontend-tools", "decklink-output-ui", "decklink-captions", "aja-output-ui", "obs-websocket",
};

// Non-module helper DLLs that share the plugin dir (CEF runtime + obs-browser's
// render-helper). obs_open_module would reject these; skip them to keep the log
// clean.
const std::set<std::string> kNonModuleDlls = {
	"chrome_elf", "libcef", "libegl", "libglesv2", "obs-browser-page",
};

std::string LowerCopy(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(tolower(c)); });
	return s;
}

std::string BaseNameNoExt(const std::string &filename)
{
	const size_t dot = filename.find_last_of('.');
	return dot == std::string::npos ? filename : filename.substr(0, dot);
}

// Route libobs/plugin blog() output to stderr so plugin lifecycle logging (e.g.
// obs-browser's "frontend owns CEF" line) is captured alongside the host's own.
void ObsLogHandler(int level, const char *format, va_list args, void *)
{
	char buf[4096];
	vsnprintf(buf, sizeof(buf), format, args);
	HostLog(std::string("[obs:log] ") + buf);
	(void)level;
}

// The frontend-api shim. Ownership is handed to libobs via
// obs_frontend_set_callbacks_internal; libobs deletes it on obs_shutdown. We
// keep a non-owning pointer to fan lifecycle events (FINISHED_LOADING).
FrontendCallbacks *g_frontend = nullptr;

// Default scene + sample source bound to output channel 0 so the preview has a
// visible canvas to render.
obs_scene_t *g_scene = nullptr;

// Curated full-set load: enumerate every *.dll in obs-plugins/64bit/ and
// obs_open_module + obs_init_module each one that isn't on the denylist or a
// non-module helper DLL, with the per-module data path. Logs a per-module result
// plus a final disposition summary. Ported from spike 4.0b's proven loader.
void LoadCuratedModules()
{
	const std::string root = RundirRoot();
	const std::string moduleDir = root + "/obs-plugins/64bit/";
	const std::string dataRoot = root + "/data/obs-plugins/";

	std::vector<std::string> loaded, initFailed, openFailed, skippedDeny, skippedHelper;

	const std::string pattern = moduleDir + "*.dll";
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) {
		HostLog("[obs] no plugin DLLs found in " + moduleDir);
		return;
	}
	do {
		const std::string file = fd.cFileName;
		const std::string name = BaseNameNoExt(file);
		const std::string lname = LowerCopy(name);

		if (kDenylist.count(lname)) {
			skippedDeny.push_back(name);
			// obs-websocket specifically would hard-crash a non-Qt process; the
			// rest are pure Qt UI helpers. Either way: intentionally skipped.
			HostLog("[obs] module " + name + " skipped (denylist, Qt-coupled / no headless value)");
			continue;
		}
		if (kNonModuleDlls.count(lname)) {
			skippedHelper.push_back(name);
			continue;
		}

		const std::string fullPath = moduleDir + file;
		const std::string dataPath = dataRoot + name + "/";

		obs_module_t *mod = nullptr;
		const int r = obs_open_module(&mod, fullPath.c_str(), dataPath.c_str());
		if (r != MODULE_SUCCESS || !mod) {
			openFailed.push_back(name);
			HostLog("[obs] module " + name + " open-failed (code=" + std::to_string(r) +
				", likely non-module)");
			continue;
		}
		if (obs_init_module(mod)) {
			loaded.push_back(name);
			HostLog("[obs] module " + name + " loaded");
		} else {
			initFailed.push_back(name);
			HostLog("[obs] module " + name + " init-failed");
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);

	auto joinList = [](const char *label, const std::vector<std::string> &v) {
		std::string line = std::string("[obs] ") + label + " (" + std::to_string(v.size()) + "):";
		for (const auto &n : v) {
			line += " " + n;
		}
		HostLog(line);
	};
	joinList("loaded", loaded);
	joinList("init-failed (environmental)", initFailed);
	joinList("open-failed/non-module", openFailed);
	joinList("skipped denylist", skippedDeny);
	joinList("skipped helper-dll", skippedHelper);
}

// Functional probes: create-then-release one of each core object kind to confirm
// the loaded plugin set registered its types. Ported from spike 4.0b.
void RunProbes()
{
	struct Probe {
		const char *kind;
		const char *id;
		void *(*create)(const char *);
		void (*release)(void *);
	};

	auto encCreate = [](const char *id) -> void * {
		return obs_video_encoder_create(id, "probe-enc", nullptr, nullptr);
	};
	auto svcCreate = [](const char *id) -> void * { return obs_service_create(id, "probe-svc", nullptr, nullptr); };
	auto outCreate = [](const char *id) -> void * { return obs_output_create(id, "probe-out", nullptr, nullptr); };
	auto srcCreate = [](const char *id) -> void * { return obs_source_create(id, "probe-web", nullptr, nullptr); };

	const Probe probes[] = {
		{"encoder", "obs_x264", encCreate, [](void *p) { obs_encoder_release((obs_encoder_t *)p); }},
		{"service", "rtmp_custom", svcCreate, [](void *p) { obs_service_release((obs_service_t *)p); }},
		{"output", "rtmp_output", outCreate, [](void *p) { obs_output_release((obs_output_t *)p); }},
		{"source", "browser_source", srcCreate, [](void *p) { obs_source_release((obs_source_t *)p); }},
	};

	for (const auto &p : probes) {
		void *obj = p.create(p.id);
		HostLog(std::string("[obs] probe ") + p.kind + " " + p.id + " -> " + (obj ? "OK" : "FAIL"));
		if (obj) {
			p.release(obj);
		}
	}
}

// Build the clean default scene: a single solid color_source sized to the canvas,
// bound to output channel 0 so the preview visibly renders a non-empty canvas.
// A placeholder until the real scene/source UI (4.3+) drives content; no network,
// no CEF dependency, ticks immediately.
obs_scene_t *BuildDefaultScene()
{
	obs_video_info ovi = {};
	const uint32_t cx = obs_get_video_info(&ovi) ? ovi.base_width : 1920;
	const uint32_t cy = obs_get_video_info(&ovi) ? ovi.base_height : 1080;

	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "color", 0xff334155); // ARGB slate (matches the UI's sunken bg family)
	obs_data_set_int(settings, "width", cx);
	obs_data_set_int(settings, "height", cy);

	obs_source_t *source = obs_source_create("color_source", "Placeholder Background", settings, nullptr);
	obs_data_release(settings);
	if (!source) {
		HostLog("[obs] obs_source_create(color_source) failed");
		return nullptr;
	}
	HostLog("[obs] default color source created (Placeholder Background)");

	obs_scene_t *scene = obs_scene_create("Default Scene");
	if (!scene) {
		HostLog("[obs] obs_scene_create failed");
		obs_source_release(source);
		return nullptr;
	}

	obs_scene_add(scene, source);
	obs_source_release(source); // scene owns the create-ref now

	obs_set_output_source(0, obs_scene_get_source(scene));
	HostLog("[obs] default scene bound to output channel 0");
	return scene; // caller owns the create-ref
}

void CreateDefaultScene()
{
	// Boot placeholder path (no scene file yet): retain the create-ref as g_scene so
	// TeardownScene can release it on a clean exit.
	g_scene = BuildDefaultScene();
}

// The native-multistream data model (Phase 4.4.0). Global so 4.4.1+ can expose it
// to the bridge. Three layers: canvases (global canvases.json), stream profiles
// (global streams.json), output bindings (profile x canvas; standalone
// output_bindings.json for now -- see OutputBindingStore).
CanvasStore g_canvases;
StreamProfileStore g_streamProfiles;
OutputBindingStore g_outputBindings;
SceneLinkStore g_sceneLinks;

// The virtual-camera output manager. Empty until Start() loads its target canvas;
// its onChanged is wired to the virtualCam.changed event next to the engine's
// status hook below. Shut down in Stop before the canvases it feeds are torn down.
VirtualCamManager g_virtualCam;

// The global General settings bag, loaded early in Start (other systems read its
// prefs) and persisted on each bridge set. A plain struct -- no teardown needed.
GeneralSettings g_general;

// The global Advanced settings bag, loaded early in Start (its process priority is
// applied once at load; the engine reads its per-output options at StartOutput)
// and persisted on each bridge set. A plain struct -- no teardown needed.
AdvancedSettings g_advanced;

// The scene-collection registry (per-collection scene sets). Loaded + migrated
// in Start before scenes are restored, so the no-arg SceneCollection::Save/Load
// target the active collection's file; cleared in Stop.
SceneCollections g_sceneCollections;

// The per-scene-collection undo/redo stack. Empty at boot; mutations record into
// it (a later task), the scene-collection switch + Stop clear it. Its onChanged is
// wired to the undo.changed event in Bridge::Init.
UndoManager g_undo;

// The fan-out streaming engine, constructed after the stores load (it captures
// them by reference) and reset in Stop before they clear.
std::unique_ptr<MultistreamEngine> g_multistream;

// Live obs_canvas_t mixes for the additional (non-Default) canvases, so the
// engine can encode them. Built from g_canvases after the model loads (before the
// engine, which resolves canvas video through it) and torn down in Stop after the
// engine is gone but while libobs is still up.
std::unique_ptr<CanvasRuntime> g_canvasRuntime;

// The audio mixer's per-source fader/volmeter manager. Built in Start after the
// default scene + modules, torn down in Stop BEFORE obs_shutdown (its volmeter
// callbacks are removed first by ClearAll). The global source activate/deactivate
// signals below rebuild its set + push audio.changed so the UI re-lists.
std::unique_ptr<AudioMonitor> g_audioMonitor;

// The embedded MCP server. Constructed at the end of Start() (after the audio
// monitor is up) and torn down at the very top of Stop() (before Bridge::Shutdown,
// so its accept thread is joined while the bridge + libobs are still alive).
// Disabled by default (mcp.json enabled=false), so nothing listens unless opted in.
std::unique_ptr<McpServer> g_mcp;

// Rebuild the audio monitor's active-source set and notify the UI. Wired to the
// global source activate/deactivate signals; runs on the signal's thread (the
// source pipeline thread), but AudioMonitor::Rebuild is self-synchronized and
// Bridge::EmitAudioChanged marshals to TID_UI, so this is safe off the UI thread.
void OnAudioSourceSetChanged(void * /*data*/, calldata_t * /*params*/)
{
	if (g_audioMonitor) {
		g_audioMonitor->Rebuild();
		Bridge::EmitAudioChanged();
	}
}

// The global signals that change which sources have active audio. Connected after
// modules load (so the global signal handler exists) and disconnected in Stop.
const char *const kAudioSourceSignals[] = {
	"source_activate",
	"source_deactivate",
	"source_audio_activate",
	"source_audio_deactivate",
};

void ConnectAudioSourceSignals()
{
	signal_handler_t *handler = obs_get_signal_handler();
	if (!handler) {
		return;
	}
	for (const char *signal : kAudioSourceSignals) {
		signal_handler_connect(handler, signal, OnAudioSourceSetChanged, nullptr);
	}
}

void DisconnectAudioSourceSignals()
{
	signal_handler_t *handler = obs_get_signal_handler();
	if (!handler) {
		return;
	}
	for (const char *signal : kAudioSourceSignals) {
		signal_handler_disconnect(handler, signal, OnAudioSourceSetChanged, nullptr);
	}
}

// Load (or seed) the model from the shared config dir and log its shape. Must run
// after modules load so EnsureDefaultEncoders sees registered encoders.
void LoadMultistreamModel()
{
	g_canvases.Load();
	if (g_canvases.EnsureDefaultEncoders()) {
		g_canvases.Save();
	}
	g_streamProfiles.Load();
	g_outputBindings.Load();
	g_sceneLinks.Load();

	const CanvasDefinition &def = g_canvases.Default();
	HostLog("[obs] multistream: " + std::to_string(g_canvases.Definitions().size()) +
		" canvas(es); default='" + def.name + "' uuid=" + def.uuid + " " + std::to_string(def.width) + "x" +
		std::to_string(def.height) + "@" + std::to_string(def.fpsNum) + "/" + std::to_string(def.fpsDen) +
		" venc=" + (def.video.id.empty() ? "(unset)" : def.video.id) +
		" aenc=" + (def.audio.id.empty() ? "(unset)" : def.audio.id));

	const StreamProfile *primary = g_streamProfiles.Primary();
	HostLog("[obs] multistream: " + std::to_string(g_streamProfiles.Profiles().size()) +
		" stream profile(s); primary=" + (primary ? primary->DisplayName() : "(none)"));

	HostLog("[obs] multistream: " + std::to_string(g_outputBindings.Bindings().bindings.size()) +
		" output binding(s); file=" + g_sceneCollections.ActiveBindingsPath());
	HostLog("[obs] multistream: canvases.json=" + CanvasStore::FilePath());
	HostLog("[obs] multistream: streams.json=" + StreamProfileStore::FilePath());
}

} // namespace

::SceneCollections &ObsBootstrap::SceneCollections()
{
	return g_sceneCollections;
}

CanvasStore &ObsBootstrap::Canvases()
{
	return g_canvases;
}

StreamProfileStore &ObsBootstrap::StreamProfiles()
{
	return g_streamProfiles;
}

OutputBindingStore &ObsBootstrap::OutputBindings()
{
	return g_outputBindings;
}

SceneLinkStore &ObsBootstrap::SceneLinks()
{
	return g_sceneLinks;
}

UndoManager &ObsBootstrap::Undo()
{
	return g_undo;
}

VirtualCamManager &ObsBootstrap::VirtualCam()
{
	return g_virtualCam;
}

GeneralSettings &ObsBootstrap::General()
{
	return g_general;
}

AdvancedSettings &ObsBootstrap::Advanced()
{
	return g_advanced;
}

::CanvasRuntime &ObsBootstrap::CanvasRuntime()
{
	// Valid between Start() (constructs g_canvasRuntime after the model loads) and
	// Stop() (resets it). Like Multistream(), every caller is a bridge method
	// driven by JS, so the pointer is non-null on every reachable path.
	return *g_canvasRuntime;
}

void ObsBootstrap::ApplyCanvasSceneLinks(const std::string &mainSceneUuid)
{
	if (mainSceneUuid.empty()) {
		return;
	}
	const CanvasSceneLink &link = SceneLinks().Links();
	auto it = link.map.find(mainSceneUuid);
	if (it == link.map.end()) {
		return;
	}
	::CanvasRuntime &runtime = CanvasRuntime();
	for (const auto &[canvasUuid, canvasSceneUuid] : it->second) {
		// Resolve the stored canvas-scene uuid -> its current name, then switch.
		for (const CanvasRuntime::SceneInfo &s : runtime.Scenes(canvasUuid)) {
			if (s.uuid == canvasSceneUuid) {
				if (runtime.SetCurrentScene(canvasUuid, s.name)) {
					Bridge::EmitEvent("scenes.changed", nlohmann::json{{"canvas", canvasUuid}});
				}
				break;
			}
		}
	}
}

void ObsBootstrap::PruneSceneLinksForMainScene(const std::string &mainSceneUuid)
{
	CanvasSceneLink &link = SceneLinks().Links();
	if (link.map.erase(mainSceneUuid) > 0) {
		SceneLinks().Save();
	}
}

void ObsBootstrap::PruneSceneLinksForCanvas(const std::string &canvasUuid)
{
	CanvasSceneLink &link = SceneLinks().Links();
	bool changed = false;
	for (auto it = link.map.begin(); it != link.map.end();) {
		if (it->second.erase(canvasUuid) > 0) {
			changed = true;
		}
		if (it->second.empty()) {
			it = link.map.erase(it);
		} else {
			++it;
		}
	}
	if (changed) {
		SceneLinks().Save();
	}
}

void ObsBootstrap::PruneSceneLinksForCanvasScene(const std::string &canvasUuid, const std::string &canvasSceneUuid)
{
	SceneLinks().Links().UnsetByCanvasScene(canvasUuid, canvasSceneUuid);
	SceneLinks().Save();
}

MultistreamEngine &ObsBootstrap::Multistream()
{
	// Valid only between Start() (constructs g_multistream after the stores load)
	// and Stop() (resets it). Every caller is a bridge method driven by JS, which
	// can only run once the CEF page has loaded -- well after construction -- so
	// the pointer is non-null on every reachable path.
	return *g_multistream;
}

::AudioMonitor &ObsBootstrap::AudioMonitor()
{
	// Valid between Start() (constructs g_audioMonitor after the default scene +
	// modules) and Stop() (resets it). Callers are bridge methods + the throttled
	// audio.levels emit, all reachable only after the CEF page loads, so the
	// pointer is non-null on every reachable path.
	return *g_audioMonitor;
}

bool ObsBootstrap::Start()
{
	// obs-browser checks this in its guarded path to skip CefInitialize (the
	// frontend already owns the single CEF context). Set before module load.
	SetEnvironmentVariableW(L"OBS_FRONTEND_OWNS_CEF", L"1");

	base_set_log_handler(ObsLogHandler, nullptr);

	if (!obs_startup("en-US", nullptr, nullptr)) {
		HostLog("[obs] obs_startup failed");
		return false;
	}
	HostLog("[obs] obs_startup ok");

	const std::string root = RundirRoot();
	obs_add_data_path((root + "/data/libobs/").c_str());

	obs_video_info ovi = {};
	ovi.graphics_module = "libobs-d3d11";
	ovi.fps_num = 60;
	ovi.fps_den = 1;
	ovi.base_width = 1920;
	ovi.base_height = 1080;
	ovi.output_width = 1920;
	ovi.output_height = 1080;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.adapter = 0;
	ovi.gpu_conversion = true;
	ovi.scale_type = OBS_SCALE_BICUBIC;

	const int rv = obs_reset_video(&ovi);
	if (rv != OBS_VIDEO_SUCCESS) {
		HostLog("[obs] obs_reset_video failed, code=" + std::to_string(rv));
		return false;
	}
	HostLog("[obs] obs_reset_video ok (1920x1080@60, D3D11)");

	obs_audio_info oai = {};
	oai.samples_per_sec = 48000;
	oai.speakers = SPEAKERS_STEREO;
	if (!obs_reset_audio(&oai)) {
		HostLog("[obs] obs_reset_audio failed");
		return false;
	}
	HostLog("[obs] obs_reset_audio ok (48kHz stereo)");

	// Register the frontend-api shim before loading modules so obs-browser's
	// obs_module_load (which calls obs_frontend_add_event_callback) resolves
	// against it. libobs takes ownership and deletes it on obs_shutdown.
	g_frontend = new FrontendCallbacks();
	obs_frontend_set_callbacks_internal(g_frontend);
	HostLog("[obs] frontend-api shim registered");

	// Build the JS<->C++ method registry and arm obs->JS event forwarding
	// before module load + the FINISHED_LOADING fan-out, so the bridge's
	// frontend event callback is registered when those events fire.
	Bridge::Init();

	// Load the global General settings early: projectors + later systems read it.
	g_general.Load();
	HostLog("[obs] general settings loaded");

	// Load the global Advanced settings and apply the stored process priority once.
	// The engine reads the rest (stream delay / reconnect / network) per output at
	// StartOutput; browserHwAccel is store-only.
	g_advanced.Load();
	ApplyProcessPriority(g_advanced.processPriority);
	HostLog("[obs] advanced settings loaded; process priority=" + g_advanced.processPriority);

	LoadCuratedModules();

	obs_post_load_modules();
	HostLog("[obs] core up (curated full-set load)");

	// Lifecycle signal plugins' registered handlers expect post-load.
	if (g_frontend) {
		g_frontend->on_event(OBS_FRONTEND_EVENT_FINISHED_LOADING);
	}

	RunProbes();

	// Load (or first-run migrate) the scene-collection registry BEFORE restoring
	// scenes, so the no-arg SceneCollection::Load/Save below resolve the active
	// collection's file. First run (no scene_collections.json) adopts the legacy
	// single-file scene_collection.json IN PLACE -- reused, not copied, so the
	// user's existing scenes carry over with zero data loss -- as the sole
	// "Untitled" collection.
	g_sceneCollections.Load();
	if (g_sceneCollections.List().empty() && !g_sceneCollections.IndexWasCorrupt()) {
		g_sceneCollections.SeedExisting("Untitled", "scene_collection.json");
		HostLog("[scene] migrated single-file scenes into collection 'Untitled'");

		// Output bindings were a single global output_bindings.json (pre-6a); they
		// are now per scene-collection. Move the legacy file to the migrated
		// collection's bindings path in place (rename; zero data loss). An absent
		// legacy file just means the collection starts with no bindings.
		const std::string legacyBindings = OutputBindingStore::FilePath();
		const std::string targetBindings = g_sceneCollections.ActiveBindingsPath();
		if (os_file_exists(legacyBindings.c_str()) && !os_file_exists(targetBindings.c_str())) {
			std::error_code ec;
			std::filesystem::rename(std::filesystem::u8path(legacyBindings),
						std::filesystem::u8path(targetBindings), ec);
			if (ec) {
				std::filesystem::copy_file(std::filesystem::u8path(legacyBindings),
							   std::filesystem::u8path(targetBindings),
							   std::filesystem::copy_options::overwrite_existing, ec);
			}
			HostLog("[scene] migrated global output bindings -> " + targetBindings +
				(ec ? " (FAILED: " + ec.message() + ")" : ""));
		}
	}
	const SceneCollectionRecord *activeCollection = g_sceneCollections.Active();
	HostLog("[scene] " + std::to_string(g_sceneCollections.List().size()) + " scene collection(s); active='" +
		(activeCollection ? activeCollection->name : "(none)") + "' file=" +
		g_sceneCollections.ActiveScenePath());

	// Restore the active collection's scenes; first run with no scene file falls
	// back to the placeholder default scene. On the Load path g_scene stays null,
	// which the null-safe TeardownScene handles.
	if (!SceneCollection::Load()) {
		CreateDefaultScene();
	}

	// Route channel 0 through the program transition: it wraps the scene just bound
	// above and rebinds itself to channel 0, so scene switches animate (Fade by
	// default). Sized to the base canvas, hence after obs_reset_video above.
	Transitions::Init();

	LoadMultistreamModel();

	// Boot reconcile: the global video pipeline was initialized to a fixed default
	// above (before modules could load), but the persisted Default canvas def is the
	// source of truth for its resolution/FPS -- the Settings UI edits the Default
	// canvas, which drives global video. Re-apply from the def so a saved resolution
	// survives restarts. Inert on first run (seeded def == the fixed init).
	{
		const CanvasDefinition &def = g_canvases.Default();
		obs_video_info cur = {};
		if (obs_get_video_info(&cur) &&
		    (cur.base_width != def.width || cur.base_height != def.height || cur.fps_num != def.fpsNum ||
		     cur.fps_den != def.fpsDen)) {
			obs_video_info want = cur;
			want.base_width = def.width;
			want.base_height = def.height;
			want.output_width = def.width;
			want.output_height = def.height;
			want.fps_num = def.fpsNum;
			want.fps_den = def.fpsDen;
			if (obs_reset_video(&want) == OBS_VIDEO_SUCCESS) {
				Transitions::OnVideoReset();
				HostLog("[obs] video reconciled to Default canvas " + std::to_string(def.width) + "x" +
					std::to_string(def.height) + "@" + std::to_string(def.fpsNum) + "/" +
					std::to_string(def.fpsDen));
			} else {
				HostLog("[obs] video reconcile to Default canvas FAILED; keeping init resolution");
			}
		}
	}

	// Bring up the live obs_canvas_t mixes for the additional canvases before the
	// engine, which resolves canvas video through the runtime below.
	g_canvasRuntime = std::make_unique<::CanvasRuntime>(g_canvases);
	g_canvasRuntime->SyncFromDefinitions();

	// Build the fan-out engine over the now-loaded stores. The Default canvas
	// encodes from the global mix; additional canvases encode from their
	// CanvasRuntime obs_canvas_t mix. State changes route to the bridge, which
	// posts the multistream.changed push on its own (thread-safe) UI marshaling.
	g_multistream = std::make_unique<MultistreamEngine>(
		g_canvases, g_streamProfiles, g_outputBindings, [](const std::string &uuid) -> video_t * {
			return uuid == g_canvases.Default().uuid ? obs_get_video() : g_canvasRuntime->VideoFor(uuid);
		});
	g_multistream->onStatusChanged = [] { Bridge::EmitMultistreamChanged(); };

	// Restore the virtual camera's target canvas and route its start/stop signal
	// state changes to the virtualCam.changed push. Done after the CanvasRuntime is
	// up (Start() resolves the target canvas's mix through it). Like the engine's
	// hook, EmitVirtualCamChanged marshals to TID_UI so the off-thread signal is
	// safe.
	g_virtualCam.Load();
	g_virtualCam.onChanged = [] { Bridge::EmitVirtualCamChanged(); };

	// Register the frontend-owned hotkeys (Start/Stop Streaming, wired to the engine
	// above) and load saved bindings. Done after modules + scenes load (so every
	// source/output/etc. hotkey id exists for Load to resolve by name) and after the
	// engine exists (the callbacks drive it). libobs's hotkey thread fires bound
	// hotkeys globally from here on -- no key injection needed.
	Hotkeys::RegisterFrontendHotkeys();

	// Seed (first run) or restore the global audio devices (Desktop Audio / Mic) on
	// output channels 1..6 -- stock OBS sets these up but the new frontend never did,
	// leaving the mixer empty. Done before the AudioMonitor below so its initial
	// Rebuild enumerates the seeded channels.
	Bridge::SeedGlobalAudio();

	// Bring up the audio mixer manager and seed it from the current active audio
	// sources, then arm the global signals that change which sources have audio so
	// the set + the UI stay in sync. Built last (after the default scene + modules)
	// so the initial Rebuild sees the steady-state pipeline.
	g_audioMonitor = std::make_unique<::AudioMonitor>();
	g_audioMonitor->Rebuild();
	ConnectAudioSourceSignals();
	HostLog("[obs] audio monitor up; active audio sources=" + std::to_string(g_audioMonitor->List().size()));

	// Bring up the embedded MCP server last (after the bridge + stores + audio are
	// all live, so any tool call lands on a fully-up engine). Disabled by default
	// (mcp.json enabled=false), so this only listens when the user opts in.
	g_mcp = std::make_unique<McpServer>();
	Mcp::SetInstance(g_mcp.get());
	g_mcp->Start();

	// Re-apply scene links to the restored program scene so "following" canvases
	// come up on their linked scene rather than their own saved current scene.
	{
		OBSSourceAutoRelease program = Transitions::GetProgramScene();
		if (program) {
			const char *pu = obs_source_get_uuid(program);
			if (pu) {
				ApplyCanvasSceneLinks(pu);
			}
		}
	}

	return true;
}

void ObsBootstrap::CreateDefaultSceneDetached()
{
	// Switching to a never-saved collection has nothing to load: stand up a fresh
	// placeholder scene like boot, but hand the create-ref to libobs (channel 0 + the
	// global source list keep it alive). Unlike boot it is deliberately NOT tracked by
	// g_scene -- the scene-collection switch tears the world down by enumeration, and
	// a retained g_scene ref would keep the removed scene alive (leak) across the next
	// switch.
	obs_scene_t *scene = BuildDefaultScene();
	if (scene) {
		obs_scene_release(scene);
	}
}

void ObsBootstrap::TeardownScene()
{
	if (!g_scene) {
		return;
	}

	// Unbind from the output channel first so nothing ticks/renders it.
	obs_set_output_source(0, nullptr);

	obs_source_t *scene_source = obs_scene_get_source(g_scene);
	obs_source_remove(scene_source);
	obs_scene_release(g_scene);
	g_scene = nullptr;
	HostLog("[obs] default scene released");
}

void ObsBootstrap::FireSceneChanged()
{
	// Re-announce the bound scene so the (now-loaded) page observes a forwarded
	// obs.event end-to-end. The shim fans this to the bridge's event callback.
	if (g_frontend) {
		g_frontend->on_event(OBS_FRONTEND_EVENT_SCENE_CHANGED);
	}
}

void ObsBootstrap::RunPropertiesSelfTest()
{
	using Bridge::json;

	auto run = [](const std::string &method, const json &params) -> json {
		json result;
		std::string error;
		if (!Bridge::Dispatch(method, params, result, error)) {
			HostLog("[selftest] " + method + " FAILED: " + error);
			return json(nullptr);
		}
		return result;
	};

	// 1) properties.get on the default color source: expect color + width/height.
	json got = run("properties.get", json{{"kind", "source"}, {"ref", "Placeholder Background"}});
	if (!got.is_object()) {
		return;
	}
	const json &props = got["props"];
	std::string names;
	for (const auto &p : props) {
		names += " " + p.value("name", std::string("?")) + "(" + p.value("type", std::string("?")) + ")";
	}
	HostLog("[selftest] properties.get color_source -> " + std::to_string(props.size()) +
		" props:" + names);
	const int64_t before = got["values"].value("color", int64_t(0));
	HostLog("[selftest] color before = " + std::to_string(before));

	// 2) properties.set a new color, prove the value round-trips on re-fetch.
	const int64_t newColor = 0xff00ff00; // opaque green (ABGR)
	json set = run("properties.set", json{{"kind", "source"},
					      {"ref", "Placeholder Background"},
					      {"settings", json{{"color", newColor}}}});
	if (set.is_object()) {
		const int64_t after = set["values"].value("color", int64_t(0));
		HostLog("[selftest] color after set = " + std::to_string(after) +
			(after == newColor ? " (round-trip OK)" : " (MISMATCH)"));
		HostLog("[selftest] re-fetched props count = " + std::to_string(set["props"].size()));
	}

	// 3) Restore the original color so the smoke run leaves no visible change.
	run("properties.set",
	    json{{"kind", "source"}, {"ref", "Placeholder Background"}, {"settings", json{{"color", before}}}});
	HostLog("[selftest] color restored to " + std::to_string(before));

	// 4) Richer-type coverage: a transient browser_source (url/fps/css/list/bool/
	// path) exercises text/int/list/group descriptors. Not added to any scene; we
	// query its properties then remove + release it (no committed scene change).
	obs_source_t *web = obs_source_create("browser_source", "selftest-web", nullptr, nullptr);
	if (web) {
		json bs = run("properties.get", json{{"kind", "source"}, {"ref", "selftest-web"}});
		if (bs.is_object()) {
			std::string types;
			for (const auto &p : bs["props"]) {
				types += " " + p.value("name", std::string("?")) + "(" +
					 p.value("type", std::string("?")) + ")";
			}
			HostLog("[selftest] browser_source props (" + std::to_string(bs["props"].size()) +
				"):" + types);
		}
		obs_source_remove(web);
		obs_source_release(web);
		HostLog("[selftest] transient browser_source released");
	}

	// 5) sourceTypes.list: prove a sensible creatable set is returned.
	json typesList = run("sourceTypes.list", json(nullptr));
	if (typesList.is_array()) {
		std::string sample;
		int shown = 0;
		for (const auto &t : typesList) {
			if (shown++ < 10) {
				sample += " " + t.value("id", std::string("?"));
			}
		}
		HostLog("[selftest] sourceTypes.list -> " + std::to_string(typesList.size()) +
			" types, e.g.:" + sample);
	}

	// 6) sources.create two distinct types into the current scene, proving each
	// adds a sceneitem (and emits sceneItems.changed). Then remove them so the
	// smoke run leaves the scene as it found it.
	const char *kCreateTypes[] = {"color_source", "image_source"};
	for (const char *type : kCreateTypes) {
		json created = run("sources.create", json{{"type", type}});
		if (created.is_object()) {
			const int64_t id = created.value("id", int64_t(0));
			const std::string src = created.value("source", std::string("?"));
			HostLog("[selftest] sources.create " + std::string(type) + " -> id=" +
				std::to_string(id) + " source='" + src + "'");

			// Transform round-trip on the first created item: set pos, prove
			// getTransform reads it back, then exercise a quick action.
			if (std::string(type) == "color_source" && id) {
				run("sceneItems.setTransform",
				    json{{"id", id}, {"transform", json{{"pos", json{{"x", 123.0}, {"y", 45.0}}}}}});
				json tf = run("sceneItems.getTransform", json{{"id", id}});
				if (tf.is_object()) {
					const double px = tf["pos"].value("x", 0.0);
					const double py = tf["pos"].value("y", 0.0);
					HostLog("[selftest] sceneItems transform pos=" + std::to_string(px) + "," +
						std::to_string(py) +
						((px == 123.0 && py == 45.0) ? " (round-trip OK)" : " (MISMATCH)") +
						" base=" + std::to_string(tf.value("baseWidth", 0)) + "x" +
						std::to_string(tf.value("baseHeight", 0)));
				}
				json act = run("sceneItems.transformAction", json{{"id", id}, {"action", "center"}});
				if (act.is_object()) {
					HostLog("[selftest] sceneItems.transformAction center -> pos=" +
						std::to_string(act["pos"].value("x", 0.0)) + "," +
						std::to_string(act["pos"].value("y", 0.0)));
				}
			}

			run("sceneItems.remove", json{{"id", id}});
			obs_source_t *s = obs_get_source_by_name(src.c_str());
			if (s) {
				obs_source_remove(s);
				obs_source_release(s);
			}
		}
	}
	HostLog("[selftest] sources.create round-trip done (transient items removed)");
}

void ObsBootstrap::RunPreviewEditSelfTest()
{
	obs_source_t *sceneSource = Transitions::GetProgramScene(); // addref'd; unwraps the ch0 transition
	if (!sceneSource) {
		HostLog("[selftest] preview-edit: no scene bound to output 0");
		return;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);

	struct First {
		obs_sceneitem_t *item;
	} ctx{nullptr};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *p) -> bool {
			static_cast<First *>(p)->item = item;
			return false; // first (bottom-most) is enough
		},
		&ctx);
	if (!ctx.item) {
		HostLog("[selftest] preview-edit: scene has no items");
		obs_source_release(sceneSource);
		return;
	}

	const int64_t id = obs_sceneitem_get_id(ctx.item);
	obs_source_t *itemSrc = obs_sceneitem_get_source(ctx.item);
	const char *srcName = itemSrc ? obs_source_get_name(itemSrc) : nullptr;
	HostLog("[selftest] preview-edit: first item '" + std::string(srcName ? srcName : "?") +
		"' id=" + std::to_string(id));

	// Item center in canvas coords via its box transform (unit 0.5,0.5 -> center).
	matrix4 boxTransform;
	obs_sceneitem_get_box_transform(ctx.item, &boxTransform);
	vec3 center;
	vec3_set(&center, 0.5f, 0.5f, 0.0f);
	vec3_transform(&center, &center, &boxTransform);

	// 1) Select via the same entry point the bridge uses (Default surface => "").
	const bool selOk = Preview::SelectFromBridge("", "", id, true);
	HostLog("[selftest] preview-edit: SelectFromBridge -> " + std::string(selOk ? "OK" : "FAIL"));

	// 2) Hit-test at the item center: expect to get the same id back.
	const int64_t hit = Preview::HitTestForTest("", center.x, center.y);
	HostLog("[selftest] preview-edit: hit-test at center (" + std::to_string(int(center.x)) + "," +
		std::to_string(int(center.y)) + ") -> id=" + std::to_string(hit) +
		(hit == id ? " (match)" : " (MISMATCH)"));

	// 3) Exercise the move math directly, then restore the original position.
	vec2 origPos;
	obs_sceneitem_get_pos(ctx.item, &origPos);
	vec2 movedPos;
	vec2_set(&movedPos, origPos.x + 50.0f, origPos.y + 30.0f);
	obs_sceneitem_set_pos(ctx.item, &movedPos);
	vec2 afterPos;
	obs_sceneitem_get_pos(ctx.item, &afterPos);
	HostLog("[selftest] preview-edit: move pos before=(" + std::to_string(int(origPos.x)) + "," +
		std::to_string(int(origPos.y)) + ") after=(" + std::to_string(int(afterPos.x)) + "," +
		std::to_string(int(afterPos.y)) + ")");
	obs_sceneitem_set_pos(ctx.item, &origPos);
	vec2 restoredPos;
	obs_sceneitem_get_pos(ctx.item, &restoredPos);
	HostLog("[selftest] preview-edit: pos restored to (" + std::to_string(int(restoredPos.x)) + "," +
		std::to_string(int(restoredPos.y)) + ")");

	// Clear the selection so the smoke run leaves no committed selection state.
	Preview::SelectFromBridge("", "", 0, false);
	obs_source_release(sceneSource);
}

void ObsBootstrap::RunSettingsSelfTest()
{
	using Bridge::json;

	auto run = [](const std::string &method, const json &params, bool &ok) -> json {
		json result;
		std::string error;
		ok = Bridge::Dispatch(method, params, result, error);
		if (!ok) {
			HostLog("[selftest] " + method + " FAILED: " + error);
			return json(nullptr);
		}
		return result;
	};

	bool ok = false;

	// 1) getVideo: expect the bootstrap defaults (1920x1080@60).
	json v0 = run("settings.getVideo", json(nullptr), ok);
	if (!ok) {
		return;
	}
	const uint32_t baseW = v0.value("baseWidth", 0u);
	const uint32_t baseH = v0.value("baseHeight", 0u);
	HostLog("[selftest] getVideo -> base " + std::to_string(baseW) + "x" + std::to_string(baseH) + " out " +
		std::to_string(v0.value("outputWidth", 0u)) + "x" + std::to_string(v0.value("outputHeight", 0u)) +
		" @ " + std::to_string(v0.value("fpsNum", 0u)) + "/" + std::to_string(v0.value("fpsDen", 0u)));

	// 2) setVideo to 1280x720@30; confirm the new active fps + that the display is
	// still alive (a frame can still draw) after the reset.
	json v1 = run("settings.setVideo",
		      json{{"baseWidth", 1280},
			   {"baseHeight", 720},
			   {"outputWidth", 1280},
			   {"outputHeight", 720},
			   {"fpsNum", 30},
			   {"fpsDen", 1}},
		      ok);
	if (ok) {
		HostLog("[selftest] setVideo 1280x720@30 -> base " + std::to_string(v1.value("baseWidth", 0u)) + "x" +
			std::to_string(v1.value("baseHeight", 0u)) + " (round-trip " +
			((v1.value("baseWidth", 0u) == 1280 && v1.value("baseHeight", 0u) == 720) ? "OK" : "MISMATCH") +
			")");
		HostLog("[selftest] active fps after reset = " + std::to_string(obs_get_active_fps()));
		// Prove the Default preview surface survived: it re-validates without
		// re-creation (its display + draw callback persist across a video reset).
		PreviewSurface *defaultSurface = Preview::Instance() ? Preview::Instance()->SurfaceFor("") : nullptr;
		const bool alive = defaultSurface && defaultSurface->OnVideoReset();
		HostLog("[selftest] preview display after video reset -> " +
			std::string(alive ? "ALIVE (re-validated)" : "not yet created"));
		// Prove the default scene is still bound to output channel 0.
		obs_source_t *scene = Transitions::GetProgramScene();
		HostLog("[selftest] output ch0 scene after reset -> " +
			std::string(scene ? obs_source_get_name(scene) : "NULL"));
		if (scene) {
			obs_source_release(scene);
		}
	}

	// 3) Restore the original video config so the smoke run leaves no change.
	run("settings.setVideo", v0, ok);
	HostLog("[selftest] video restored to " + std::to_string(baseW) + "x" + std::to_string(baseH));

	// 4) getAudio: expect the bootstrap defaults (48000 stereo).
	json a0 = run("settings.getAudio", json(nullptr), ok);
	if (ok) {
		HostLog("[selftest] getAudio -> " + std::to_string(a0.value("sampleRate", 0u)) + "Hz " +
			a0.value("speakers", std::string("?")));
	}

	// 5) setAudio to 44100; confirm it round-trips.
	json a1 = run("settings.setAudio", json{{"sampleRate", 44100}, {"speakers", "stereo"}}, ok);
	if (ok) {
		HostLog("[selftest] setAudio 44100 -> " + std::to_string(a1.value("sampleRate", 0u)) + "Hz " +
			a1.value("speakers", std::string("?")) + " (round-trip " +
			(a1.value("sampleRate", 0u) == 44100 ? "OK" : "MISMATCH") + ")");
	}

	// 6) Restore the original audio config.
	run("settings.setAudio", a0, ok);
	HostLog("[selftest] audio restored to " + std::to_string(a0.value("sampleRate", 0u)) + "Hz");
}

void ObsBootstrap::RunCanvasBridgeSelfTest()
{
	using Bridge::json;

	auto run = [](const std::string &method, const json &params, bool &ok) -> json {
		json result;
		std::string error;
		ok = Bridge::Dispatch(method, params, result, error);
		if (!ok) {
			HostLog("[selftest] " + method + " FAILED: " + error);
			return json(nullptr);
		}
		return result;
	};

	bool ok = false;

	// 1) canvas.list: report the user's real canvases.
	json list = run("canvas.list", json(nullptr), ok);
	if (ok && list.is_array()) {
		std::string names;
		for (const auto &c : list) {
			names += " '" + c.value("name", std::string("?")) + "'(" +
				 std::to_string(c.value("baseWidth", 0u)) + "x" +
				 std::to_string(c.value("baseHeight", 0u)) + "@" +
				 std::to_string(c.value("fpsNum", 0u)) + "/" + std::to_string(c.value("fpsDen", 0u)) +
				 (c.value("isDefault", false) ? ",default" : "") + ")";
		}
		HostLog("[selftest] canvas.list -> " + std::to_string(list.size()) + " canvas(es):" + names);
	}

	// 2) encoderTypes.list video + audio: prove sane sets.
	for (const char *kind : {"video", "audio"}) {
		json types = run("encoderTypes.list", json{{"kind", kind}}, ok);
		if (ok && types.is_array()) {
			std::string sample;
			int shown = 0;
			for (const auto &t : types) {
				if (shown++ < 6) {
					sample += " " + t.value("id", std::string("?"));
				}
			}
			HostLog("[selftest] encoderTypes.list " + std::string(kind) + " -> " +
				std::to_string(types.size()) + ":" + sample);
		}
	}

	// 3) create -> update -> (encoder properties.get) -> remove round-trip, proving
	// each persists to canvases.json and the file ends exactly as it began.
	json created = run("canvas.create",
			   json{{"name", "selftest-bridge-canvas"},
				{"baseWidth", 1280},
				{"baseHeight", 720},
				{"outputWidth", 854},
				{"outputHeight", 480},
				{"scaleType", "lanczos"},
				{"fpsNum", 60000},
				{"fpsDen", 1001}},
			   ok);
	if (!ok || !created.is_object()) {
		return;
	}
	const std::string uuid = created.value("uuid", std::string());
	HostLog("[selftest] canvas.create -> uuid=" + uuid);

	// Confirm the scaled-output / downscale-filter / fractional-fps fields round-trip
	// through canvas.list (CanvasToJson reads them straight off the stored def).
	{
		json relist = run("canvas.list", json(nullptr), ok);
		if (ok && relist.is_array()) {
			for (const auto &c : relist) {
				if (c.value("uuid", std::string()) != uuid) {
					continue;
				}
				const bool match = c.value("outputWidth", 0u) == 854u &&
						   c.value("outputHeight", 0u) == 480u &&
						   c.value("scaleType", std::string()) == "lanczos" &&
						   c.value("fpsNum", 0u) == 60000u && c.value("fpsDen", 0u) == 1001u;
				HostLog("[selftest] canvas.create output/scale/fps round-trip -> out " +
					std::to_string(c.value("outputWidth", 0u)) + "x" +
					std::to_string(c.value("outputHeight", 0u)) + " scale=" +
					c.value("scaleType", std::string("?")) + " fps=" +
					std::to_string(c.value("fpsNum", 0u)) + "/" +
					std::to_string(c.value("fpsDen", 0u)) + " (" + (match ? "OK" : "MISMATCH") + ")");
				break;
			}
		}
	}

	// Confirm it persisted to disk by reloading a fresh store.
	{
		CanvasStore reloaded;
		reloaded.Load();
		const CanvasDefinition *found = reloaded.Find(uuid);
		HostLog(std::string("[selftest] canvas.create persisted: ") +
			(found ? "FOUND (" + std::to_string(found->width) + "x" + std::to_string(found->height) + "@" +
					 std::to_string(found->fpsNum) + ")"
			       : "MISSING"));
	}

	json updated = run("canvas.update",
			   json{{"uuid", uuid}, {"name", "selftest-renamed"}, {"baseWidth", 1920}, {"baseHeight", 1080}},
			   ok);
	if (ok && updated.is_object()) {
		HostLog("[selftest] canvas.update -> name='" + updated.value("name", std::string("?")) + "' " +
			std::to_string(updated.value("baseWidth", 0u)) + "x" +
			std::to_string(updated.value("baseHeight", 0u)) + " (round-trip " +
			((updated.value("name", std::string()) == "selftest-renamed" &&
			  updated.value("baseWidth", 0u) == 1920u)
				 ? "OK"
				 : "MISMATCH") +
			")");
	}

	// Encoder properties.get through the generic serializer (kind:"encoder").
	json encProps = run("properties.get", json{{"kind", "encoder"}, {"ref", uuid + ":video"}}, ok);
	if (ok && encProps.is_object() && encProps.contains("props")) {
		HostLog("[selftest] properties.get encoder(video) -> " +
			std::to_string(encProps["props"].size()) + " descriptors");
	}

	json removed = run("canvas.remove", json{{"uuid", uuid}}, ok);
	if (ok) {
		HostLog("[selftest] canvas.remove -> removed=" + removed.value("removed", std::string("?")));
	}

	// Confirm the file is back to its original shape (temp canvas gone).
	{
		CanvasStore reloaded;
		reloaded.Load();
		const bool gone = reloaded.Find(uuid) == nullptr;
		HostLog(std::string("[selftest] canvas.remove restored file: ") + (gone ? "OK (temp gone)" : "STILL PRESENT") +
			"; store now " + std::to_string(reloaded.Definitions().size()));
	}
}

void ObsBootstrap::RunStreamProfileBridgeSelfTest()
{
	using Bridge::json;

	auto run = [](const std::string &method, const json &params, bool &ok) -> json {
		json result;
		std::string error;
		ok = Bridge::Dispatch(method, params, result, error);
		if (!ok) {
			HostLog("[selftest] " + method + " FAILED: " + error);
			return json(nullptr);
		}
		return result;
	};

	bool ok = false;

	// 1) streamProfile.list: report the user's real profiles + which is primary.
	json list = run("streamProfile.list", json(nullptr), ok);
	if (ok && list.is_array()) {
		std::string names;
		for (const auto &p : list) {
			names += " '" + p.value("label", std::string("?")) + "'(" + p.value("platform", std::string("?")) +
				 "/" + p.value("service", std::string("?")) + (p.value("isPrimary", false) ? ",primary" : "") +
				 ")";
		}
		HostLog("[selftest] streamProfile.list -> " + std::to_string(list.size()) + " profile(s):" + names);
	}

	// 2) serviceTypes.list: prove a sane set (rtmp_common/rtmp_custom/whip_custom).
	json svcTypes = run("serviceTypes.list", json(nullptr), ok);
	if (ok && svcTypes.is_array()) {
		std::string sample;
		for (const auto &t : svcTypes) {
			sample += " " + t.value("id", std::string("?"));
		}
		HostLog("[selftest] serviceTypes.list -> " + std::to_string(svcTypes.size()) + ":" + sample);
	}

	// 3) create -> update -> setPrimary -> (service properties.get) -> remove
	// round-trip, proving each persists to streams.json and the file ends as it
	// began.
	json created = run("streamProfile.create",
			   json{{"label", "selftest-bridge-profile"},
				{"service", "rtmp_custom"},
				{"settings", json{{"server", "rtmp://selftest.example/app"}, {"key", "selftest-key-1"}}}},
			   ok);
	if (!ok || !created.is_object()) {
		return;
	}
	const std::string uuid = created.value("uuid", std::string());
	HostLog("[selftest] streamProfile.create -> uuid=" + uuid);

	// Confirm it persisted to disk by reloading a fresh store.
	{
		StreamProfileStore reloaded;
		reloaded.Load();
		const StreamProfile *found = reloaded.Find(uuid);
		HostLog(std::string("[selftest] streamProfile.create persisted: ") +
			(found ? "FOUND label='" + found->label + "' key='" + found->Key() + "'" : "MISSING"));
	}

	// 3b) Duplicate guard: a second create with the SAME stream key must be rejected.
	{
		json dupResult;
		std::string dupError;
		const bool dupOk =
			Bridge::Dispatch("streamProfile.create",
					 json{{"label", "selftest-dup"},
					      {"service", "rtmp_custom"},
					      {"settings", json{{"server", "rtmp://other.example/app"},
								{"key", "selftest-key-1"}}}},
					 dupResult, dupError);
		HostLog(std::string("[selftest] duplicate-key create -> ") +
			(dupOk ? "ACCEPTED (BUG: should reject)" : "REJECTED (\"" + dupError + "\")"));
	}

	json updated = run("streamProfile.update",
			   json{{"uuid", uuid}, {"label", "selftest-renamed"}, {"settings", json{{"key", "selftest-key-2"}}}},
			   ok);
	if (ok && updated.is_object()) {
		HostLog("[selftest] streamProfile.update -> label='" + updated.value("label", std::string("?")) +
			"' (round-trip " +
			(updated.value("label", std::string()) == "selftest-renamed" ? "OK" : "MISMATCH") + ")");
	}

	json primary = run("streamProfile.setPrimary", json{{"uuid", uuid}}, ok);
	if (ok) {
		HostLog("[selftest] streamProfile.setPrimary -> isPrimary=" +
			std::string(primary.value("isPrimary", false) ? "true" : "false"));
	}

	// Service properties.get through the generic serializer (kind:"service").
	json svcProps = run("properties.get", json{{"kind", "service"}, {"ref", uuid}}, ok);
	if (ok && svcProps.is_object() && svcProps.contains("props")) {
		HostLog("[selftest] properties.get service -> " + std::to_string(svcProps["props"].size()) +
			" descriptors");
	}

	json removed = run("streamProfile.remove", json{{"uuid", uuid}}, ok);
	if (ok) {
		HostLog("[selftest] streamProfile.remove -> removed=" + removed.value("removed", std::string("?")));
	}

	// Confirm the file is back to its original shape (temp profile gone).
	{
		StreamProfileStore reloaded;
		reloaded.Load();
		const bool gone = reloaded.Find(uuid) == nullptr;
		const StreamProfile *primaryProfile = reloaded.Primary();
		HostLog(std::string("[selftest] streamProfile.remove restored file: ") +
			(gone ? "OK (temp gone)" : "STILL PRESENT") + "; store now " +
			std::to_string(reloaded.Profiles().size()) +
			", primary=" + (primaryProfile ? primaryProfile->DisplayName() : "(none)"));
	}
}

void ObsBootstrap::RunOutputBindingBridgeSelfTest()
{
	using Bridge::json;

	auto run = [](const std::string &method, const json &params, bool &ok) -> json {
		json result;
		std::string error;
		ok = Bridge::Dispatch(method, params, result, error);
		if (!ok) {
			HostLog("[selftest] " + method + " FAILED: " + error);
			return json(nullptr);
		}
		return result;
	};

	bool ok = false;

	// 1) outputBinding.list: report the user's real bindings, joined to names.
	json list = run("outputBinding.list", json(nullptr), ok);
	if (ok && list.is_array()) {
		std::string rows;
		for (const auto &b : list) {
			rows += " [" + b.value("profileLabel", std::string("?")) + " -> " +
				b.value("canvasName", std::string("?")) +
				(b.value("enabled", false) ? ",on" : ",off") + "]";
		}
		HostLog("[selftest] outputBinding.list -> " + std::to_string(list.size()) + " binding(s):" + rows);
	}

	// A binding needs a real canvas (the Default always exists). A profile is
	// optional, but bind a real one if the user has any so the join is exercised.
	const std::string canvasUuid = g_canvases.Default().uuid;
	std::string profileUuid;
	if (!g_streamProfiles.Profiles().empty()) {
		profileUuid = g_streamProfiles.Profiles().front().uuid;
	}

	// 2) create -> setEnabled -> update -> remove round-trip, proving each persists
	// to output_bindings.json and the file ends as it began.
	json createParams = json{{"canvasUuid", canvasUuid}};
	if (!profileUuid.empty()) {
		createParams["profileUuid"] = profileUuid;
	}
	json created = run("outputBinding.create", createParams, ok);
	if (!ok || !created.is_object()) {
		return;
	}
	const std::string uuid = created.value("uuid", std::string());
	HostLog("[selftest] outputBinding.create -> uuid=" + uuid + " (canvas=" + canvasUuid +
		", profile=" + (profileUuid.empty() ? "(unset)" : profileUuid) + ")");

	// Confirm it persisted to disk by reloading a fresh store + that the live list
	// joins it to the right names.
	{
		OutputBindingStore reloaded;
		reloaded.Load();
		const bool found = reloaded.Bindings().Find(uuid) != nullptr;
		HostLog(std::string("[selftest] outputBinding.create persisted: ") + (found ? "FOUND" : "MISSING"));
	}
	{
		json relist = run("outputBinding.list", json(nullptr), ok);
		if (ok && relist.is_array()) {
			for (const auto &b : relist) {
				if (b.value("uuid", std::string()) == uuid) {
					HostLog("[selftest] outputBinding.list join -> profileLabel='" +
						b.value("profileLabel", std::string("?")) + "' canvasName='" +
						b.value("canvasName", std::string("?")) + "'");
				}
			}
		}
	}

	// 2b) Duplicate guard: a second create with the SAME (profile x canvas) pair
	// must be rejected.
	{
		json dupResult;
		std::string dupError;
		const bool dupOk = Bridge::Dispatch("outputBinding.create", createParams, dupResult, dupError);
		HostLog(std::string("[selftest] duplicate-pair create -> ") +
			(dupOk ? "ACCEPTED (BUG: should reject)" : "REJECTED (\"" + dupError + "\")"));
	}

	// setEnabled(true) -> AnyEnabledForCanvas must flip on for this canvas.
	json enabled = run("outputBinding.setEnabled", json{{"uuid", uuid}, {"enabled", true}}, ok);
	if (ok) {
		const bool any = g_outputBindings.Bindings().AnyEnabledForCanvas(canvasUuid);
		HostLog("[selftest] outputBinding.setEnabled(true) -> enabled=" +
			std::string(enabled.value("enabled", false) ? "true" : "false") +
			"; AnyEnabledForCanvas=" + (any ? "true" : "false"));
	}

	// update: re-point to "(unset)" profile, confirming the join reflects the change.
	json updated = run("outputBinding.update", json{{"uuid", uuid}, {"profileUuid", std::string()}}, ok);
	if (ok && updated.is_object()) {
		HostLog("[selftest] outputBinding.update -> profileLabel='" +
			updated.value("profileLabel", std::string("?")) + "' (expect '(unset)')");
	}

	// setEnabled(false) -> AnyEnabledForCanvas flips back off (no other binding).
	run("outputBinding.setEnabled", json{{"uuid", uuid}, {"enabled", false}}, ok);
	if (ok) {
		const bool any = g_outputBindings.Bindings().AnyEnabledForCanvas(canvasUuid);
		HostLog(std::string("[selftest] outputBinding.setEnabled(false) -> AnyEnabledForCanvas=") +
			(any ? "true" : "false"));
	}

	json removed = run("outputBinding.remove", json{{"uuid", uuid}}, ok);
	if (ok) {
		HostLog("[selftest] outputBinding.remove -> removed=" + removed.value("removed", std::string("?")));
	}

	// Confirm the file is back to its original shape (temp binding gone).
	{
		OutputBindingStore reloaded;
		reloaded.Load();
		const bool gone = reloaded.Bindings().Find(uuid) == nullptr;
		HostLog(std::string("[selftest] outputBinding.remove restored file: ") +
			(gone ? "OK (temp gone)" : "STILL PRESENT") + "; store now " +
			std::to_string(reloaded.Bindings().bindings.size()));
	}
}

void ObsBootstrap::RunMultistreamModelSelfTest()
{
	// Canvas round-trip: add a temporary canvas to the LIVE store, Save to the real
	// canvases.json, reload into a FRESH store, confirm it persisted, then remove +
	// Save so the user's file ends exactly as it began.
	{
		const size_t before = g_canvases.Definitions().size();
		CanvasDefinition tmp;
		tmp.name = "selftest-canvas";
		tmp.width = 1280;
		tmp.height = 720;
		const std::string uuid = g_canvases.Add(std::move(tmp)).uuid;
		g_canvases.Save();

		CanvasStore reloaded;
		reloaded.Load();
		const CanvasDefinition *found = reloaded.Find(uuid);
		HostLog(std::string("[selftest] canvas round-trip: reloaded ") +
			std::to_string(reloaded.Definitions().size()) + " (was " + std::to_string(before) +
			"+1); selftest-canvas " + (found ? "FOUND" : "MISSING") +
			(found ? " (" + std::to_string(found->width) + "x" + std::to_string(found->height) + ")" : ""));

		g_canvases.Remove(uuid);
		g_canvases.Save();
		HostLog("[selftest] canvas round-trip: removed temp canvas, store back to " +
			std::to_string(g_canvases.Definitions().size()));
	}

	// Stream-profile round-trip: same pattern against streams.json.
	{
		const size_t before = g_streamProfiles.Profiles().size();
		StreamProfile tmp;
		tmp.label = "selftest-profile";
		tmp.serviceId = "rtmp_custom";
		const std::string uuid = g_streamProfiles.Add(std::move(tmp)).uuid;
		g_streamProfiles.Save();

		StreamProfileStore reloaded;
		reloaded.Load();
		const StreamProfile *found = reloaded.Find(uuid);
		HostLog(std::string("[selftest] profile round-trip: reloaded ") +
			std::to_string(reloaded.Profiles().size()) + " (was " + std::to_string(before) +
			"+1); selftest-profile " + (found ? "FOUND" : "MISSING") +
			(found ? " label='" + found->label + "'" : ""));

		g_streamProfiles.Remove(uuid);
		g_streamProfiles.Save();
		HostLog("[selftest] profile round-trip: removed temp profile, store back to " +
			std::to_string(g_streamProfiles.Profiles().size()));
	}

	// Output-binding round-trip: add a binding for the Default canvas, Save, reload,
	// confirm, then clear + Save back to the original on-disk state.
	{
		const std::string canvasUuid = g_canvases.Default().uuid;
		const size_t before = g_outputBindings.Bindings().bindings.size();
		const std::string uuid = g_outputBindings.Bindings().Add(canvasUuid).uuid;
		g_outputBindings.Save();

		OutputBindingStore reloaded;
		reloaded.Load();
		const bool found = reloaded.Bindings().Find(uuid) != nullptr;
		HostLog(std::string("[selftest] binding round-trip: reloaded ") +
			std::to_string(reloaded.Bindings().bindings.size()) + " (was " + std::to_string(before) +
			"+1); temp binding " + (found ? "FOUND" : "MISSING"));

		g_outputBindings.Bindings().Remove(uuid);
		g_outputBindings.Save();
		HostLog("[selftest] binding round-trip: removed temp binding, store back to " +
			std::to_string(g_outputBindings.Bindings().bindings.size()));
	}
}

void ObsBootstrap::RunMultistreamEngineSelfTest()
{
	// Drive the fan-out engine end-to-end without a real broadcast: create a temp
	// profile pointed at a dead local RTMP host + a temp enabled binding on the
	// Default canvas, start the output, then stop it. Operate ONLY on the in-memory
	// stores (never Save), so the user's files are untouched and the model returns
	// to baseline at the end.
	const std::string canvasUuid = g_canvases.Default().uuid;
	const size_t profilesBefore = g_streamProfiles.Profiles().size();
	const size_t bindingsBefore = g_outputBindings.Bindings().bindings.size();

	StreamProfile prof;
	prof.label = "selftest-engine";
	prof.serviceId = "rtmp_custom";
	prof.settings = OBSDataAutoRelease(obs_data_create());
	obs_data_set_string(prof.settings, "server", "rtmp://127.0.0.1:1/live");
	obs_data_set_string(prof.settings, "key", "selftest");
	const std::string profileUuid = g_streamProfiles.Add(std::move(prof)).uuid;

	OutputBinding &binding = g_outputBindings.Bindings().Add(canvasUuid);
	binding.profileUuid = profileUuid;
	binding.enabled = true;
	const std::string bindingUuid = binding.uuid;

	// Start: connecting to a dead host stays Connecting or flips Error -- either is
	// valid proof the start path ran (encoders built, output+service created).
	const bool started = g_multistream->StartOutput(bindingUuid);
	const bool canvasLive = g_multistream->IsCanvasLive(canvasUuid);
	std::vector<MultistreamEngine::OutputStatus> statuses = g_multistream->Statuses();
	const std::string firstState =
		statuses.empty() ? "(none)" : MultistreamEngine::StateName(statuses.front().state);
	HostLog(std::string("[selftest] engine StartOutput -> ") + (started ? "true" : "false") +
		"; IsCanvasLive(default)=" + (canvasLive ? "true" : "false") + "; first status state=" + firstState +
		" (" + std::to_string(statuses.size()) + " enabled)");

	// Stop: the output must drop out of the live set for both the binding and canvas.
	g_multistream->StopOutput(bindingUuid);
	const bool stillLive = g_multistream->IsLive(bindingUuid);
	const bool stillCanvasLive = g_multistream->IsCanvasLive(canvasUuid);
	HostLog(std::string("[selftest] engine StopOutput -> IsLive=") + (stillLive ? "true (BUG)" : "false") +
		"; IsCanvasLive(default)=" + (stillCanvasLive ? "true (BUG)" : "false"));

	// Restore the model to baseline (in-memory only; nothing was Saved).
	g_outputBindings.Bindings().Remove(bindingUuid);
	g_streamProfiles.Remove(profileUuid);
	HostLog("[selftest] engine cleanup: profiles " + std::to_string(g_streamProfiles.Profiles().size()) +
		" (was " + std::to_string(profilesBefore) + "), bindings " +
		std::to_string(g_outputBindings.Bindings().bindings.size()) + " (was " +
		std::to_string(bindingsBefore) + ")");
}

void ObsBootstrap::RunCanvasRuntimeSelfTest()
{
	// Prove an ADDITIONAL canvas (not the Default) now encodes: bring up its live
	// obs_canvas_t mix, confirm the uuid is preserved so the engine resolver can
	// match it, then drive StartOutput end-to-end. Operate ONLY on the in-memory
	// stores (never Save) so the user's files stay untouched and the model returns
	// to baseline. The temp canvas leaves its encoder ids empty on purpose: the
	// engine falls back to the Default canvas's encoders, which are already seeded.
	CanvasDefinition def;
	def.name = "selftest-runtime-canvas";
	def.isDefault = false;
	def.width = 1280;
	def.height = 720;
	def.fpsNum = 30;
	def.fpsDen = 1;
	const CanvasDefinition &added = g_canvases.Add(std::move(def));
	const std::string canvasUuid = added.uuid;
	g_canvasRuntime->EnsureCanvas(added);

	obs_canvas_t *canvas = g_canvasRuntime->Find(canvasUuid);
	video_t *video = g_canvasRuntime->VideoFor(canvasUuid);
	const char *liveUuid = canvas ? obs_canvas_get_uuid(canvas) : nullptr;
	const bool uuidMatches = liveUuid && canvasUuid == liveUuid;
	HostLog(std::string("[selftest] canvas-runtime EnsureCanvas -> Find=") + (canvas ? "ok" : "null (BUG)") +
		"; VideoFor=" + (video ? "ok" : "null (BUG)") + "; uuid " + (uuidMatches ? "preserved" : "MISMATCH (BUG)") +
		" (" + canvasUuid + " vs " + (liveUuid ? liveUuid : "(null)") + ")");

	StreamProfile prof;
	prof.label = "selftest-runtime";
	prof.serviceId = "rtmp_custom";
	prof.settings = OBSDataAutoRelease(obs_data_create());
	obs_data_set_string(prof.settings, "server", "rtmp://127.0.0.1:1/live");
	obs_data_set_string(prof.settings, "key", "selftest");
	const std::string profileUuid = g_streamProfiles.Add(std::move(prof)).uuid;

	OutputBinding &binding = g_outputBindings.Bindings().Add(canvasUuid);
	binding.profileUuid = profileUuid;
	binding.enabled = true;
	const std::string bindingUuid = binding.uuid;

	// Start: this could only return true if the additional canvas has a real mix
	// (the resolver returns it) so the engine could bind encoders to it. Before the
	// runtime layer the resolver returned null here and StartOutput refused.
	const bool started = g_multistream->StartOutput(bindingUuid);
	const bool canvasLive = g_multistream->IsCanvasLive(canvasUuid);
	HostLog(std::string("[selftest] canvas-runtime StartOutput -> ") + (started ? "true" : "false (BUG)") +
		"; IsCanvasLive(additional)=" + (canvasLive ? "true" : "false (BUG)"));

	g_multistream->StopOutput(bindingUuid);
	const bool stillLive = g_multistream->IsCanvasLive(canvasUuid);
	HostLog(std::string("[selftest] canvas-runtime StopOutput -> IsCanvasLive=") +
		(stillLive ? "true (BUG)" : "false"));

	// Restore the model to baseline (in-memory only; nothing was Saved). Drop the
	// engine's cached encoders for the temp canvas before its mix goes away.
	g_outputBindings.Bindings().Remove(bindingUuid);
	g_streamProfiles.Remove(profileUuid);
	g_multistream->InvalidateCanvasEncoders(canvasUuid);
	g_canvasRuntime->RemoveCanvas(canvasUuid);
	g_canvases.Remove(canvasUuid);
	const bool gone = g_canvasRuntime->Find(canvasUuid) == nullptr && g_canvases.Find(canvasUuid) == nullptr;
	HostLog(std::string("[selftest] canvas-runtime cleanup: temp canvas ") + (gone ? "removed" : "STILL PRESENT (BUG)") +
		"; canvases now " + std::to_string(g_canvases.Definitions().size()));
}

void ObsBootstrap::RunCanvasSceneSelfTest()
{
	using Bridge::json;

	auto run = [](const std::string &method, const json &params, bool &ok) -> json {
		json result;
		std::string error;
		ok = Bridge::Dispatch(method, params, result, error);
		if (!ok) {
			HostLog("[selftest] " + method + " FAILED: " + error);
			return json(nullptr);
		}
		return result;
	};

	// Bring up a temporary ADDITIONAL canvas with its own live mix (+ a default
	// channel-0 "Scene"), exactly like the canvas-runtime selftest. Operate ONLY on
	// the in-memory stores (never Save) so the user's files stay untouched. The
	// point is to prove the bridge's scene/source ops, when given this canvas's uuid,
	// act on the canvas's OWN scenes -- isolated from the global channel-0 scene list.
	CanvasDefinition def;
	def.name = "selftest-scene-canvas";
	def.isDefault = false;
	def.width = 1280;
	def.height = 720;
	def.fpsNum = 30;
	def.fpsDen = 1;
	const CanvasDefinition &added = g_canvases.Add(std::move(def));
	const std::string canvasUuid = added.uuid;
	g_canvasRuntime->EnsureCanvas(added);

	bool ok = false;

	// 1) Create a scene inside the canvas via the canvas-scoped bridge path.
	const char *kSceneName = "selftest-canvas-scene";
	json created = run("scenes.create", json{{"canvas", canvasUuid}, {"name", kSceneName}}, ok);
	HostLog(std::string("[selftest] canvas-scene scenes.create -> ") +
		(ok ? "ok name='" + created.value("name", std::string("?")) + "'" : "FAIL"));

	// 2) List the canvas's scenes (expect the default "Scene" + our new one) and the
	// GLOBAL scenes (expect our scene ABSENT -- proof of isolation).
	json canvasScenes = run("scenes.list", json{{"canvas", canvasUuid}}, ok);
	bool inCanvasList = false;
	std::string canvasNames;
	if (ok && canvasScenes.is_array()) {
		for (const auto &s : canvasScenes) {
			canvasNames += " '" + s.value("name", std::string("?")) + "'";
			if (s.value("name", std::string()) == kSceneName) {
				inCanvasList = true;
			}
		}
	}
	json globalScenes = run("scenes.list", json(nullptr), ok);
	bool inGlobalList = false;
	if (ok && globalScenes.is_array()) {
		for (const auto &s : globalScenes) {
			if (s.value("name", std::string()) == kSceneName) {
				inGlobalList = true;
			}
		}
	}
	HostLog(std::string("[selftest] canvas-scene scenes.list -> canvas has scene=") +
		(inCanvasList ? "true" : "false (BUG)") + " (" + std::to_string(canvasScenes.size()) + ":" + canvasNames +
		" ); global has scene=" + (inGlobalList ? "true (BUG: not isolated)" : "false") + " (isolation " +
		((inCanvasList && !inGlobalList) ? "OK" : "BUG") + ")");

	// 3) Make it the canvas's current scene (its channel 0, NOT output 0).
	json setCur = run("scenes.setCurrent", json{{"canvas", canvasUuid}, {"name", kSceneName}}, ok);
	HostLog(std::string("[selftest] canvas-scene scenes.setCurrent -> ") + (ok ? "ok" : "FAIL"));

	// 4) Add a source via the canvas-scoped path; assert it lands in the CANVAS's
	// current scene, NOT the global output-0 scene.
	json srcCreated = run("sources.create", json{{"canvas", canvasUuid}, {"type", "color_source"}}, ok);
	const int64_t newItemId = ok ? srcCreated.value("id", int64_t(0)) : 0;
	const std::string newSrcName = ok ? srcCreated.value("source", std::string()) : std::string();
	HostLog(std::string("[selftest] canvas-scene sources.create -> ") +
		(ok ? "id=" + std::to_string(newItemId) + " source='" + newSrcName + "'" : "FAIL"));

	// Count the source's presence in the canvas's current scene items vs the global
	// output-0 scene items by the bridge's own list methods.
	auto sceneHasItem = [&](const json &listParams) -> bool {
		bool listOk = false;
		json items = run("sceneItems.list", listParams, listOk);
		if (!listOk || !items.is_array()) {
			return false;
		}
		for (const auto &it : items) {
			if (it.value("source", std::string()) == newSrcName) {
				return true;
			}
		}
		return false;
	};
	const bool inCanvasScene = sceneHasItem(json{{"canvas", canvasUuid}});
	const bool inGlobalScene = sceneHasItem(json(nullptr));
	HostLog(std::string("[selftest] canvas-scene source placement -> in canvas scene=") +
		(inCanvasScene ? "true" : "false (BUG)") + "; in global scene=" +
		(inGlobalScene ? "true (BUG: leaked to output 0)" : "false") + " (placement " +
		((inCanvasScene && !inGlobalScene) ? "OK" : "BUG") + ")");

	// Clean up: remove the source from the canvas scene, then destroy the temp
	// canvas + drop its mix, returning the in-memory model to baseline (nothing
	// Saved). Destroying the canvas releases its scenes (including our created ones).
	if (newItemId) {
		run("sceneItems.remove", json{{"canvas", canvasUuid}, {"id", newItemId}}, ok);
		obs_source_t *s = obs_get_source_by_name(newSrcName.c_str());
		if (s) {
			obs_source_remove(s);
			obs_source_release(s);
		}
	}
	g_multistream->InvalidateCanvasEncoders(canvasUuid);
	g_canvasRuntime->RemoveCanvas(canvasUuid);
	g_canvases.Remove(canvasUuid);
	const bool gone = g_canvasRuntime->Find(canvasUuid) == nullptr && g_canvases.Find(canvasUuid) == nullptr;
	HostLog(std::string("[selftest] canvas-scene cleanup: temp canvas ") + (gone ? "removed" : "STILL PRESENT (BUG)") +
		"; canvases now " + std::to_string(g_canvases.Definitions().size()));
}

void ObsBootstrap::RunPreviewSurfaceIsolationSelfTest()
{
	using Bridge::json;

	if (!Preview::Instance()) {
		HostLog("[selftest] preview-isolation: no preview manager (skipped)");
		return;
	}

	auto run = [](const std::string &method, const json &params, bool &ok) -> json {
		json result;
		std::string error;
		ok = Bridge::Dispatch(method, params, result, error);
		if (!ok) {
			HostLog("[selftest] " + method + " FAILED: " + error);
			return json(nullptr);
		}
		return result;
	};

	// Bring up a temporary ADDITIONAL canvas with its own live mix (+ a default
	// channel-0 "Scene"), like the canvas-scene selftest. In-memory only (never
	// Save). The point: an edit driven on this canvas's preview surface must touch
	// ONLY that surface's state, leaving the Default surface + output-0 scene alone.
	CanvasDefinition def;
	def.name = "selftest-isolation-canvas";
	def.isDefault = false;
	def.width = 1280;
	def.height = 720;
	def.fpsNum = 30;
	def.fpsDen = 1;
	const CanvasDefinition &added = g_canvases.Add(std::move(def));
	const std::string canvasUuid = added.uuid;
	g_canvasRuntime->EnsureCanvas(added);

	bool ok = false;

	// Add a source to the canvas's current scene so there is an item to hit-test.
	json srcCreated = run("sources.create", json{{"canvas", canvasUuid}, {"type", "color_source"}}, ok);
	const int64_t canvasItemId = ok ? srcCreated.value("id", int64_t(0)) : 0;
	const std::string canvasSrcName = ok ? srcCreated.value("source", std::string()) : std::string();
	HostLog(std::string("[selftest] preview-isolation: canvas source -> ") +
		(ok ? "id=" + std::to_string(canvasItemId) + " '" + canvasSrcName + "'" : "FAIL"));

	// Snapshot the Default surface's selection BEFORE touching the additional
	// surface, so we can prove it is unchanged afterward. (The preview-edit selftest
	// ran earlier and cleared it, so this should already be -1.)
	PreviewSurface *defaultSurface = Preview::Instance()->SurfaceFor("");
	const int64_t defaultSelBefore = defaultSurface ? defaultSurface->SelectedIdForTest() : -2;

	// Address the additional canvas's surface; it shares no state with the Default.
	PreviewSurface *canvasSurface = Preview::Instance()->SurfaceFor(canvasUuid);
	HostLog(std::string("[selftest] preview-isolation: additional surface -> ") +
		(canvasSurface ? "ok" : "NULL (BUG)"));

	// 1) Hit-test the canvas item at its box-transform center, against the ADDITIONAL
	// surface. It must find the canvas item (proof the surface resolves the canvas's
	// own scene, not output 0).
	int64_t hitOnCanvas = -1;
	if (canvasSurface) {
		obs_source_t *canvasScene = g_canvasRuntime->CurrentScene(canvasUuid); // addref'd
		if (canvasScene) {
			struct First {
				obs_sceneitem_t *item;
			} fctx{nullptr};
			obs_scene_enum_items(
				obs_scene_from_source(canvasScene),
				[](obs_scene_t *, obs_sceneitem_t *item, void *p) -> bool {
					static_cast<First *>(p)->item = item;
					return false;
				},
				&fctx);
			if (fctx.item) {
				matrix4 boxTransform;
				obs_sceneitem_get_box_transform(fctx.item, &boxTransform);
				vec3 center;
				vec3_set(&center, 0.5f, 0.5f, 0.0f);
				vec3_transform(&center, &center, &boxTransform);
				hitOnCanvas = Preview::HitTestForTest(canvasUuid, center.x, center.y);
			}
			obs_source_release(canvasScene);
		}
	}
	HostLog(std::string("[selftest] preview-isolation: hit-test on additional surface -> id=") +
		std::to_string(hitOnCanvas) + (hitOnCanvas == canvasItemId ? " (match)" : " (MISMATCH)"));

	// 2) Select the canvas item on the ADDITIONAL surface. Its selection state must
	// flip; the Default surface's must NOT.
	const bool selOk = Preview::SelectFromBridge(canvasUuid, "", canvasItemId, true);
	const int64_t canvasSel = canvasSurface ? canvasSurface->SelectedIdForTest() : -2;
	const int64_t defaultSelAfter = defaultSurface ? defaultSurface->SelectedIdForTest() : -2;
	HostLog(std::string("[selftest] preview-isolation: select on additional -> ") + (selOk ? "ok" : "FAIL") +
		"; additional sel=" + std::to_string(canvasSel) + " (expect " + std::to_string(canvasItemId) + ")" +
		"; default sel before=" + std::to_string(defaultSelBefore) + " after=" + std::to_string(defaultSelAfter) +
		" (isolation " +
		((canvasSel == canvasItemId && defaultSelAfter == defaultSelBefore) ? "OK" : "BUG: bled into Default") +
		")");

	// 3) Move the canvas item; assert the global output-0 scene's items are
	// unaffected (the move went to the canvas scene, not the program scene).
	bool movedOk = false;
	if (canvasItemId) {
		obs_source_t *canvasScene = g_canvasRuntime->CurrentScene(canvasUuid); // addref'd
		if (canvasScene) {
			obs_scene_t *sc = obs_scene_from_source(canvasScene);
			obs_sceneitem_t *item = nullptr;
			struct FindCtx {
				int64_t id;
				obs_sceneitem_t *found;
			} fc{canvasItemId, nullptr};
			obs_scene_enum_items(
				sc,
				[](obs_scene_t *, obs_sceneitem_t *it, void *p) -> bool {
					auto *c = static_cast<FindCtx *>(p);
					if (obs_sceneitem_get_id(it) == c->id) {
						c->found = it;
						return false;
					}
					return true;
				},
				&fc);
			item = fc.found;
			if (item) {
				vec2 orig;
				obs_sceneitem_get_pos(item, &orig);
				vec2 moved;
				vec2_set(&moved, orig.x + 40.0f, orig.y + 25.0f);
				obs_sceneitem_set_pos(item, &moved);
				vec2 after;
				obs_sceneitem_get_pos(item, &after);
				movedOk = int(after.x) == int(orig.x + 40.0f) && int(after.y) == int(orig.y + 25.0f);
				obs_sceneitem_set_pos(item, &orig); // restore
			}
			obs_source_release(canvasScene);
		}
	}
	HostLog(std::string("[selftest] preview-isolation: canvas item move -> ") + (movedOk ? "ok (restored)" : "FAIL"));

	// Clear the additional surface's selection so it leaves no committed state, then
	// tear down the temp canvas (drops its surface's mix; the surface itself is
	// reaped by DestroyAll at shutdown, but its display already has no mix to render
	// once the canvas is gone -- so destroy the surface now to keep ordering clean).
	Preview::SelectFromBridge(canvasUuid, "", 0, false);
	Preview::Instance()->Destroy(canvasUuid);

	if (canvasItemId) {
		obs_source_t *s = obs_get_source_by_name(canvasSrcName.c_str());
		if (s) {
			obs_source_remove(s);
			obs_source_release(s);
		}
	}
	g_multistream->InvalidateCanvasEncoders(canvasUuid);
	g_canvasRuntime->RemoveCanvas(canvasUuid);
	g_canvases.Remove(canvasUuid);
	const bool gone = g_canvasRuntime->Find(canvasUuid) == nullptr && g_canvases.Find(canvasUuid) == nullptr;
	HostLog(std::string("[selftest] preview-isolation cleanup: temp canvas ") +
		(gone ? "removed" : "STILL PRESENT (BUG)"));
}

void ObsBootstrap::RunProjectorSelfTest()
{
	if (!Projector::Instance()) {
		HostLog("[selftest] projector: no manager (skipped)");
		return;
	}

	HostLog("[selftest] projector display.listMonitors -> " + std::to_string(EnumerateMonitors().size()) +
		" monitor(s)");

	// Open a windowed PROGRAM projector directly via the manager (windowed needs no
	// monitor index, so this works headlessly). Confirm it got a live display, then
	// close it so the run leaves no state behind.
	std::string error;
	const int id = Projector::Instance()->Open(ProjectorKind::Program, "", "", /*fullscreen=*/false,
						   /*monitor=*/-1, error);
	if (id <= 0) {
		HostLog("[selftest] projector windowed program -> FAILED: " + error);
		return;
	}
	const bool hasDisplay = Projector::Instance()->HasDisplayForTest(id);
	const bool closed = Projector::Instance()->Close(id);
	if (!hasDisplay || !closed) {
		HostLog("[selftest] projector windowed program -> FAILED (hasDisplay=" + std::to_string(hasDisplay) +
			" closed=" + std::to_string(closed) + ")");
		return;
	}
	HostLog("[selftest] projector windowed program -> opened id=" + std::to_string(id) + ", closed OK");

	// Open a windowed MULTIVIEW projector for the Default canvas (empty uuid =
	// global scenes; windowed needs no monitor index, so it works headlessly). This
	// exercises the scene-snapshot build + label creation + ordered teardown.
	std::string mvError;
	const int mvId = Projector::Instance()->Open(ProjectorKind::Multiview, "", "", /*fullscreen=*/false,
						     /*monitor=*/-1, mvError);
	if (mvId <= 0) {
		HostLog("[selftest] projector windowed multiview -> FAILED: " + mvError);
		return;
	}
	const bool mvHasDisplay = Projector::Instance()->HasDisplayForTest(mvId);
	const bool mvClosed = Projector::Instance()->Close(mvId);
	if (!mvHasDisplay || !mvClosed) {
		HostLog("[selftest] projector windowed multiview -> FAILED (hasDisplay=" +
			std::to_string(mvHasDisplay) + " closed=" + std::to_string(mvClosed) + ")");
		return;
	}
	HostLog("[selftest] projector windowed multiview -> opened id=" + std::to_string(mvId) + ", closed OK");
}

void ObsBootstrap::RunAudioMixerSelfTest()
{
	using Bridge::json;

	auto run = [](const std::string &method, const json &params, bool &ok) -> json {
		json result;
		std::string error;
		ok = Bridge::Dispatch(method, params, result, error);
		if (!ok) {
			HostLog("[selftest] " + method + " FAILED: " + error);
			return json(nullptr);
		}
		return result;
	};

	bool ok = false;

	// 1) audio.list against the steady state. The default color source has no
	// audio, so this may legitimately be empty until a temp subject is added.
	json list0 = run("audio.list", json(nullptr), ok);
	const size_t baseCount = (ok && list0.is_object() && list0["sources"].is_array()) ? list0["sources"].size() : 0;
	HostLog("[selftest] audio-mixer audio.list (baseline) -> " + std::to_string(baseCount) + " source(s)");

	// 2) Add a temporary audio-capable source (desktop audio) bound to a free
	// global output channel so it activates, then rebuild the monitor so the new
	// source picks up a fader + volmeter. wasapi_output_capture is a synchronous
	// audio source: audio_active stays true even without a live device.
	constexpr int kTempChannel = 6; // high channel, unlikely bound by the bootstrap
	OBSSourceAutoRelease prior = obs_get_output_source(kTempChannel); // save to restore
	obs_source_t *audioSrc = obs_source_create("wasapi_output_capture", "selftest-audio", nullptr, nullptr);
	if (!audioSrc) {
		HostLog("[selftest] audio-mixer: wasapi_output_capture create FAILED (skipping)");
		return;
	}
	obs_set_output_source(kTempChannel, audioSrc);

	const char *uuidPtr = obs_source_get_uuid(audioSrc);
	const std::string uuid = uuidPtr ? uuidPtr : std::string();
	const bool audioActive = obs_source_audio_active(audioSrc);
	HostLog("[selftest] audio-mixer temp source created -> uuid=" + uuid + " audioActive=" +
		(audioActive ? "true" : "false"));

	g_audioMonitor->Rebuild();

	// 3) audio.list now includes the temp source (proof the monitor attached a
	// fader+volmeter to it -- List() reads the fader's deflection/dB).
	json list1 = run("audio.list", json(nullptr), ok);
	bool found = false;
	float listedDef = -1.0f;
	if (ok && list1.is_object() && list1["sources"].is_array()) {
		for (const auto &s : list1["sources"]) {
			if (s.value("uuid", std::string()) == uuid) {
				found = true;
				listedDef = s.value("deflection", -1.0f);
			}
		}
	}
	HostLog("[selftest] audio-mixer audio.list (with temp) -> " +
		std::to_string(list1.is_object() && list1["sources"].is_array() ? list1["sources"].size() : 0) +
		" source(s); temp present=" + (found ? "true (volmeter attached)" : "false (BUG)") +
		" deflection=" + std::to_string(listedDef));

	// 4) setDeflection round-trip: set ~0.5, read it back from the response.
	json setDef = run("audio.setDeflection", json{{"uuid", uuid}, {"deflection", 0.5}}, ok);
	if (ok && setDef.is_object()) {
		const float appliedDef = setDef.value("deflection", -1.0f);
		HostLog("[selftest] audio-mixer setDeflection(0.5) -> deflection=" + std::to_string(appliedDef) +
			" volumeDb=" + std::to_string(setDef.value("volumeDb", 0.0f)) + " (round-trip " +
			(appliedDef > 0.45f && appliedDef < 0.55f ? "OK" : "MISMATCH") + ")");
	}

	// 5) setMuted round-trip: mute, confirm via obs_source_muted, then unmute.
	json setMuted = run("audio.setMuted", json{{"uuid", uuid}, {"muted", true}}, ok);
	if (ok) {
		obs_source_t *check = obs_get_source_by_uuid(uuid.c_str());
		const bool reallyMuted = check && obs_source_muted(check);
		if (check) {
			obs_source_release(check);
		}
		HostLog(std::string("[selftest] audio-mixer setMuted(true) -> ") +
			(setMuted.value("muted", false) ? "reported muted" : "reported unmuted") +
			"; obs_source_muted=" + (reallyMuted ? "true (round-trip OK)" : "false (MISMATCH)"));
	}
	run("audio.setMuted", json{{"uuid", uuid}, {"muted", false}}, ok);

	// 6) Restore: unbind the channel (or its prior source), remove + release the
	// temp source, then rebuild so the monitor drops its entry. Leaves no state.
	obs_set_output_source(kTempChannel, prior); // null or the prior source
	obs_source_remove(audioSrc);
	obs_source_release(audioSrc);
	g_audioMonitor->Rebuild();
	json list2 = run("audio.list", json(nullptr), ok);
	const size_t finalCount =
		(ok && list2.is_object() && list2["sources"].is_array()) ? list2["sources"].size() : 0;
	HostLog("[selftest] audio-mixer cleanup -> audio.list back to " + std::to_string(finalCount) +
		" source(s) (was " + std::to_string(baseCount) + ")");
}

void ObsBootstrap::RunHotkeysSelfTest()
{
	using Bridge::json;

	auto run = [](const std::string &method, const json &params, bool &ok) -> json {
		json result;
		std::string error;
		ok = Bridge::Dispatch(method, params, result, error);
		if (!ok) {
			HostLog("[selftest] " + method + " FAILED: " + error);
			return json(nullptr);
		}
		return result;
	};

	bool ok = false;

	// 1) hotkeys.list: expect > 0, and find the frontend Start Streaming hotkey (proof
	// the frontend registration ran). Capture its id + whether it already has bindings.
	json list = run("hotkeys.list", json(nullptr), ok);
	if (!ok || !list.is_object() || !list["hotkeys"].is_array()) {
		return;
	}
	const size_t total = list["hotkeys"].size();
	std::string startId;
	bool startHadBindings = false;
	size_t frontendCount = 0;
	for (const auto &h : list["hotkeys"]) {
		if (h.value("registerer", std::string()) == "frontend") {
			frontendCount++;
		}
		if (h.value("name", std::string()) == "OBSBasic.StartStreaming") {
			startId = h.value("id", std::string());
			startHadBindings = h["bindings"].is_array() && !h["bindings"].empty();
		}
	}
	HostLog("[selftest] hotkeys.list -> " + std::to_string(total) + " hotkey(s), " +
		std::to_string(frontendCount) + " frontend; Start Streaming id=" +
		(startId.empty() ? "(MISSING -- BUG)" : startId));
	if (startId.empty()) {
		return;
	}

	// 2) set Ctrl+Shift+F12 on it via a DOM-code-shaped binding, expect a non-empty
	// display string back.
	json set = run("hotkeys.set",
		       json{{"id", startId},
			    {"bindings", json::array({json{{"code", "F12"},
							   {"ctrl", true},
							   {"shift", true},
							   {"alt", false},
							   {"meta", false}}})}},
		       ok);
	std::string setDisplay;
	if (ok && set.is_object() && set["bindings"].is_array() && !set["bindings"].empty()) {
		setDisplay = set["bindings"][0].value("display", std::string());
	}
	HostLog("[selftest] hotkeys.set Ctrl+Shift+F12 -> display='" + setDisplay + "'");

	// 3) Re-list and confirm the binding reads back on that hotkey (round-trip).
	json relist = run("hotkeys.list", json(nullptr), ok);
	std::string readback;
	if (ok && relist.is_object() && relist["hotkeys"].is_array()) {
		for (const auto &h : relist["hotkeys"]) {
			if (h.value("id", std::string()) == startId && h["bindings"].is_array() &&
			    !h["bindings"].empty()) {
				readback = h["bindings"][0].value("display", std::string());
			}
		}
	}
	HostLog("[selftest] hotkeys.set round-trip -> readback='" + readback + "' (" +
		(!readback.empty() && readback == setDisplay ? "OK" : "MISMATCH") + ")");

	// 4) Restore: clear the binding we added (the portable smoke run starts with no
	// saved file, so Start Streaming was unbound -- clearing returns it to baseline).
	run("hotkeys.clear", json{{"id", startId}}, ok);
	HostLog(std::string("[selftest] hotkeys.clear restore -> ") + (ok ? "ok" : "FAIL") +
		(startHadBindings ? " (NOTE: hotkey had a prior binding; cleared)" : ""));
}

void ObsBootstrap::RunStatsSelfTest()
{
	using Bridge::json;

	json result;
	std::string error;
	if (!Bridge::Dispatch("stats.get", json(nullptr), result, error)) {
		HostLog("[selftest] stats.get FAILED: " + error);
		return;
	}

	const bool hasGeneral = result.is_object() && result["general"].is_object();
	const bool hasFps = hasGeneral && result["general"].contains("fps") && result["general"]["fps"].is_number();
	const bool hasCpu = hasGeneral && result["general"].contains("cpu") && result["general"]["cpu"].is_number();
	const bool outputsArray = result.is_object() && result["outputs"].is_array();

	size_t enabled = 0;
	for (const auto &b : g_outputBindings.Bindings().bindings) {
		if (b.enabled) {
			enabled++;
		}
	}
	const size_t outputsSize = outputsArray ? result["outputs"].size() : 0;
	const double fps = hasFps ? result["general"].value("fps", 0.0) : 0.0;
	const double cpu = hasCpu ? result["general"].value("cpu", 0.0) : 0.0;

	HostLog("[selftest] stats.get -> fps=" + std::to_string(fps) + " cpu=" + std::to_string(cpu) +
		" outputs=" + std::to_string(outputsSize) + " (enabled bindings=" + std::to_string(enabled) + ", " +
		((hasFps && hasCpu && outputsArray && outputsSize == enabled) ? "OK" : "MISMATCH") + ")");
}

void ObsBootstrap::RunMcpSelfTest()
{
	using Bridge::json;

	// Drive the MCP request path IN-PROCESS via HandleRequest (no real socket), so
	// the smoke is free of socket timing flakiness. StartForTest sets the config the
	// in-process path reads WITHOUT touching the user's mcp.json. Because this runs
	// on the UI thread (WM_TIMER), obs_call -> RunBridge sees CefCurrentlyOn(TID_UI)
	// and calls Bridge::Dispatch directly (no post-and-block deadlock).
	// Bind port 0 so the OS picks a free ephemeral port: a fixed port collides with
	// a prior run's TIME_WAIT socket on rapid restarts (the smoke does exactly that).
	// The in-process HandleRequest path the assertions use does not depend on the
	// socket; the listen is exercised only to prove the accept thread comes up cleanly.
	McpServer server;
	const bool listening = server.StartForTest(0, "selftest-token", /*allowMutations=*/true,
						   /*allowGoLive=*/false);
	HostLog(std::string("[selftest] mcp StartForTest(ephemeral) -> ") +
		(listening ? "listening" : "not-listening (in-process path still exercised)"));

	auto call = [&](const json &rpc) -> json {
		Mcp::HttpRequest req;
		req.method = "POST";
		req.path = "/mcp";
		req.authorization = "Bearer selftest-token";
		req.body = rpc.dump();
		Mcp::HttpResponse resp = server.HandleRequest(req);
		if (resp.body.empty()) {
			return json(nullptr); // e.g. a 202 notification ack
		}
		try {
			return json::parse(resp.body);
		} catch (...) {
			return json(nullptr);
		}
	};

	// 1) initialize -> serverInfo present.
	json init = call(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", json::object()}});
	const bool hasServerInfo = init.is_object() && init.contains("result") && init["result"].contains("serverInfo");
	HostLog(std::string("[selftest] mcp initialize -> serverInfo ") + (hasServerInfo ? "present" : "MISSING") +
		(hasServerInfo ? " (name='" + init["result"]["serverInfo"].value("name", std::string("?")) +
					 "' version=" + init["result"]["serverInfo"].value("version", std::string("?")) + ")"
			       : ""));

	// 2) tools/list -> contains obs_call AND the curated tools (e.g. switch_scene,
	// get_stats).
	json toolsList = call(json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
	bool hasObsCall = false;
	bool hasSwitchScene = false;
	bool hasGetStats = false;
	int toolCount = 0;
	if (toolsList.is_object() && toolsList.contains("result") && toolsList["result"]["tools"].is_array()) {
		for (const auto &t : toolsList["result"]["tools"]) {
			const std::string tn = t.value("name", std::string());
			++toolCount;
			if (tn == "obs_call") {
				hasObsCall = true;
			} else if (tn == "switch_scene") {
				hasSwitchScene = true;
			} else if (tn == "get_stats") {
				hasGetStats = true;
			}
		}
	}
	const bool curatedOk = hasObsCall && hasSwitchScene && hasGetStats;
	HostLog(std::string("[selftest] mcp tools/list -> ") + std::to_string(toolCount) + " tools; obs_call " +
		(hasObsCall ? "present" : "MISSING") + ", switch_scene " + (hasSwitchScene ? "present" : "MISSING") +
		", get_stats " + (hasGetStats ? "present" : "MISSING") + " (" + (curatedOk ? "OK" : "MISMATCH") + ")");

	// 3) tools/call obs_call scenes.list -> isError false + parsed text is an array.
	json scenesCall = call(json{{"jsonrpc", "2.0"},
				    {"id", 3},
				    {"method", "tools/call"},
				    {"params", json{{"name", "obs_call"},
						    {"arguments", json{{"method", "scenes.list"}, {"params", json::object()}}}}}});
	bool scenesOk = false;
	bool scenesArray = false;
	if (scenesCall.is_object() && scenesCall.contains("result")) {
		const json &r = scenesCall["result"];
		scenesOk = r.is_object() && r.value("isError", true) == false;
		if (scenesOk && r["content"].is_array() && !r["content"].empty()) {
			try {
				const json parsed = json::parse(r["content"][0].value("text", std::string()));
				scenesArray = parsed.is_array();
			} catch (...) {
				scenesArray = false;
			}
		}
	}
	HostLog(std::string("[selftest] mcp obs_call scenes.list -> isError=") + (scenesOk ? "false" : "true") +
		" content[0] is array=" + (scenesArray ? "true" : "false") +
		((scenesOk && scenesArray) ? " (OK)" : " (MISMATCH)"));

	// 4) Gating: allowGoLive=false, so multistream.startOutput is blocked before
	// Dispatch -> isError true with "disabled" in the text, and it did NOT execute.
	json goLiveCall =
		call(json{{"jsonrpc", "2.0"},
			  {"id", 4},
			  {"method", "tools/call"},
			  {"params", json{{"name", "obs_call"},
					  {"arguments", json{{"method", "multistream.startOutput"},
							     {"params", json{{"bindingUuid", "does-not-exist"}}}}}}}});
	bool gateBlocked = false;
	std::string gateText;
	if (goLiveCall.is_object() && goLiveCall.contains("result")) {
		const json &r = goLiveCall["result"];
		gateBlocked = r.is_object() && r.value("isError", false) == true;
		if (r["content"].is_array() && !r["content"].empty()) {
			gateText = r["content"][0].value("text", std::string());
		}
	}
	const bool gateOk = gateBlocked && gateText.find("disabled") != std::string::npos;
	HostLog(std::string("[selftest] mcp go-live gating -> isError=") + (gateBlocked ? "true" : "false") +
		" text='" + gateText + "' (" + (gateOk ? "OK, did not execute" : "MISMATCH") + ")");

	// 5) Curated tool round-trip: tools/call get_stats -> isError false + parsed text
	// is an object (the Stats snapshot). Exercises the data-driven curated dispatch.
	json statsCall = call(json{{"jsonrpc", "2.0"},
				   {"id", 5},
				   {"method", "tools/call"},
				   {"params", json{{"name", "get_stats"}, {"arguments", json::object()}}}});
	bool statsOk = false;
	bool statsObject = false;
	if (statsCall.is_object() && statsCall.contains("result")) {
		const json &r = statsCall["result"];
		statsOk = r.is_object() && r.value("isError", true) == false;
		if (statsOk && r["content"].is_array() && !r["content"].empty()) {
			try {
				const json parsed = json::parse(r["content"][0].value("text", std::string()));
				statsObject = parsed.is_object();
			} catch (...) {
				statsObject = false;
			}
		}
	}
	HostLog(std::string("[selftest] mcp tools/call get_stats -> isError=") + (statsOk ? "false" : "true") +
		" content[0] is object=" + (statsObject ? "true" : "false") +
		((statsOk && statsObject) ? " (OK)" : " (MISMATCH)"));

	// 6) Security: the mcp.* namespace configures THIS server, so it must NOT be
	// reachable via the network endpoint it authorizes -- else a client could
	// obs_call mcp.setConfig {allowGoLive:true} to escalate its own capabilities.
	// Both the escalation attempt (setConfig) and a config read (getConfig) must be
	// refused at the GateAndRun chokepoint, BEFORE dispatch, so nothing executes.
	auto deniedFromMcp = [&](const std::string &method, const json &p) -> bool {
		json r = call(json{{"jsonrpc", "2.0"},
				   {"id", 6},
				   {"method", "tools/call"},
				   {"params", json{{"name", "obs_call"},
						   {"arguments", json{{"method", method}, {"params", p}}}}}});
		if (!r.is_object() || !r.contains("result")) {
			return false;
		}
		const json &res = r["result"];
		const bool isErr = res.is_object() && res.value("isError", false) == true;
		std::string text;
		if (res["content"].is_array() && !res["content"].empty()) {
			text = res["content"][0].value("text", std::string());
		}
		return isErr && text.find("not callable") != std::string::npos;
	};
	const bool escalationBlocked = deniedFromMcp("mcp.setConfig", json{{"allowGoLive", true}});
	const bool configReadBlocked = deniedFromMcp("mcp.getConfig", json::object());
	const bool mcpNsGuarded = escalationBlocked && configReadBlocked;
	HostLog(std::string("[selftest] mcp namespace guard -> setConfig blocked=") +
		(escalationBlocked ? "true" : "false") + " getConfig blocked=" + (configReadBlocked ? "true" : "false") +
		(mcpNsGuarded ? " (OK, no self-config via MCP)" : " (MISMATCH -- privilege escalation!)"));

	if (hasServerInfo && hasObsCall && curatedOk && scenesOk && scenesArray && gateOk && statsOk && statsObject &&
	    mcpNsGuarded) {
		HostLog("[selftest] mcp -> initialize/tools.list/obs_call/curated OK; go-live + mcp.* gated OK");
	} else {
		HostLog("[selftest] mcp -> FAILED (see step lines above)");
	}

	server.Stop();
}

void ObsBootstrap::Stop()
{
	// Stop the MCP server FIRST: set its shutdown flag and join its accept thread so
	// no new request is accepted and any in-flight marshalled call bails. The CEF
	// loop has already returned by the time Stop() runs (main.cpp pumps then stops),
	// so the marshal-to-UI path can no longer be serviced -- joining here guarantees
	// nothing is left blocked on it. Done before Bridge::Shutdown so the bridge +
	// libobs are still up for any draining call.
	if (g_mcp) {
		g_mcp->Stop();
		Mcp::SetInstance(nullptr);
		g_mcp.reset();
	}

	// Drop the bridge's obs frontend event callback while libobs is still up.
	Bridge::Shutdown();

	// Unregister the frontend-owned hotkeys while libobs is still up, before the
	// engine they drive is torn down below. Saved bindings already persisted on every
	// hotkeys.set/clear, so no save is needed here.
	Hotkeys::UnregisterFrontendHotkeys();

	// Unbind channel 0 and destroy the program transition while libobs is still up,
	// before the scene-removal pass below, so the transition releases its wrapped
	// scene first (and its own free lands in the drain loops here, not obs_shutdown).
	Transitions::Shutdown();

	// Tear the audio mixer down while libobs is still up: disconnect the global
	// source signals FIRST (so no further Rebuild/audio.changed fires during
	// teardown), then ClearAll removes every volmeter callback before destroying
	// the volmeter/fader (the callback-fires-during-destroy hazard) and reset. Done
	// before the destroy-queue drains + obs_shutdown so every fader/volmeter is
	// released within the leak measurement.
	DisconnectAudioSourceSignals();
	if (g_audioMonitor) {
		g_audioMonitor->ClearAll();
		g_audioMonitor.reset();
	}

	// Unbind the global audio channels (Desktop Audio / Mic) so the wasapi sources
	// are destroyed before obs_shutdown, mirroring the channel-0 unbind in
	// TeardownScene. Done after the monitor teardown (its volmeters are detached
	// first) and before the drain loop so the source frees are captured here.
	Bridge::ClearGlobalAudio();

	// Deferred source destruction can cascade across the destruction-task
	// thread; drain in a loop until no more work is spawned before
	// obs_shutdown, mirroring the Qt frontend's ClearSceneData.
	while (obs_wait_for_destroy_queue()) {
	}

	// Tear the engine down before the stores it references clear and before
	// obs_shutdown: StopAll releases its services/outputs/encoders while libobs
	// is still up. The dtor also calls StopAll (defensive), but reset here so the
	// order against Clear() is explicit. Safe to notify during the explicit
	// StopAll because (a) Bridge::Shutdown ran above and the CEF loop has already
	// returned, so any TID_UI emit task is drained before this point, and (b) the
	// deferred DoEmit reads no engine state -- BuildStatusArray runs synchronously
	// at post time. The dtor clears onStatusChanged before its own StopAll, so the
	// reset()-nulled global is never dereferenced.
	if (g_multistream) {
		g_multistream->StopAll();
		g_multistream.reset();
	}

	// Stop + release the virtual-camera output while libobs is still up, before the
	// canvas mixes it feeds are destroyed below. Shutdown() disconnects its signals
	// first, so onChanged is never read again after this -- no nulling needed.
	g_virtualCam.Shutdown();

	// Destroy the additional-canvas mixes while libobs is still up, after the
	// engine (which bound encoders to those mixes) is gone but before the stores
	// clear and obs_shutdown. The canvases hold scene references, so freeing them
	// here keeps the leak count below at the libobs static residual. Destroying a
	// canvas releases its scene sources, whose obs_source_destroy defers the actual
	// free onto the destruction-task thread, so drain again afterward (the earlier
	// drain ran before these scenes existed) before obs_shutdown.
	if (g_canvasRuntime) {
		g_canvasRuntime->ClearAll();
		g_canvasRuntime.reset();
		while (obs_wait_for_destroy_queue()) {
		}
	}

	// Remove the loaded global scene collection while libobs is still up, mirroring
	// the legacy ClearSceneData. On the Load() boot path TeardownScene early-returns
	// (g_scene is null), so the restored main-canvas scenes + their input sources
	// would otherwise survive to obs_shutdown -- which force-frees them, growing the
	// leak count and tripping a double-destroy. Unbind channel 0, remove every
	// remaining scene and source, then drain the destruction queue so the frees land
	// here. No-op-safe when nothing is loaded (default-scene path already cleared).
	obs_set_output_source(0, nullptr);
	auto removeCb = [](void *, obs_source_t *source) -> bool {
		obs_source_remove(source);
		return true;
	};
	obs_enum_scenes(removeCb, nullptr);
	obs_enum_sources(removeCb, nullptr);
	while (obs_wait_for_destroy_queue()) {
	}

	// Release the multistream model's obs_data while libobs is still up, so the
	// leak count below reflects libobs's true static residual rather than the
	// loaded canvas/profile config (the store globals would otherwise live until
	// process exit, past the measurement).
	g_canvases.Clear();
	g_streamProfiles.Clear();
	g_outputBindings.Clear();
	g_sceneLinks.Clear();
	g_sceneCollections.Clear();
	g_undo.Clear();

	obs_shutdown();

	// Same counter the legacy frontend prints. Nonzero == fixed libobs static
	// residuals (no per-run growth), not host-introduced leaks.
	HostLog("[obs] leaks: " + std::to_string(bnum_allocs()));
	HostLog("[obs] shutdown complete");
}
