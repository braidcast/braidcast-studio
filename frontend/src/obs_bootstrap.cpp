#include "obs_bootstrap.hpp"
#include "event_names.hpp"

#include <obs.h>
#include <obs-frontend-internal.hpp>
#include <util/base.h>
#include <util/platform.h>

#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "util/async_task.hpp"
#include "audio/AudioMonitor.hpp"
#include "bridge.hpp"
#include "settings/DiagnosticsSettings.hpp"
#include "frontend_callbacks.hpp"
#include "log.hpp"
#include "chat/channel_stats_poller.hpp"
#include "events/event_hub.hpp"
#include "events/event_store.hpp"
#include "events/transport_health.hpp"
#include "multistream/CanvasRuntime.hpp"
#include "multistream/CanvasService.hpp"
#include "multistream/CanvasStore.hpp"
#include "multistream/GlobalAudioChannels.hpp"
#include "multistream/Hotkeys.hpp"
#include "mcp/McpServer.hpp"
#include "multistream/MultistreamEngine.hpp"
#include "multistream/OutputBindingStore.hpp"
#include "multistream/SceneLinkStore.hpp"
#include "multistream/StreamMetaStore.hpp"
#include "multistream/StreamProfileStore.hpp"
#include "multistream/VirtualCamManager.hpp"
#include "oauth/registry.hpp"
#include "overlay/overlay_server.hpp"
#include "overlay/overlay_store.hpp"
#include "settings/AdvancedSettings.hpp"
#include "settings/GeneralSettings.hpp"
#include "util/paths.hpp"
#include "windowing/native_theme.hpp"
#include "windowing/preview_window.hpp"
#include "windowing/projector_window.hpp"
#include "scene/scene_collections.hpp"
#include "util/session_log.hpp"
#include "scene/scene_persistence.hpp"
#include "scene/transitions.hpp"
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

// Read `key`'s value from a KEY=VALUE .env file (CRLF- and whitespace-tolerant).
// Yields the raw value string; nullopt when the file is missing or has no such
// key, so the caller falls through to the next source.
std::optional<std::string> DebugFromEnvFile(const char *path, const char *key)
{
	std::ifstream f(path);
	if (!f) {
		return std::nullopt;
	}
	const auto trim = [](std::string s) {
		const size_t b = s.find_first_not_of(" \t\r");
		if (b == std::string::npos) {
			return std::string();
		}
		return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
	};
	std::string line;
	while (std::getline(f, line)) {
		const size_t eq = line.find('=');
		if (eq == std::string::npos) {
			continue;
		}
		if (trim(line.substr(0, eq)) != key) {
			continue;
		}
		return trim(line.substr(eq + 1));
	}
	return std::nullopt;
}

// Resolve one debug key's raw value: the process env var wins, else the same key
// in the gitignored repo-root .env (dev builds only; the path is baked at
// configure time and absent in CI/shipped builds), else nullopt. Edit .env and
// relaunch to flip debug logging without a rebuild or env var.
std::optional<std::string> ResolveDebugRaw(const char *key)
{
	if (const char *env = getenv(key)) {
		return std::string(env);
	}
#ifdef BRAIDCAST_ENV_FILE
	if (const std::optional<std::string> v = DebugFromEnvFile(BRAIDCAST_ENV_FILE, key)) {
		return v;
	}
#endif
	return std::nullopt;
}

// The pure-boolean master grammar: 1/true/on/yes (case-insensitive) are on;
// everything else -- 0/false/off/no/empty -- is off.
bool ParseBool(const std::string &raw)
{
	std::string v;
	for (const char c : raw) {
		if (!std::isspace(static_cast<unsigned char>(c))) {
			v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
	}
	return v == "1" || v == "true" || v == "on" || v == "yes";
}

// Whether the gpudiag sampler was requested via BRAIDCAST_DEBUG_COMPONENTS;
// resolved once in Start() and read back by ObsBootstrap::GpuDiagRequested().
bool g_gpuDiagRequested = false;

// Resolve the two-var debug scheme into the applied config. The master
// BRAIDCAST_DEBUG (env -> .env -> persisted DiagnosticsSettings.debugLogging ->
// off) is a pure boolean; while on, BRAIDCAST_DEBUG_COMPONENTS (env -> .env ->
// empty) selects the categories + subsystems, defaulting to kDefaultCats (every
// category except the render firehose) when empty/unset. Log::ParseComponents
// owns the component vocabulary.
Log::DebugComponents ResolveDebugConfig()
{
	bool master;
	if (const std::optional<std::string> raw = ResolveDebugRaw("BRAIDCAST_DEBUG")) {
		master = ParseBool(*raw);
	} else {
		DiagnosticsSettings ds;
		ds.Load();
		master = ds.debugLogging;
	}
	if (!master) {
		return Log::DebugComponents{};
	}

	const std::optional<std::string> comps = ResolveDebugRaw("BRAIDCAST_DEBUG_COMPONENTS");
	const bool compsEmpty = !comps || comps->find_first_not_of(" \t\r\n") == std::string::npos;
	if (compsEmpty) {
		return Log::DebugComponents{Log::kDefaultCats, false};
	}
	return Log::ParseComponents(*comps);
}

// Route libobs/plugin blog() output to stderr so plugin lifecycle logging (e.g.
// obs-browser's "frontend owns CEF" line) is captured alongside the host's own.
void ObsLogHandler(int level, const char *format, va_list args, void *)
{
	char buf[4096];
	vsnprintf(buf, sizeof(buf), format, args);
	// Write blog() output straight to the debugger + stderr here. Do NOT route it
	// through HostLog: HostLog now emits via blog(), so calling it from the blog
	// handler recurses (blog -> handler -> HostLog -> blog ...) until the stack
	// overflows. SessionLog's chained handler separately persists every blog() line
	// to the session file, so HostLog's own lifecycle lines (HostLog -> blog) land
	// in that file too -- without this handler ever calling back into HostLog.
	OutputDebugStringA("[obs:log] ");
	OutputDebugStringA(buf);
	OutputDebugStringA("\n");
	fprintf(stderr, "[obs:log] %s\n", buf);
	fflush(stderr);
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
			// WARNING, not HostLog's INFO: a curated module that silently fails to
			// open ships the app with a missing encoder/source/output and nothing
			// to point at (finding G6).
			blog(LOG_WARNING, "[obs] module '%s' open-failed (code=%d, likely non-module)", name.c_str(),
			     r);
			continue;
		}
		if (obs_init_module(mod)) {
			loaded.push_back(name);
			HostLog("[obs] module " + name + " loaded");
		} else {
			initFailed.push_back(name);
			blog(LOG_WARNING, "[obs] module '%s' init-failed (obs_init_module returned false)",
			     name.c_str());
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);

	// warnIfAny promotes a non-empty failure category from HostLog's always-on
	// INFO to WARNING, so a scan for warnings alone surfaces the disposition
	// summary even if the per-module lines above scroll past.
	auto joinList = [](const char *label, const std::vector<std::string> &v, bool warnIfAny) {
		std::string line = std::string("[obs] ") + label + " (" + std::to_string(v.size()) + "):";
		for (const auto &n : v) {
			line += " " + n;
		}
		if (warnIfAny && !v.empty()) {
			blog(LOG_WARNING, "%s", line.c_str());
		} else {
			HostLog(line);
		}
	};
	joinList("loaded", loaded, false);
	joinList("init-failed (environmental)", initFailed, true);
	joinList("open-failed/non-module", openFailed, true);
	joinList("skipped denylist", skippedDeny, false);
	joinList("skipped helper-dll", skippedHelper, false);
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
	auto svcCreate = [](const char *id) -> void * {
		return obs_service_create(id, "probe-svc", nullptr, nullptr);
	};
	auto outCreate = [](const char *id) -> void * {
		return obs_output_create(id, "probe-out", nullptr, nullptr);
	};
	auto srcCreate = [](const char *id) -> void * {
		return obs_source_create(id, "probe-web", nullptr, nullptr);
	};

	const Probe probes[] = {
		{"encoder", "obs_x264", encCreate,
		 [](void *p) {
			 obs_encoder_release((obs_encoder_t *)p);
		 }},
		{"service", "rtmp_custom", svcCreate,
		 [](void *p) {
			 obs_service_release((obs_service_t *)p);
		 }},
		{"output", "rtmp_output", outCreate,
		 [](void *p) {
			 obs_output_release((obs_output_t *)p);
		 }},
		{"source", "browser_source", srcCreate,
		 [](void *p) {
			 obs_source_release((obs_source_t *)p);
		 }},
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

// The canvas update/reconciliation domain service. Constructed after the runtime +
// engine (it holds references to both plus the canvas model) and reset in Stop
// before them. Its GlobalVideoApplier is Bridge::ApplyDefaultCanvasVideo, so the
// domain layer owns the ordering while the bridge keeps the preview/transition
// side-effects of a global video reset.
std::unique_ptr<CanvasService> g_canvasService;

// The audio mixer's per-source fader/volmeter manager. Built in Start after the
// default scene + modules, torn down in Stop BEFORE obs_shutdown (its volmeter
// callbacks are removed first by ClearAll). The global source activate/deactivate
// signals below rebuild its set + push audio.changed so the UI re-lists.
std::unique_ptr<AudioMonitor> g_audioMonitor;

// The global desktop/mic audio channels (wasapi sources on output channels 1..6).
// Seeded/restored in Start before the audio monitor is built, unbound in Stop after
// the monitor teardown. Stateless (operates on the live OBS channels + the on-disk
// map), so a plain member -- no teardown state of its own.
GlobalAudioChannels g_globalAudio;

// The remembered stream-metadata store. Trivial ctor; Load()ed early in Start()
// (after obs_startup + portable config) like the other stores. A plain member
// with no teardown state of its own.
StreamMetaStore g_streamMeta;

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
	g_streamMeta.Load();
	g_outputBindings.Load();
	g_sceneLinks.Load();

	const CanvasDefinition &def = g_canvases.Default();
	HostLog("[obs] multistream: " + std::to_string(g_canvases.Definitions().size()) + " canvas(es); default='" +
		def.name + "' uuid=" + def.uuid + " " + std::to_string(def.width) + "x" + std::to_string(def.height) +
		"@" + std::to_string(def.fpsNum) + "/" + std::to_string(def.fpsDen) +
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

::GlobalAudioChannels &ObsBootstrap::GlobalAudioChannels()
{
	return g_globalAudio;
}

::StreamMetaStore &ObsBootstrap::StreamMeta()
{
	return g_streamMeta;
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

::CanvasService &ObsBootstrap::CanvasService()
{
	// Valid between Start() (constructs g_canvasService after the runtime + engine)
	// and Stop() (resets it). Its only caller is the canvas.update bridge method,
	// driven by JS after the CEF page loads, so the pointer is non-null on every
	// reachable path.
	return *g_canvasService;
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
		const std::string sceneName = runtime.SceneNameForUuid(canvasUuid, canvasSceneUuid);
		if (!sceneName.empty() && runtime.SetCurrentScene(canvasUuid, sceneName)) {
			Bridge::EmitEvent(EventNames::kScenesChanged, nlohmann::json{{"canvas", canvasUuid}});
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

size_t ObsBootstrap::PruneOutputBindingsForProfile(const std::string &profileUuid)
{
	// Only the ACTIVE collection's bindings are in memory; inactive collections keep
	// their stale rows on disk and fall back to the ProfileLabelFor "(deleted)" label
	// until they load. `bindings` is the raw vector (auto& avoids naming the struct,
	// whose name the OutputBindings() accessor shadows here).
	auto &bindings = OutputBindings().Bindings().bindings;
	const size_t before = bindings.size();
	bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
				      [&profileUuid](const OutputBinding &b) { return b.profileUuid == profileUuid; }),
		       bindings.end());
	const size_t removed = before - bindings.size();
	if (removed > 0) {
		OutputBindings().Save();
	}
	return removed;
}

MultistreamEngine &ObsBootstrap::Multistream()
{
	// Valid only between Start() (constructs g_multistream after the stores load)
	// and Stop() (resets it). Every caller is a bridge method driven by JS, which
	// can only run once the CEF page has loaded -- well after construction -- so
	// the pointer is non-null on every reachable path.
	return *g_multistream;
}

bool ObsBootstrap::MultistreamAlive()
{
	return g_multistream != nullptr;
}

::AudioMonitor &ObsBootstrap::AudioMonitor()
{
	// Valid between Start() (constructs g_audioMonitor after the default scene +
	// modules) and Stop() (resets it). Bridge methods only reach here while the CEF
	// page is loaded, but the throttled audio.levels emit can be drained by
	// CefShutdown after Stop() reset the pointer, so that path guards with
	// AudioMonitorAlive() first.
	return *g_audioMonitor;
}

bool ObsBootstrap::AudioMonitorAlive()
{
	return g_audioMonitor != nullptr;
}

bool ObsBootstrap::GpuDiagRequested()
{
	return g_gpuDiagRequested;
}

// Re-pin the process priority to the current live state. In "auto" mode this is HIGH
// while any output is live and ABOVE_NORMAL when idle; a manual override just re-applies
// its fixed class (idempotent). The engine fires onLiveStateChanged from the libobs
// output-signal thread, but g_advanced is only ever written on the CEF UI thread (the
// setAdvanced bridge setter), so reading it off-thread would race. Marshal to the UI
// thread and re-read AnyLive() there so the resolution sees a consistent snapshot.
// PostToUi's alive-guard drops the task after teardown; g_multistream is re-checked in
// case Stop() already ran.
static void SyncProcessPriorityToLiveState()
{
	AsyncTask::PostToUi([] {
		if (g_multistream) {
			ApplyEffectivePriority(g_advanced.processPriority, g_multistream->AnyLive());
		}
	});
}

bool ObsBootstrap::Start()
{
	// obs-browser checks this in its guarded path to skip CefInitialize (the
	// frontend already owns the single CEF context). Set before module load.
	SetEnvironmentVariableW(L"OBS_FRONTEND_OWNS_CEF", L"1");

	base_set_log_handler(ObsLogHandler, nullptr);

	// Chain a per-session file writer onto the stderr/HostLog handler installed
	// above so every blog() line is also persisted under .../braidcast/logs.
	SessionLog::Init();

	// Resolve + apply the two-var debug scheme before anything else logs: master
	// BRAIDCAST_DEBUG gates, BRAIDCAST_DEBUG_COMPONENTS selects. Off by default ->
	// DBG() costs nothing. The resolved gpudiag flag is stashed for GpuDiag::Start.
	const Log::DebugComponents dbg = ResolveDebugConfig();
	Log::SetDebugMask(dbg.logMask);
	g_gpuDiagRequested = dbg.gpuDiag;
	DBG(LogCat::Lifecycle, "bootstrap start (debug categories=0x%x gpudiag=%d)", (unsigned)Log::DebugMask(),
	    dbg.gpuDiag ? 1 : 0);

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

	// Re-apply the seeded DEBUG gate now that obs exists: the boot seed above ran
	// through Log::SetDebug before obs_startup, when obs_set_render_debug no-ops.
	obs_set_render_debug(Log::DebugEnabled());

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
	// Nothing can be live at startup, and g_multistream is not constructed yet, so
	// resolve "auto" against an idle state (false) rather than calling AnyLive().
	ApplyEffectivePriority(g_advanced.processPriority, false);
	DisableAudioDucking(g_advanced.disableAudioDucking);
	HostLog("[obs] advanced settings loaded; process priority=" + g_advanced.processPriority +
		"; audio ducking disabled=" + std::string(g_advanced.disableAudioDucking ? "true" : "false"));

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
	if (g_sceneCollections.IndexWasCorrupt()) {
		// A doubly-corrupt index would otherwise strand intact scenes/*.json behind a
		// blank app; rebuild the index from the scene files still on disk.
		g_sceneCollections.RebuildFromScenes();
	}
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
		(activeCollection ? activeCollection->name : "(none)") +
		"' file=" + g_sceneCollections.ActiveScenePath());

	// Load the multistream model (canvas defs / stream profiles / output bindings)
	// and bring up the additional-canvas obs_canvas_t mixes BEFORE restoring scenes,
	// so each saved scene's canvas_uuid rebinds to its real canvas instead of
	// falling back to the main canvas (libobs obs_load_source_type). Bindings load
	// from the active collection's path, so this must run after the registry +
	// bindings migration above.
	LoadMultistreamModel();
	g_canvasRuntime = std::make_unique<::CanvasRuntime>(g_canvases);
	// Reuse OutputBindings::AnyEnabledForCanvas as the "has enabled destination"
	// half of the active predicate. Set before Sync so only canvases with an
	// enabled destination get a mix at bootstrap; inert ones stay mix-less (zero
	// composite) until a destination is enabled or a preview opens.
	g_canvasRuntime->SetEnabledPredicate(
		[](const std::string &uuid) { return g_outputBindings.Bindings().AnyEnabledForCanvas(uuid); });
	g_canvasRuntime->SyncFromDefinitions();

	// Restore the active collection's scenes; first run with no scene file falls
	// back to the placeholder default scene. On the Load path g_scene stays null,
	// which the null-safe TeardownScene handles. Load also seeds + re-binds every
	// additional canvas's scene internally, so no follow-up is needed here.
	if (!SceneCollection::Load()) {
		CreateDefaultScene();
	}

	// Route channel 0 through the program transition: it wraps the scene just bound
	// above and rebinds itself to channel 0, so scene switches animate (Fade by
	// default). Sized to the base canvas, hence after obs_reset_video above.
	Transitions::Init();

	// Populate the OAuth provider registry (Phase 8a). Empty in Task 3 (framework
	// only); Task 4 registers the Twitch provider. Done after the model loads so a
	// provider can read configured credentials.
	OAuth::BootProviders();

	// Reclaim OAuth accounts stranded by a deleted stream profile before the hubs read
	// them: an account is only ever created by a profile's connect flow, so one no
	// profile references is unowned and must not resume its chat/events transports.
	// Runs after the profile + account stores loaded and the registry is populated
	// (needed by the shared teardown), before StartConnectedAccounts/Chat::Start below.
	Bridge::ReconcileOrphanedAccounts();

	// Phase 9.2a: resume the live-events feed for accounts connected in a prior
	// session (the events feed is account-lifecycle, always-on). Run once here now
	// that the registry + account store are ready; inert until a provider's makeEvents()
	// transport is non-null (9.2b+).
	Events::Hub().StartConnectedAccounts();

	// Launch-time credential self-heal: some stream profiles were linked before the
	// connect flow started seeding "server=auto" alongside the key, leaving them
	// unable to go live with no visible error. Runs once here, now that the profile +
	// account stores and provider registry are all ready.
	Bridge::SelfHealStreamCredentials();

	// Channel identity: the audience-total poller is always-on (account-lifecycle,
	// not go-live-gated), so follower/subscriber totals refresh before/after
	// streaming. Stopped in Bridge::Shutdown alongside the other always-on workers.
	Chat::Channels().Start();

	// Phase 9.3: bring up the overlay-widget loopback server (127.0.0.1). Started
	// after the model + providers so a widget URL is immediately servable; stopped in
	// Bridge::Shutdown before CEF teardown (alongside the chat/events transports).
	// The bind result feeds the transport-health surface (Connected when listening,
	// Failed when no port in range binds).
	const bool overlayUp = Overlay::Server().Start();
	Transports::Health().Report(Transports::kOverlayTransportId,
				    overlayUp ? Transports::TransportHealth::State::Connected
					      : Transports::TransportHealth::State::Failed,
				    overlayUp ? "" : Overlay::Server().LastError());

	// Boot reconcile: the global video pipeline was initialized to a fixed default
	// above (before modules could load), but the persisted Default canvas def is the
	// source of truth for its resolution/FPS -- the Settings UI edits the Default
	// canvas, which drives global video. Re-apply from the def so a saved resolution
	// survives restarts. Inert on first run (seeded def == the fixed init).
	{
		const CanvasDefinition &def = g_canvases.Default();
		obs_video_info cur = {};
		if (obs_get_video_info(&cur) && (cur.base_width != def.width || cur.base_height != def.height ||
						 cur.fps_num != def.fpsNum || cur.fps_den != def.fpsDen)) {
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

	// Build the fan-out engine over the now-loaded stores. The Default canvas
	// encodes from the global mix; additional canvases encode from their
	// CanvasRuntime obs_canvas_t mix. State changes route to the bridge, which
	// posts the multistream.changed push on its own (thread-safe) UI marshaling.
	g_multistream = std::make_unique<MultistreamEngine>(
		g_canvases, g_streamProfiles, g_outputBindings, [](const std::string &uuid) -> video_t * {
			return uuid == g_canvases.Default().uuid ? obs_get_video() : g_canvasRuntime->VideoFor(uuid);
		});
	g_multistream->onStatusChanged = [] {
		Bridge::EmitMultistreamChanged();
	};

	// Wire CanvasRuntime <-> engine now that both exist (deferred past g_canvasRuntime's
	// own construction to avoid a cycle; injected callbacks mirror SetEnabledPredicate):
	//  - the runtime gates its mix-drop on the engine's real-handle liveness so a video
	//    mix is never freed while an output's encoder still pulls from it (async stop);
	//  - the runtime drops the engine's cached encoder pair whenever a mix is (re)built
	//    or cleared, so the once-bound encoder video_t never dangles across a rebuild;
	//  - the engine re-runs the runtime's reconcile once an async output stop completes,
	//    marshaled to the UI thread (where all CanvasRuntime ops run) so the mix is never
	//    freed off the libobs stop-signal thread.
	g_canvasRuntime->SetOutputActivePredicate(
		[](const std::string &uuid) { return g_multistream && g_multistream->CanvasHasActiveOutput(uuid); });
	g_canvasRuntime->SetEncoderInvalidator([](const std::string &uuid) {
		if (g_multistream) {
			g_multistream->InvalidateCanvasEncoders(uuid);
		}
	});
	g_multistream->onOutputStopped = [](const std::string &canvasUuid) {
		AsyncTask::PostToUi([canvasUuid] {
			if (g_canvasRuntime) {
				g_canvasRuntime->Reconcile(canvasUuid);
			}
		});
	};

	// Re-pin the process priority at every live transition (the seam UpdateSleepInhibit
	// fires on). "auto" tracks live state -> HIGH live, ABOVE_NORMAL idle; a manual
	// override re-applies its fixed class. SyncProcessPriorityToLiveState marshals the
	// g_advanced read onto the UI thread since this fires off the libobs signal thread.
	g_multistream->onLiveStateChanged = [] {
		SyncProcessPriorityToLiveState();
	};

	// Build the canvas update/reconciliation service over the shared model, runtime,
	// and engine (it holds references to all three). The Default->global-video
	// coupling is injected as Bridge::ApplyDefaultCanvasVideo so the service owns the
	// ordering while the bridge keeps the pipeline-reset side-effects.
	g_canvasService = std::make_unique<::CanvasService>(g_canvases, *g_canvasRuntime, *g_multistream,
							    Bridge::ApplyDefaultCanvasVideo);

	// Restore the virtual camera's target canvas and route its start/stop signal
	// state changes to the virtualCam.changed push. Done after the CanvasRuntime is
	// up (Start() resolves the target canvas's mix through it). Like the engine's
	// hook, EmitVirtualCamChanged marshals to TID_UI so the off-thread signal is
	// safe.
	g_virtualCam.Load();
	g_virtualCam.onChanged = [] {
		Bridge::EmitVirtualCamChanged();
	};

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
	g_globalAudio.SeedOrRestore();

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
	HostLog("[selftest] properties.get color_source -> " + std::to_string(props.size()) + " props:" + names);
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
			HostLog("[selftest] sources.create " + std::string(type) + " -> id=" + std::to_string(id) +
				" source='" + src + "'");

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
					std::to_string(c.value("outputHeight", 0u)) +
					" scale=" + c.value("scaleType", std::string("?")) +
					" fps=" + std::to_string(c.value("fpsNum", 0u)) + "/" +
					std::to_string(c.value("fpsDen", 0u)) + " (" + (match ? "OK" : "MISMATCH") +
					")");
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

	json updated =
		run("canvas.update",
		    json{{"uuid", uuid}, {"name", "selftest-renamed"}, {"baseWidth", 1920}, {"baseHeight", 1080}}, ok);
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
		HostLog("[selftest] properties.get encoder(video) -> " + std::to_string(encProps["props"].size()) +
			" descriptors");
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
		HostLog(std::string("[selftest] canvas.remove restored file: ") +
			(gone ? "OK (temp gone)" : "STILL PRESENT") + "; store now " +
			std::to_string(reloaded.Definitions().size()));
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
			names += " '" + p.value("label", std::string("?")) + "'(" +
				 p.value("platform", std::string("?")) + "/" + p.value("service", std::string("?")) +
				 (p.value("isPrimary", false) ? ",primary" : "") + ")";
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
	json created =
		run("streamProfile.create",
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
		const bool dupOk = Bridge::Dispatch(
			"streamProfile.create",
			json{{"label", "selftest-dup"},
			     {"service", "rtmp_custom"},
			     {"settings", json{{"server", "rtmp://other.example/app"}, {"key", "selftest-key-1"}}}},
			dupResult, dupError);
		HostLog(std::string("[selftest] duplicate-key create -> ") +
			(dupOk ? "ACCEPTED (BUG: should reject)" : "REJECTED (\"" + dupError + "\")"));
	}

	json updated = run(
		"streamProfile.update",
		json{{"uuid", uuid}, {"label", "selftest-renamed"}, {"settings", json{{"key", "selftest-key-2"}}}}, ok);
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
				b.value("canvasName", std::string("?")) + (b.value("enabled", false) ? ",on" : ",off") +
				"]";
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
	const std::string firstState = statuses.empty() ? "(none)"
							: MultistreamEngine::StateName(statuses.front().state);
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
	HostLog("[selftest] engine cleanup: profiles " + std::to_string(g_streamProfiles.Profiles().size()) + " (was " +
		std::to_string(profilesBefore) + "), bindings " +
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

	// In the lazy model EnsureCanvas creates a mix-less object; the enabled binding
	// above makes the canvas active, so reconcile builds its mix (mirrors what
	// MethodOutputBindingCreate/SetEnabled do after Save). VideoFor is null until then.
	g_canvasRuntime->ReconcileAll();

	obs_canvas_t *canvas = g_canvasRuntime->Find(canvasUuid);
	video_t *video = g_canvasRuntime->VideoFor(canvasUuid);
	const char *liveUuid = canvas ? obs_canvas_get_uuid(canvas) : nullptr;
	const bool uuidMatches = liveUuid && canvasUuid == liveUuid;
	HostLog(std::string("[selftest] canvas-runtime EnsureCanvas -> Find=") + (canvas ? "ok" : "null (BUG)") +
		"; VideoFor=" + (video ? "ok" : "null (BUG)") + "; uuid " +
		(uuidMatches ? "preserved" : "MISMATCH (BUG)") + " (" + canvasUuid + " vs " +
		(liveUuid ? liveUuid : "(null)") + ")");

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
	HostLog(std::string("[selftest] canvas-runtime cleanup: temp canvas ") +
		(gone ? "removed" : "STILL PRESENT (BUG)") + "; canvases now " +
		std::to_string(g_canvases.Definitions().size()));
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
		(inCanvasList ? "true" : "false (BUG)") + " (" + std::to_string(canvasScenes.size()) + ":" +
		canvasNames + " ); global has scene=" + (inGlobalList ? "true (BUG: not isolated)" : "false") +
		" (isolation " + ((inCanvasList && !inGlobalList) ? "OK" : "BUG") + ")");

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
		(inCanvasScene ? "true" : "false (BUG)") +
		"; in global scene=" + (inGlobalScene ? "true (BUG: leaked to output 0)" : "false") + " (placement " +
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
	HostLog(std::string("[selftest] canvas-scene cleanup: temp canvas ") +
		(gone ? "removed" : "STILL PRESENT (BUG)") + "; canvases now " +
		std::to_string(g_canvases.Definitions().size()));
}

void ObsBootstrap::RunSceneDuplicateSelfTest()
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

	// Bring up two temporary ADDITIONAL canvases (source + destination), exactly
	// like the canvas-scene selftest. Operate ONLY on the in-memory stores (never
	// Save explicitly) so the user's files stay untouched. The point is to prove
	// scenes.duplicateToCanvas performs a real deep copy across canvases, with
	// undo/redo that preserves the copied source's uuid on restore.
	auto makeTempCanvas = [](const char *name) -> std::string {
		CanvasDefinition def;
		def.name = name;
		def.isDefault = false;
		def.width = 1280;
		def.height = 720;
		def.fpsNum = 30;
		def.fpsDen = 1;
		const CanvasDefinition &added = g_canvases.Add(std::move(def));
		const std::string uuid = added.uuid;
		g_canvasRuntime->EnsureCanvas(added);
		return uuid;
	};
	const std::string srcCanvasUuid = makeTempCanvas("selftest-duplicate-src-canvas");
	const std::string destCanvasUuid = makeTempCanvas("selftest-duplicate-dest-canvas");

	bool ok = false;

	// 1) Create + select a scene on the source canvas, then add one color source.
	const char *kSceneName = "selftest-duplicate-scene";
	run("scenes.create", json{{"canvas", srcCanvasUuid}, {"name", kSceneName}}, ok);
	HostLog(std::string("[selftest] scene-duplicate scenes.create -> ") + (ok ? "ok" : "FAIL (BUG)"));

	run("scenes.setCurrent", json{{"canvas", srcCanvasUuid}, {"name", kSceneName}}, ok);
	HostLog(std::string("[selftest] scene-duplicate scenes.setCurrent -> ") + (ok ? "ok" : "FAIL (BUG)"));

	// Explicit unique name: sources.create's default (the type's display name,
	// "Color") collides globally against any source already named that in the
	// user's loaded scene collection, which would fail this step for reasons
	// having nothing to do with what's under test here.
	json srcCreated = run(
		"sources.create",
		json{{"canvas", srcCanvasUuid}, {"type", "color_source"}, {"name", "selftest-duplicate-color"}}, ok);
	const int64_t srcItemId = ok ? srcCreated.value("id", int64_t(0)) : 0;
	const std::string srcSrcName = ok ? srcCreated.value("source", std::string()) : std::string();
	HostLog(std::string("[selftest] scene-duplicate sources.create -> ") +
		(ok ? "id=" + std::to_string(srcItemId) + " source='" + srcSrcName + "'" : "FAIL (BUG)"));

	std::string origSrcUuid;
	if (!srcSrcName.empty()) {
		OBSSourceAutoRelease s = obs_get_source_by_name(srcSrcName.c_str());
		if (s) {
			const char *u = obs_source_get_uuid(s);
			origSrcUuid = u ? u : std::string();
		}
	}
	HostLog(std::string("[selftest] scene-duplicate original source uuid -> ") +
		(origSrcUuid.empty() ? "MISSING (BUG)" : origSrcUuid));

	// 2) Duplicate the scene from the source canvas onto the destination canvas.
	json dup = run("scenes.duplicateToCanvas",
		       json{{"name", kSceneName}, {"canvas", srcCanvasUuid}, {"destCanvas", destCanvasUuid}}, ok);
	const std::string newSceneName = ok ? dup.value("name", std::string()) : std::string();
	HostLog(std::string("[selftest] scene-duplicate scenes.duplicateToCanvas -> ") +
		(ok ? "ok name='" + newSceneName + "'" : "FAIL (BUG)"));

	// Helper: does the destination canvas's scene list contain `sceneName`?
	auto sceneExistsOnDest = [&](const std::string &sceneName) -> bool {
		bool listOk = false;
		json scenes = run("scenes.list", json{{"canvas", destCanvasUuid}}, listOk);
		if (!listOk || !scenes.is_array()) {
			return false;
		}
		for (const auto &s : scenes) {
			if (s.value("name", std::string()) == sceneName) {
				return true;
			}
		}
		return false;
	};

	// Helper: make `sceneName` current on the destination canvas, assert it has
	// exactly one item, and return that item's source uuid (empty on any failure).
	auto singleItemSourceUuid = [&](const std::string &sceneName, int64_t &outItemId,
					std::string &outSrcName) -> std::string {
		bool setOk = false;
		run("scenes.setCurrent", json{{"canvas", destCanvasUuid}, {"name", sceneName}}, setOk);
		if (!setOk) {
			return {};
		}
		bool listOk = false;
		json items = run("sceneItems.list", json{{"canvas", destCanvasUuid}}, listOk);
		if (!listOk || !items.is_array() || items.size() != 1) {
			return {};
		}
		outItemId = items[0].value("id", int64_t(0));
		outSrcName = items[0].value("source", std::string());
		if (outSrcName.empty()) {
			return {};
		}
		OBSSourceAutoRelease s = obs_get_source_by_name(outSrcName.c_str());
		if (!s) {
			return {};
		}
		const char *u = obs_source_get_uuid(s);
		return u ? u : std::string();
	};

	// 3) Assert the new scene exists on the destination canvas with exactly one
	// item whose source uuid differs from the original -- a real copy.
	const bool foundOnDest = sceneExistsOnDest(newSceneName);
	HostLog(std::string("[selftest] scene-duplicate destCanvas scenes.list -> scene present=") +
		(foundOnDest ? "true" : "false (BUG)"));

	int64_t dupItemId = 0;
	std::string dupSrcName;
	const std::string dupSrcUuid = foundOnDest ? singleItemSourceUuid(newSceneName, dupItemId, dupSrcName)
						   : std::string();
	const bool isRealCopy = !dupSrcUuid.empty() && dupSrcUuid != origSrcUuid;
	HostLog(std::string("[selftest] scene-duplicate item copy -> uuid=") +
		(dupSrcUuid.empty() ? "MISSING (BUG)" : dupSrcUuid) +
		"; independent-of-original=" + (isRealCopy ? "true" : "false (BUG)"));

	// 4) Undo: the duplicated scene must disappear from the destination canvas.
	// The removed scene + its child source only fully leave the uuid/name
	// registry once the destruction-task thread processes their deferred
	// obs_source_destroy (obs_source_remove only marks them removed); drain it
	// synchronously so the redo below restores into a clean registry instead of
	// racing a still-live duplicate of the same uuid, mirroring the drain-loop
	// idiom Stop() already uses around canvas/scene teardown.
	ObsBootstrap::Undo().Undo();
	while (obs_wait_for_destroy_queue()) {
	}
	const bool goneAfterUndo = !sceneExistsOnDest(newSceneName);
	HostLog(std::string("[selftest] scene-duplicate undo -> scene removed=") +
		(goneAfterUndo ? "true" : "false (BUG)"));

	// 5) Redo: the scene must come back with the SAME item source uuid captured
	// above -- proof of uuid-preserving restore-from-snapshot, not a fresh
	// re-duplicate (which would mint a new uuid).
	ObsBootstrap::Undo().Redo();
	const bool backAfterRedo = sceneExistsOnDest(newSceneName);
	HostLog(std::string("[selftest] scene-duplicate redo -> scene restored=") +
		(backAfterRedo ? "true" : "false (BUG)"));

	int64_t redoItemId = 0;
	std::string redoSrcName;
	const std::string redoSrcUuid = backAfterRedo ? singleItemSourceUuid(newSceneName, redoItemId, redoSrcName)
						      : std::string();
	const bool uuidPreserved = !redoSrcUuid.empty() && redoSrcUuid == dupSrcUuid;
	HostLog(std::string("[selftest] scene-duplicate redo uuid-preserved -> ") +
		(uuidPreserved ? "true" : "false (BUG)") + " (uuid=" + redoSrcUuid + ")");

	// Clean up: remove the source items + underlying sources we created, then
	// destroy both temp canvases, returning the in-memory model to baseline.
	if (redoItemId) {
		run("sceneItems.remove", json{{"canvas", destCanvasUuid}, {"id", redoItemId}}, ok);
		obs_source_t *s = obs_get_source_by_name(redoSrcName.c_str());
		if (s) {
			obs_source_remove(s);
			obs_source_release(s);
		}
	}
	if (srcItemId) {
		run("sceneItems.remove", json{{"canvas", srcCanvasUuid}, {"id", srcItemId}}, ok);
		obs_source_t *s = obs_get_source_by_name(srcSrcName.c_str());
		if (s) {
			obs_source_remove(s);
			obs_source_release(s);
		}
	}
	g_multistream->InvalidateCanvasEncoders(destCanvasUuid);
	g_multistream->InvalidateCanvasEncoders(srcCanvasUuid);
	g_canvasRuntime->RemoveCanvas(destCanvasUuid);
	g_canvasRuntime->RemoveCanvas(srcCanvasUuid);
	g_canvases.Remove(destCanvasUuid);
	g_canvases.Remove(srcCanvasUuid);
	const bool gone = g_canvasRuntime->Find(destCanvasUuid) == nullptr &&
			  g_canvases.Find(destCanvasUuid) == nullptr &&
			  g_canvasRuntime->Find(srcCanvasUuid) == nullptr && g_canvases.Find(srcCanvasUuid) == nullptr;
	HostLog(std::string("[selftest] scene-duplicate cleanup: temp canvases ") +
		(gone ? "removed" : "STILL PRESENT (BUG)") + "; canvases now " +
		std::to_string(g_canvases.Definitions().size()));
}

void ObsBootstrap::RunTransformPivotSelfTest()
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

	// Bring up a temporary ADDITIONAL canvas, exactly like the canvas-scene
	// selftest. In-memory only (never Save). The point is to prove, against a
	// real libobs scene item, that setTransform/transformAction rotations pivot
	// around the item's visual center (Task 1) and that an off-canvas transform
	// gets nudged back on-screen (Task 2).
	CanvasDefinition def;
	def.name = "selftest-transform-pivot-canvas";
	def.isDefault = false;
	def.width = 1280;
	def.height = 720;
	def.fpsNum = 30;
	def.fpsDen = 1;
	const CanvasDefinition &added = g_canvases.Add(std::move(def));
	const std::string canvasUuid = added.uuid;
	g_canvasRuntime->EnsureCanvas(added);

	// Read the axis-aligned box of a scene item on the temp canvas's current
	// scene by id, mirroring bridge.cpp's GetSceneItemBox exactly (corner-
	// transform via obs_sceneitem_get_box_transform), but resolved locally
	// since the bridge doesn't expose a ready-made AABB over the wire.
	auto readBox = [&](int64_t itemId, vec3 &tl, vec3 &br) -> bool {
		obs_source_t *scene = g_canvasRuntime->CurrentScene(canvasUuid); // addref'd
		if (!scene) {
			return false;
		}
		struct FindCtx {
			int64_t id;
			obs_sceneitem_t *found;
		} fc{itemId, nullptr};
		obs_scene_enum_items(
			obs_scene_from_source(scene),
			[](obs_scene_t *, obs_sceneitem_t *it, void *p) -> bool {
				auto *c = static_cast<FindCtx *>(p);
				if (obs_sceneitem_get_id(it) == c->id) {
					c->found = it;
					return false;
				}
				return true;
			},
			&fc);
		if (!fc.found) {
			obs_source_release(scene);
			return false;
		}
		matrix4 boxTransform;
		obs_sceneitem_get_box_transform(fc.found, &boxTransform);
		vec3_set(&tl, M_INFINITE, M_INFINITE, 0.0f);
		vec3_set(&br, -M_INFINITE, -M_INFINITE, 0.0f);
		const float corners[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
		for (const auto &c : corners) {
			vec3 pos;
			vec3_set(&pos, c[0], c[1], 0.0f);
			vec3_transform(&pos, &pos, &boxTransform);
			vec3_min(&tl, &tl, &pos);
			vec3_max(&br, &br, &pos);
		}
		obs_source_release(scene);
		return true;
	};
	auto centerOf = [](const vec3 &tl, const vec3 &br) -> vec3 {
		vec3 c;
		vec3_set(&c, (tl.x + br.x) / 2.0f, (tl.y + br.y) / 2.0f, 0.0f);
		return c;
	};
	constexpr float kCenterEpsilonPx = 1.0f;

	bool ok = false;

	// 1) Create + select a scene on the temp canvas (its current channel-0 scene
	// is otherwise empty until one is set), then add a color source to it and
	// read its native size back through the bridge so the wide/off-center test
	// box is derived from the real source size rather than a guessed constant.
	const char *kSceneName = "selftest-transform-pivot-scene";
	run("scenes.create", json{{"canvas", canvasUuid}, {"name", kSceneName}}, ok);
	run("scenes.setCurrent", json{{"canvas", canvasUuid}, {"name", kSceneName}}, ok);
	HostLog(std::string("[selftest] transform-pivot scene setup -> ") + (ok ? "ok" : "FAIL (BUG)"));

	json srcCreated = run(
		"sources.create",
		json{{"canvas", canvasUuid}, {"type", "color_source"}, {"name", "selftest-transform-pivot-color"}}, ok);
	const int64_t itemId = ok ? srcCreated.value("id", int64_t(0)) : 0;
	const std::string srcName = ok ? srcCreated.value("source", std::string()) : std::string();
	HostLog(std::string("[selftest] transform-pivot sources.create -> ") +
		(itemId ? "id=" + std::to_string(itemId) + " source='" + srcName + "'" : "FAIL (BUG)"));
	if (!itemId) {
		g_canvasRuntime->RemoveCanvas(canvasUuid);
		g_canvases.Remove(canvasUuid);
		return;
	}

	json xform = run("sceneItems.getTransform", json{{"canvas", canvasUuid}, {"id", itemId}}, ok);
	const uint32_t srcW = ok ? xform.value("sourceWidth", uint32_t(0)) : 0;
	const uint32_t srcH = ok ? xform.value("sourceHeight", uint32_t(0)) : 0;
	const float scaleX = srcW ? 400.0f / float(srcW) : 1.0f;
	const float scaleY = srcH ? 100.0f / float(srcH) : 1.0f;

	// 2) Position it wide, off-center, non-uniformly scaled -- ~400x100 near a
	// corner of the 1280x720 canvas -- so a non-center rotation pivot would
	// visibly move it.
	run("sceneItems.setTransform",
	    json{{"canvas", canvasUuid},
		 {"id", itemId},
		 {"transform",
		  json{{"pos", json{{"x", 100.0}, {"y", 300.0}}}, {"scale", json{{"x", scaleX}, {"y", scaleY}}}}}},
	    ok);
	vec3 setupTl, setupBr;
	const bool haveSetupBox = ok && readBox(itemId, setupTl, setupBr);
	HostLog(std::string("[selftest] transform-pivot setup box -> ") + (haveSetupBox ? "ok" : "FAIL (BUG)"));

	// 3) Rotate 90 degrees via setTransform (no pos in the same call, so Task 1's
	// center-pivot correction is the only thing moving pos). Assert the visual
	// center barely moved.
	vec3 beforeRotTl, beforeRotBr;
	const bool haveBeforeRot = haveSetupBox && readBox(itemId, beforeRotTl, beforeRotBr);
	const vec3 centerBeforeRot = centerOf(beforeRotTl, beforeRotBr);
	run("sceneItems.setTransform", json{{"canvas", canvasUuid}, {"id", itemId}, {"transform", json{{"rot", 90.0}}}},
	    ok);
	vec3 afterRotTl, afterRotBr;
	const bool haveAfterRot = ok && readBox(itemId, afterRotTl, afterRotBr);
	const vec3 centerAfterRot = centerOf(afterRotTl, afterRotBr);
	const float rotDrift = haveBeforeRot && haveAfterRot ? std::hypot(centerAfterRot.x - centerBeforeRot.x,
									  centerAfterRot.y - centerBeforeRot.y)
							     : -1.0f;
	const bool rotCenterHeld = haveBeforeRot && haveAfterRot && rotDrift <= kCenterEpsilonPx;
	HostLog(std::string("[selftest] transform-pivot rotate-center (setTransform) -> ") +
		(rotCenterHeld ? "true" : "false (BUG)") + " (drift=" + std::to_string(rotDrift) + "px)");

	// 4) Reset rotation back to 0 (also exercises the center-pivot correction on
	// the way back), then drive the SAME property through transformAction's
	// rotate90cw entry point.
	run("sceneItems.setTransform", json{{"canvas", canvasUuid}, {"id", itemId}, {"transform", json{{"rot", 0.0}}}},
	    ok);
	vec3 beforeActionTl, beforeActionBr;
	const bool haveBeforeAction = ok && readBox(itemId, beforeActionTl, beforeActionBr);
	const vec3 centerBeforeAction = centerOf(beforeActionTl, beforeActionBr);
	run("sceneItems.transformAction", json{{"canvas", canvasUuid}, {"id", itemId}, {"action", "rotate90cw"}}, ok);
	vec3 afterActionTl, afterActionBr;
	const bool haveAfterAction = ok && readBox(itemId, afterActionTl, afterActionBr);
	const vec3 centerAfterAction = centerOf(afterActionTl, afterActionBr);
	const float actionDrift = haveBeforeAction && haveAfterAction
					  ? std::hypot(centerAfterAction.x - centerBeforeAction.x,
						       centerAfterAction.y - centerBeforeAction.y)
					  : -1.0f;
	const bool actionCenterHeld = haveBeforeAction && haveAfterAction && actionDrift <= kCenterEpsilonPx;
	HostLog(std::string("[selftest] transform-pivot rotate-center (transformAction) -> ") +
		(actionCenterHeld ? "true" : "false (BUG)") + " (drift=" + std::to_string(actionDrift) + "px)");

	// 5) Send it fully off-canvas via an extreme pos-only setTransform (no rot in
	// this call, staying clear of Task 1's pos-vs-rot gating), then assert the
	// clamp nudged it back to have positive overlap with the canvas.
	run("sceneItems.setTransform",
	    json{{"canvas", canvasUuid},
		 {"id", itemId},
		 {"transform", json{{"pos", json{{"x", -10000.0}, {"y", -10000.0}}}}}},
	    ok);
	vec3 clampedTl, clampedBr;
	const bool haveClampedBox = ok && readBox(itemId, clampedTl, clampedBr);
	const float overlapW = haveClampedBox ? std::min(clampedBr.x, 1280.0f) - std::max(clampedTl.x, 0.0f) : -1.0f;
	const float overlapH = haveClampedBox ? std::min(clampedBr.y, 720.0f) - std::max(clampedTl.y, 0.0f) : -1.0f;
	const bool clamped = haveClampedBox && overlapW > 0.0f && overlapH > 0.0f;
	HostLog(std::string("[selftest] transform-pivot off-canvas clamp -> ") + (clamped ? "true" : "false (BUG)") +
		" (overlapW=" + std::to_string(overlapW) + " overlapH=" + std::to_string(overlapH) + ")");

	// Clean up: remove the source + its item, then destroy the temp canvas,
	// returning the in-memory model to baseline (nothing Saved beyond what the
	// bridge calls above already do normally).
	run("sceneItems.remove", json{{"canvas", canvasUuid}, {"id", itemId}}, ok);
	obs_source_t *s = obs_get_source_by_name(srcName.c_str());
	if (s) {
		obs_source_remove(s);
		obs_source_release(s);
	}
	g_multistream->InvalidateCanvasEncoders(canvasUuid);
	g_canvasRuntime->RemoveCanvas(canvasUuid);
	g_canvases.Remove(canvasUuid);
	const bool gone = g_canvasRuntime->Find(canvasUuid) == nullptr && g_canvases.Find(canvasUuid) == nullptr;
	HostLog(std::string("[selftest] transform-pivot cleanup: temp canvas ") +
		(gone ? "removed" : "STILL PRESENT (BUG)"));
}

void ObsBootstrap::RunRotationBoundsSelfTest()
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

	// Bring up a temporary ADDITIONAL canvas, same in-memory-only pattern as the
	// transform-pivot self-test. The point: prove that a 90-degree rotation swaps
	// a non-square item's AABB width/height correctly for BOTH OBS_BOUNDS_NONE
	// (plain scale) and OBS_BOUNDS_SCALE_INNER (fixed bounds box) -- the
	// bounds-mode case is the specific hypothesis behind the user-reported
	// "selection outline stays sized for the old orientation after rotate" bug
	// (design spec S4): if a bounds-mode item's box_transform footprint doesn't
	// reflect the rotated shape, the outline -- which reads box_transform live
	// every frame, per the earlier static review of DrawSelection -- would
	// visibly lag even though the draw code itself is correct.
	CanvasDefinition def;
	def.name = "selftest-rotation-bounds-canvas";
	def.isDefault = false;
	def.width = 1280;
	def.height = 720;
	def.fpsNum = 30;
	def.fpsDen = 1;
	const CanvasDefinition &added = g_canvases.Add(std::move(def));
	const std::string canvasUuid = added.uuid;
	g_canvasRuntime->EnsureCanvas(added);

	// Read the axis-aligned box of a scene item by id -- identical to
	// RunTransformPivotSelfTest's readBox helper (bridge.cpp's GetSceneItemBox
	// isn't exposed outside its translation unit, so each self-test resolves its
	// own copy against the live libobs item).
	auto readBox = [&](int64_t itemId, vec3 &tl, vec3 &br) -> bool {
		obs_source_t *scene = g_canvasRuntime->CurrentScene(canvasUuid); // addref'd
		if (!scene) {
			return false;
		}
		struct FindCtx {
			int64_t id;
			obs_sceneitem_t *found;
		} fc{itemId, nullptr};
		obs_scene_enum_items(
			obs_scene_from_source(scene),
			[](obs_scene_t *, obs_sceneitem_t *it, void *p) -> bool {
				auto *c = static_cast<FindCtx *>(p);
				if (obs_sceneitem_get_id(it) == c->id) {
					c->found = it;
					return false;
				}
				return true;
			},
			&fc);
		if (!fc.found) {
			obs_source_release(scene);
			return false;
		}
		matrix4 boxTransform;
		obs_sceneitem_get_box_transform(fc.found, &boxTransform);
		vec3_set(&tl, M_INFINITE, M_INFINITE, 0.0f);
		vec3_set(&br, -M_INFINITE, -M_INFINITE, 0.0f);
		const float corners[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
		for (const auto &c : corners) {
			vec3 pos;
			vec3_set(&pos, c[0], c[1], 0.0f);
			vec3_transform(&pos, &pos, &boxTransform);
			vec3_min(&tl, &tl, &pos);
			vec3_max(&br, &br, &pos);
		}
		obs_source_release(scene);
		return true;
	};

	constexpr float kSwapEpsilonPx = 1.0f;

	bool ok = false;
	const char *kSceneName = "selftest-rotation-bounds-scene";
	run("scenes.create", json{{"canvas", canvasUuid}, {"name", kSceneName}}, ok);
	run("scenes.setCurrent", json{{"canvas", canvasUuid}, {"name", kSceneName}}, ok);
	HostLog(std::string("[selftest] rotation-bounds scene setup -> ") + (ok ? "ok" : "FAIL (BUG)"));

	// One case per bounds mode: a fresh color source given a known 400x100
	// footprint (non-square, so a correct 90-degree rotation must swap which
	// axis is longer), then rotated and re-measured. boundsType < 0 means "leave
	// at the item's default (OBS_BOUNDS_NONE)" and derive the footprint via scale
	// instead of bounds, matching how the plain-scale case is actually driven
	// through the Transform dialog.
	struct BoundsCase {
		const char *label;
		int boundsType;
		float boxW;
		float boxH;
	};
	const BoundsCase cases[] = {
		{"none", -1, 400.0f, 100.0f},
		{"scale_inner", static_cast<int>(OBS_BOUNDS_SCALE_INNER), 400.0f, 100.0f},
	};

	std::vector<int64_t> createdItemIds;
	std::vector<std::string> createdSourceNames;
	bool allSwapped = true;

	for (const BoundsCase &c : cases) {
		json srcCreated = run("sources.create",
				      json{{"canvas", canvasUuid},
					   {"type", "color_source"},
					   {"name", std::string("selftest-rotation-bounds-") + c.label}},
				      ok);
		const int64_t itemId = ok ? srcCreated.value("id", int64_t(0)) : 0;
		const std::string srcName = ok ? srcCreated.value("source", std::string()) : std::string();
		HostLog(std::string("[selftest] rotation-bounds sources.create (") + c.label + ") -> " +
			(itemId ? "id=" + std::to_string(itemId) : "FAIL (BUG)"));
		if (!itemId) {
			allSwapped = false;
			continue;
		}
		createdItemIds.push_back(itemId);
		createdSourceNames.push_back(srcName);

		json transform{{"pos", json{{"x", 100.0}, {"y", 300.0}}}};
		if (c.boundsType >= 0) {
			transform["boundsType"] = c.boundsType;
			transform["bounds"] = json{{"x", c.boxW}, {"y", c.boxH}};
		} else {
			json xform = run("sceneItems.getTransform", json{{"canvas", canvasUuid}, {"id", itemId}}, ok);
			const uint32_t srcW = ok ? xform.value("sourceWidth", uint32_t(0)) : 0;
			const uint32_t srcH = ok ? xform.value("sourceHeight", uint32_t(0)) : 0;
			transform["scale"] = json{{"x", srcW ? double(c.boxW) / double(srcW) : 1.0},
						  {"y", srcH ? double(c.boxH) / double(srcH) : 1.0}};
		}
		run("sceneItems.setTransform", json{{"canvas", canvasUuid}, {"id", itemId}, {"transform", transform}},
		    ok);

		vec3 beforeTl, beforeBr;
		const bool haveBefore = ok && readBox(itemId, beforeTl, beforeBr);
		const float widthBefore = haveBefore ? beforeBr.x - beforeTl.x : -1.0f;
		const float heightBefore = haveBefore ? beforeBr.y - beforeTl.y : -1.0f;

		run("sceneItems.setTransform",
		    json{{"canvas", canvasUuid}, {"id", itemId}, {"transform", json{{"rot", 90.0}}}}, ok);

		vec3 afterTl, afterBr;
		const bool haveAfter = ok && readBox(itemId, afterTl, afterBr);
		const float widthAfter = haveAfter ? afterBr.x - afterTl.x : -1.0f;
		const float heightAfter = haveAfter ? afterBr.y - afterTl.y : -1.0f;

		const bool swapped = haveBefore && haveAfter &&
				     std::fabs(widthAfter - heightBefore) <= kSwapEpsilonPx &&
				     std::fabs(heightAfter - widthBefore) <= kSwapEpsilonPx;
		allSwapped = allSwapped && swapped;
		HostLog(std::string("[selftest] rotation-bounds ") + c.label +
			" -> swapped=" + (swapped ? "true" : "false (BUG)") +
			" (before=" + std::to_string(widthBefore) + "x" + std::to_string(heightBefore) +
			" after=" + std::to_string(widthAfter) + "x" + std::to_string(heightAfter) + ")");
	}

	// Also drive the SAME bounds-mode item through sceneItems.transformAction's
	// rotate90cw entry point -- the actual "Rotate 90 CW" quick-action a user
	// clicks, distinct from the setTransform dialog path exercised above, and
	// the specific code path a frontend investigation of this bug flagged as
	// unverified (MethodSceneItemsTransformAction's rotate branch calls
	// obs_sceneitem_set_rot then RepositionForCenterPivot, rather than
	// obs_sceneitem_set_info2 -- same underlying update_item_transform, but
	// worth proving empirically rather than by inference alone).
	if (createdItemIds.size() == std::size(cases) && cases[1].boundsType >= 0) {
		const int64_t boundsItemId = createdItemIds[1];
		run("sceneItems.setTransform",
		    json{{"canvas", canvasUuid}, {"id", boundsItemId}, {"transform", json{{"rot", 0.0}}}}, ok);
		vec3 beforeTl, beforeBr;
		const bool haveBefore = ok && readBox(boundsItemId, beforeTl, beforeBr);
		const float widthBefore = haveBefore ? beforeBr.x - beforeTl.x : -1.0f;
		const float heightBefore = haveBefore ? beforeBr.y - beforeTl.y : -1.0f;

		run("sceneItems.transformAction",
		    json{{"canvas", canvasUuid}, {"id", boundsItemId}, {"action", "rotate90cw"}}, ok);
		vec3 afterTl, afterBr;
		const bool haveAfter = ok && readBox(boundsItemId, afterTl, afterBr);
		const float widthAfter = haveAfter ? afterBr.x - afterTl.x : -1.0f;
		const float heightAfter = haveAfter ? afterBr.y - afterTl.y : -1.0f;

		const bool swapped = haveBefore && haveAfter &&
				     std::fabs(widthAfter - heightBefore) <= kSwapEpsilonPx &&
				     std::fabs(heightAfter - widthBefore) <= kSwapEpsilonPx;
		allSwapped = allSwapped && swapped;
		HostLog(std::string("[selftest] rotation-bounds scale_inner-quickaction -> swapped=") +
			(swapped ? "true" : "false (BUG)") + " (before=" + std::to_string(widthBefore) + "x" +
			std::to_string(heightBefore) + " after=" + std::to_string(widthAfter) + "x" +
			std::to_string(heightAfter) + ")");
	}

	// Clean up: remove each item + its source, then destroy the temp canvas,
	// returning the in-memory model to baseline (nothing Saved beyond what the
	// bridge calls above already do normally).
	for (size_t i = 0; i < createdItemIds.size(); ++i) {
		run("sceneItems.remove", json{{"canvas", canvasUuid}, {"id", createdItemIds[i]}}, ok);
		obs_source_t *s = obs_get_source_by_name(createdSourceNames[i].c_str());
		if (s) {
			obs_source_remove(s);
			obs_source_release(s);
		}
	}
	g_multistream->InvalidateCanvasEncoders(canvasUuid);
	g_canvasRuntime->RemoveCanvas(canvasUuid);
	g_canvases.Remove(canvasUuid);
	const bool gone = g_canvasRuntime->Find(canvasUuid) == nullptr && g_canvases.Find(canvasUuid) == nullptr;
	HostLog(std::string("[selftest] rotation-bounds cleanup: temp canvas ") +
		(gone ? "removed" : "STILL PRESENT (BUG)"));
	HostLog(std::string("[selftest] rotation-bounds overall -> ") + (allSwapped ? "PASS" : "FAIL (BUG)"));
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
		"; default sel before=" + std::to_string(defaultSelBefore) +
		" after=" + std::to_string(defaultSelAfter) + " (isolation " +
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
	HostLog(std::string("[selftest] preview-isolation: canvas item move -> ") +
		(movedOk ? "ok (restored)" : "FAIL"));

	// Clear the additional surface's selection so it leaves no committed state, then
	// tear down the temp canvas (drops its surface's mix; the surface itself is
	// reaped by DestroyAll at shutdown, but its display already has no mix to render
	// once the canvas is gone -- so destroy the surface now to keep ordering clean).
	Preview::SelectFromBridge(canvasUuid, "", 0, false);
	Preview::Instance()->DestroyForCanvas(canvasUuid);

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
	obs_source_t *audioSrc =
		obs_source_create("wasapi_output_capture", GlobalAudioChannels::kSelfTestSourceName, nullptr, nullptr);
	if (!audioSrc) {
		HostLog("[selftest] audio-mixer: wasapi_output_capture create FAILED (skipping)");
		return;
	}
	obs_set_output_source(kTempChannel, audioSrc);

	const char *uuidPtr = obs_source_get_uuid(audioSrc);
	const std::string uuid = uuidPtr ? uuidPtr : std::string();
	const bool audioActive = obs_source_audio_active(audioSrc);
	HostLog("[selftest] audio-mixer temp source created -> uuid=" + uuid +
		" audioActive=" + (audioActive ? "true" : "false"));

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
	const size_t finalCount = (ok && list2.is_object() && list2["sources"].is_array()) ? list2["sources"].size()
											   : 0;
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
	HostLog("[selftest] hotkeys.list -> " + std::to_string(total) + " hotkey(s), " + std::to_string(frontendCount) +
		" frontend; Start Streaming id=" + (startId.empty() ? "(MISSING -- BUG)" : startId));
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

void ObsBootstrap::RunNativeThemeSelfTest()
{
	// The only owned window up at smoke time is the host, so a >= 1 count proves its
	// residual chrome was darkened at startup. Pixels can't be checked headlessly.
	const int applied = NativeTheme::AppliedCount();
	if (applied < 1) {
		HostLog("[selftest] native-theme FAIL: host chrome not darkened (AppliedCount=0)");
		return;
	}
	HostLog("[selftest] native-theme OK: AppliedCount=" + std::to_string(applied));
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
		(hasServerInfo
			 ? " (name='" + init["result"]["serverInfo"].value("name", std::string("?")) +
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
	json scenesCall = call(
		json{{"jsonrpc", "2.0"},
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
		(escalationBlocked ? "true" : "false") +
		" getConfig blocked=" + (configReadBlocked ? "true" : "false") +
		(mcpNsGuarded ? " (OK, no self-config via MCP)" : " (MISMATCH -- privilege escalation!)"));

	if (hasServerInfo && hasObsCall && curatedOk && scenesOk && scenesArray && gateOk && statsOk && statsObject &&
	    mcpNsGuarded) {
		HostLog("[selftest] mcp -> initialize/tools.list/obs_call/curated OK; go-live + mcp.* gated OK");
	} else {
		HostLog("[selftest] mcp -> FAILED (see step lines above)");
	}

	server.Stop();
}

void ObsBootstrap::RunEventSelfTest()
{
	// Snapshot the user's real events.json (+ its .bak) so the synthetic ingests below
	// can exercise the persist / dedupe / round-trip path without clobbering real
	// history; restored verbatim at the end (mirrors the model self-test discipline).
	auto snapshot = [](const std::string &p) -> std::optional<std::string> {
		std::ifstream in(std::filesystem::u8path(p), std::ios::binary);
		if (!in) {
			return std::nullopt;
		}
		return std::optional<std::string>(std::in_place, std::istreambuf_iterator<char>(in),
						  std::istreambuf_iterator<char>());
	};
	auto restore = [](const std::string &p, const std::optional<std::string> &data) {
		if (data) {
			std::ofstream out(std::filesystem::u8path(p), std::ios::binary | std::ios::trunc);
			out.write(data->data(), static_cast<std::streamsize>(data->size()));
		} else {
			std::error_code ec;
			std::filesystem::remove(std::filesystem::u8path(p), ec);
		}
	};

	const std::string path = Events::EventStore::FilePath();
	const std::string bakPath = path + ".bak";
	const std::optional<std::string> origMain = snapshot(path);
	const std::optional<std::string> origBak = snapshot(bakPath);

	// Start from a known-empty store so the counts below are deterministic regardless
	// of any pre-existing history.
	Events::Store().Clear();

	auto makeEvent = [](const std::string &id, const std::string &type, int64_t ts) {
		Events::NormalizedEvent ev;
		ev.id = id;
		ev.platform = "twitch";
		ev.type = type;
		ev.ts = ts;
		ev.actorName = "selftest-" + id;
		return ev;
	};

	// Ingest three synthetic events (distinct ids) through the hub -> store -> emit path.
	Events::Hub().Ingest(makeEvent("selftest-ev-1", "follow", 1000));
	Events::Hub().Ingest(makeEvent("selftest-ev-2", "cheer", 2000));
	Events::Hub().Ingest(makeEvent("selftest-ev-3", "raid", 3000));

	const std::vector<Events::NormalizedEvent> list = Events::Store().List();
	const bool orderOk = list.size() == 3 && list.front().id == "selftest-ev-3" &&
			     list.back().id == "selftest-ev-1";
	HostLog(std::string("[selftest] events ingest -> ") + std::to_string(list.size()) +
		" (expect 3, newest-first " + (orderOk ? "OK" : "MISMATCH") + ")");

	// Dedupe: re-Ingest an existing id -> count must stay 3.
	Events::Hub().Ingest(makeEvent("selftest-ev-2", "cheer", 2000));
	const size_t afterDup = Events::Store().List().size();
	HostLog(std::string("[selftest] events dedupe -> ") + std::to_string(afterDup) +
		(afterDup == 3 ? " (OK, duplicate id rejected)" : " (BUG, duplicate stored)"));

	// Persistence round-trip: a fresh store loads the just-saved events.json.
	Events::EventStore reloaded;
	const size_t reloadedCount = reloaded.List().size();
	HostLog(std::string("[selftest] events round-trip -> reloaded ") + std::to_string(reloadedCount) +
		(reloadedCount == 3 ? " (OK)" : " (MISMATCH)"));

	// Restore: wipe the synthetic history from memory, then put the user's original
	// files back byte-for-byte (or remove them if there were none).
	Events::Store().Clear();
	restore(path, origMain);
	restore(bakPath, origBak);
	HostLog(std::string("[selftest] events cleanup -> restored user events.json ") +
		(origMain ? "(original contents)" : "(removed test file)"));
}

void ObsBootstrap::Stop(void (*drainCefTasks)())
{
	DBG(LogCat::Lifecycle, "bootstrap stop");

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
	g_globalAudio.Clear();

	// Deferred source destruction can cascade across the destruction-task
	// thread; drain in a loop until no more work is spawned before
	// obs_shutdown, mirroring the Qt frontend's ClearSceneData.
	while (obs_wait_for_destroy_queue()) {
	}

	// Drop the canvas service first: it holds references to the engine + runtime +
	// model reset below. Stateless beyond those references (no libobs resources of
	// its own), so a bare reset is enough.
	g_canvasService.reset();

	// Tear the engine down before the stores it references clear and before
	// obs_shutdown: StopAll releases its services/outputs/encoders while libobs
	// is still up. The dtor also calls StopAll (defensive), but reset here so the
	// order against Clear() is explicit. Null onStatusChanged FIRST: obs_output_stop
	// inside StopAll can fire its "stop" signal asynchronously on the output thread,
	// whose OnOutputStop -> EmitMultistreamChanged posts a TID_UI task that outlives
	// this reset(). CefShutdown drains that task later and would call
	// BuildStatusArray -> Multistream() on the now-null g_multistream. Disconnecting
	// the callback stops new emits at the source; EmitMultistreamChanged also guards
	// any already-queued task with MultistreamAlive().
	if (g_multistream) {
		g_multistream->onStatusChanged = nullptr;
		// Same rationale: StopAll's async stop fires UpdateSleepInhibit -> onLiveStateChanged,
		// which would post a priority re-pin that outlives this reset. Disconnect it here.
		g_multistream->onLiveStateChanged = nullptr;
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

	// The sweep above is where browser sources restored from a loaded scene
	// collection actually die, and BrowserSource::Destroy only *posts* its
	// `delete this` + browser close to TID_UI -- the destroy-queue drain above
	// cannot run those tasks. Pump CEF now, while libobs is still up, so no live
	// CefBrowser survives into CefShutdown (libcef CHECK-crashes on that) and no
	// CEF close task runs after obs_shutdown.
	if (drainCefTasks) {
		drainCefTasks();
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

	// Self-gates on OBS_TRACK_ALLOCS=1; a no-op otherwise. Dumps symbolized
	// stacks for every still-outstanding allocation before the count below.
	bmem_dump_outstanding();

	// Same counter the legacy frontend prints. Nonzero == fixed libobs static
	// residuals (no per-run growth), not host-introduced leaks.
	HostLog("[obs] leaks: " + std::to_string(bnum_allocs()));
	HostLog("[obs] shutdown complete");

	// Restore the stderr/HostLog handler and close the session log file last, after
	// obs_shutdown's own blog output has been captured.
	SessionLog::Shutdown();
}
