#include "obs_bootstrap.hpp"
#include "event_names.hpp"

#include <obs.h>
#include <obs-frontend-internal.hpp>
#include <util/base.h>
#include <util/platform.h>
#include <util/profiler.h>

#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>

#include <windows.h>
#include <util/windows/WinHandle.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "util/async_task.hpp"
#include "util/env_config.hpp"
#include "util/file_util.hpp"
#include "util/json_util.hpp"
#include "util/op_error.hpp"
#include "util/random_util.hpp"
#include "util/string_util.hpp"
#include "audio/AudioMonitor.hpp"
#include "bridge.hpp"
#include "build_info.hpp"
#include "devtools_port.hpp"
#include "settings/DiagnosticsSettings.hpp"
#include "frontend_callbacks.hpp"
#include "gpu_safe_mode.hpp"
#include "log.hpp"
#include "overlay/overlay_viewport.hpp"
#include "chat/channel_stats_poller.hpp"
#include "chat/chat_hub.hpp" // Chat::BindingDestination, Chat::Hub
#include "chat/poll_registry.hpp"
#include "chat/twitch_chat.hpp"
#include "chat/youtube_innertube.hpp"
#include "chat/youtube_poll.hpp"
#include "events/event_hub.hpp"
#include "events/event_store.hpp"
#include "events/kick_events.hpp"
#include "events/transport_health.hpp"
#include "history/Db.hpp"
#include "history/SessionRecorder.hpp"
#include "history/ScheduleRunner.hpp"
#include "history/ScheduleStore.hpp"
#include "history/ScheduledSetup.hpp"
#include "history/SessionStore.hpp"
#include "history/Thumbnails.hpp"
#include "multistream/CanvasRuntime.hpp"
#include "multistream/VideoGate.hpp"
#include "multistream/CanvasService.hpp"
#include "multistream/CanvasStore.hpp"
#include "multistream/GlobalAudioChannels.hpp"
#include "multistream/Hotkeys.hpp"
#include "mcp/McpServer.hpp"
#include "multistream/MultistreamEngine.hpp"
#include "multistream/OutputBindingStore.hpp"
#include "multistream/SceneLinkStore.hpp"
#include "multistream/StreamInfoPresetStore.hpp"
#include "multistream/PollTemplateStore.hpp"
#include "multistream/StreamMetaStore.hpp"
#include "multistream/StorePaths.hpp"
#include "multistream/StreamProfileStore.hpp"
#include "multistream/VirtualCamManager.hpp"
#include "util/time_util.hpp"
#include "oauth/account_store.hpp"
#include "oauth/registry.hpp"
#include "overlay/overlay_server.hpp"
#include "overlay/overlay_sources.hpp"
#include "overlay/overlay_store.hpp"
#include "settings/AdvancedSettings.hpp"
#include "settings/GeneralSettings.hpp"
#include "util/paths.hpp"
#include "windowing/native_theme.hpp"
#include "windowing/filter_preview.hpp"
#include "windowing/preview_window.hpp"
#include "windowing/projector_window.hpp"
#include "scene/scene_collections.hpp"
#include "util/session_log.hpp"
#include "util/speaker_layout.hpp"
#include "oauth/youtube_snippet.hpp"
#include "scene/main_channel.hpp"
#include "scene/scene_items.hpp"
#include "scene/scene_persistence.hpp"
#include "scene/transitions.hpp"
#include "target_destinations.hpp"
#include "UndoManager.hpp"

// kSelfTestOutputChannel is a bare literal whose whole justification is that it sits inside the
// span the scene-save filter excludes, so nothing an interrupted self-test leaves bound can be
// persisted into a scene collection. Narrowing that span elsewhere would silently falsify it.
static_assert(ObsBootstrap::kSelfTestOutputChannel >= GlobalAudioChannels::kFirstChannel &&
		      ObsBootstrap::kSelfTestOutputChannel <= GlobalAudioChannels::kLastChannel,
	      "the self-test output channel must stay inside the span the scene-save filter excludes");

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

std::string BaseNameNoExt(const std::string &filename)
{
	const size_t dot = filename.find_last_of('.');
	return dot == std::string::npos ? filename : filename.substr(0, dot);
}

// Whether the gpudiag sampler was requested via BRAIDCAST_DEBUG_COMPONENTS;
// resolved once in Start() and read back by ObsBootstrap::GpuDiagRequested().
bool g_gpuDiagRequested = false;

// Whether Start() turned the OBS profiler on, so Stop() knows to dump and free
// it. Decided once at boot: the profiler is process-global and taxes every
// profiled thread, so the runtime render-debug toggle does not move it.
bool g_profilerStarted = false;

// Resolve the two-var debug scheme into the applied config. The master
// BRAIDCAST_DEBUG (env -> .env -> persisted DiagnosticsSettings.debugLogging ->
// off) is a pure boolean; while on, BRAIDCAST_DEBUG_COMPONENTS (env -> .env ->
// empty) selects the categories + subsystems, defaulting to kDefaultCats (every
// category except the render firehose) when empty/unset. Log::ParseComponents
// owns the component vocabulary.
Log::DebugComponents ResolveDebugConfig()
{
	bool master;
	if (const std::optional<std::string> raw = Env::Raw("BRAIDCAST_DEBUG")) {
		master = StringUtil::ParseBool(*raw);
	} else {
		DiagnosticsSettings ds;
		ds.Load();
		master = ds.debugLogging;
	}
	if (!master) {
		return Log::DebugComponents{};
	}

	const std::optional<std::string> comps = Env::Raw("BRAIDCAST_DEBUG_COMPONENTS");
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
		const std::string lname = StringUtil::ToLower(name);

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

	MainChannel::Set(obs_scene_get_source(scene));
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

// Live-ness mirrored out of the engine at every live transition. Static storage
// duration on purpose: the detached account/event poller workers read it and are
// documented to outlive Stop(), so they must not touch g_multistream itself.
std::atomic<bool> g_anyOutputLive{false};

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

// The saved stream-info presets. Same shape as g_streamMeta: trivial ctor, Load()ed early
// in Start(), a plain member with no teardown state of its own. Its own file so a corrupt
// presets document cannot cost the user their remembered defaults.
StreamInfoPresetStore g_streamInfoPresets;

// The saved live-poll templates. Same shape as g_streamInfoPresets.
PollTemplateStore g_pollTemplates;

// The stream-history database and its two users. Db migrates at Start(); a failure
// degrades history to unavailable rather than aborting startup, because streaming
// must never depend on the archive. All three are UI-thread-only.
History::Db g_historyDb;
History::SessionStore g_sessions;
History::ScheduleStore g_schedule;
History::ScheduleRunner g_scheduleRunner;
History::SessionRecorder g_recorder;
History::ThumbnailSampler g_thumbs;

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
// The destinations a session opens with: every binding currently going out, with
// the metadata the platform actually accepted rather than what the profile says.
// Bridge::TakeSentMetadata consumes its entry, so this runs exactly once per
// session -- a second call would find the bag already emptied.
std::vector<History::DestinationRecord> LiveDestinations(const MultistreamEngine &engine)
{
	std::vector<History::DestinationRecord> out;
	for (const MultistreamEngine::OutputStatus &st : engine.Statuses()) {
		if (!MultistreamEngine::IsActiveState(st.state)) {
			continue;
		}
		// OutputStatus carries no profile uuid -- only a display label -- so the
		// binding is what maps a live output back to its credential.
		const OutputBinding *b = g_outputBindings.Bindings().Find(st.bindingUuid);
		if (!b) {
			continue;
		}
		History::DestinationRecord d;
		d.bindingUuid = st.bindingUuid;
		d.profileId = b->profileUuid;
		if (const StreamProfile *p = g_streamProfiles.Find(b->profileUuid)) {
			d.platform = p->PlatformKey();
			d.accountLabel = p->label;
		}
		const Bridge::json meta = Bridge::TakeSentMetadata(b->profileUuid);
		if (meta.is_object()) {
			if (meta.contains("title") && meta["title"].is_string()) {
				d.title = meta["title"].get<std::string>();
			}
			// An object of {id, name}, which is the only shape a provider is
			// ever handed one in. Read as a bare string this always came back
			// empty, so every recorded session claimed no category at all. The
			// history card wants the readable half.
			if (meta.contains("category") && meta["category"].is_object()) {
				d.category = JsonUtil::Str(meta["category"], "name");
			}
			if (meta.contains("tags") && meta["tags"].is_array()) {
				for (const auto &tag : meta["tags"]) {
					if (tag.is_string()) {
						d.tags.push_back(tag.get<std::string>());
					}
				}
			}
		}
		out.push_back(std::move(d));
	}
	return out;
}

// --- loading a scheduled entry into the go-live path ------------------------
//
// Going live for a scheduled entry means the destinations it names become the
// enabled routing, and the metadata it carries becomes the per-stream override bag
// CollectBroadcastPrelude merges over the channel defaults and hands to the
// provider. That is configuration the user owns, so it is theirs again the moment
// the broadcast ends. The capture-and-restore itself lives in ScheduledSetup, which
// knows nothing about libobs or either store and is tested on its own; what follows
// is only the wiring into them.
History::ScheduledSetup g_scheduledSetup;

// Is a broadcast running right now. Read from the engine rather than the published
// AnyOutputLive() flag: both the runner and the setup ask on the UI thread where
// the engine is valid, and refusing a scheduled start on a stale reading is a
// broadcast that does not happen.
bool AnythingLive()
{
	return ObsBootstrap::MultistreamAlive() && ObsBootstrap::Multistream().AnyLive();
}

// What both the runner and the setup mean by "is a broadcast running": the outputs
// that have come up, plus a go-live prelude that has not brought any up yet.
//
// A prelude counts even though AnythingLive() is false throughout it -- it runs
// strictly before the first encoder starts. It has already snapshotted the routing it
// is creating broadcasts and pushing metadata against, and it starts whatever is
// enabled at the moment it lands, seconds later. So an entry that re-routes underneath
// one sends those prepared broadcasts nowhere and puts its own destinations on the air
// with nothing created for them, while the start it made to go with the re-routing is
// silently dropped -- only one prelude may be in flight.
//
// UI thread only, which is where the runner, the setup and the engine all are.
bool StreamingOrGoingLive()
{
	return AnythingLive() || Bridge::GoLivePreludeInFlight();
}

// The binding that routes `profileId`, or empty when nothing does. A profile can be
// bound on several canvases but only one may be enabled (one RTMP key = one live
// stream), so a scheduled entry takes the first. `enabledOnly` narrows that to a
// binding that is actually on the air, which is a different question from whether the
// profile is routed anywhere at all.
std::string BindingForProfile(const std::string &profileId, bool enabledOnly)
{
	for (const OutputBinding &b : g_outputBindings.Bindings().bindings) {
		if (!profileId.empty() && b.profileUuid == profileId && (!enabledOnly || b.enabled)) {
			return b.uuid;
		}
	}
	return {};
}

// Skips a flip the binding has already made: the shared setter persists and emits
// unconditionally, so re-asserting an unchanged set would rewrite the bindings file
// once per binding for nothing.
//
// Answers whether the binding holds `enabled` afterwards, which is not the same as
// what the setter returns: it refuses the single-live-stream case before touching
// anything, but a failed save is reported after the flag, the output stop and the
// reconcile have already happened. Reporting that as "nothing changed" would leave
// a real change unrecorded and so unrestorable.
bool SetBindingEnabled(const std::string &uuid, bool enabled)
{
	const OutputBinding *b = g_outputBindings.Bindings().Find(uuid);
	if (!b) {
		return false;
	}
	if (b->enabled == enabled) {
		return true;
	}
	std::string err;
	const bool ok = Bridge::SetOutputBindingEnabled(uuid, enabled, err);
	const OutputBinding *after = g_outputBindings.Bindings().Find(uuid);
	const bool landed = after && after->enabled == enabled;
	if (!ok) {
		// Distinguished in the log because the two read as opposite events: a
		// refusal left the routing alone, while a save failure already changed it
		// in the running app and only lost the record on disk.
		const std::string verb = enabled ? "enable" : "disable";
		HostLog(landed ? "[schedule] applied but did not save the " + verb +
					 " of a scheduled destination: " + err
			       : "[schedule] could not " + verb + " a scheduled destination: " + err);
	}
	return landed;
}

std::vector<std::string> LiveCanvasUuids(const MultistreamEngine &engine)
{
	std::vector<std::string> out;
	for (const MultistreamEngine::OutputStatus &st : engine.Statuses()) {
		if (!MultistreamEngine::IsActiveState(st.state)) {
			continue;
		}
		if (std::find(out.begin(), out.end(), st.canvasUuid) == out.end()) {
			out.push_back(st.canvasUuid);
		}
	}
	return out;
}

// What the history list calls this broadcast. The title a platform accepted is
// the most specific thing anyone typed for it; with none sent (a stream-key
// destination with no linked account) the active scene collection is the only
// name the session has.
std::string BuildSessionTitle(const std::vector<History::DestinationRecord> &destinations)
{
	for (const History::DestinationRecord &d : destinations) {
		if (!d.title.empty()) {
			return d.title;
		}
	}
	const SceneCollectionRecord *active = g_sceneCollections.Active();
	return active ? active->name : std::string();
}

// "failed" only when every destination ended in error -- one dead destination out
// of four is a partial outage the per-destination rows already record, not a
// failed broadcast.
std::string SessionEndReason(const MultistreamEngine &engine)
{
	bool sawAny = false;
	for (const MultistreamEngine::OutputStatus &st : engine.Statuses()) {
		sawAny = true;
		if (st.state != MultistreamEngine::State::Error) {
			return "ended";
		}
	}
	return sawAny ? "failed" : "ended";
}

// Close the open session row, thumbnail included. Finalize has to run BEFORE End:
// SetThumbnail only writes while the session is open, so the other order drops the
// thumbnail on every stream without erroring. Every path that ends a broadcast --
// the live-state edge and a clean shutdown mid-broadcast -- goes through here so
// that ordering exists once. Callers own the IsRecording() check and whatever their
// own path emits afterward.
void EndSessionWithThumbnail(const std::string &reason)
{
	g_thumbs.Finalize(g_recorder);
	g_recorder.End(TimeUtil::NowMs(), reason);
}

// Map the stats snapshot onto a health sample. The field names come from
// BuildStatsSnapshot; bitrate and drops sum across destinations while congestion
// takes the worst, since one congested destination is the problem worth seeing.
History::HealthSample SampleFromSnapshot(const Bridge::json &snapshot)
{
	History::HealthSample s;
	if (!snapshot.is_object()) {
		return s;
	}
	if (snapshot.contains("sampledAtMs") && snapshot["sampledAtMs"].is_number()) {
		s.tMs = snapshot["sampledAtMs"].get<int64_t>();
	}
	if (snapshot.contains("general") && snapshot["general"].is_object()) {
		const Bridge::json &g = snapshot["general"];
		if (g.contains("cpu") && g["cpu"].is_number()) {
			s.cpuPct = g["cpu"].get<double>();
		}
		if (g.contains("encodeSkipped") && g["encodeSkipped"].is_number()) {
			s.cumulativeEncodeSkipped = g["encodeSkipped"].get<int64_t>();
		}
	}
	if (snapshot.contains("outputs") && snapshot["outputs"].is_array()) {
		for (const Bridge::json &o : snapshot["outputs"]) {
			if (!o.is_object()) {
				continue;
			}
			if (o.contains("bitrateKbps") && o["bitrateKbps"].is_number()) {
				s.bitrateKbps += static_cast<int64_t>(o["bitrateKbps"].get<double>());
			}
			if (o.contains("droppedFrames") && o["droppedFrames"].is_number()) {
				s.cumulativeDroppedFrames += o["droppedFrames"].get<int64_t>();
			}
			if (o.contains("congestionPct") && o["congestionPct"].is_number()) {
				s.congestionPct = std::max(s.congestionPct, o["congestionPct"].get<double>());
			}
		}
	}
	return s;
}

void LoadMultistreamModel()
{
	g_canvases.Load();
	// EnsureDefaultEncoders first and unconditionally: it is the one with the side
	// effect, so it must not sit behind a short-circuit. Load() has already numbered
	// whatever came back unnumbered; writing that back is what keeps the number stable
	// against a later reorder.
	const bool seededEncoders = g_canvases.EnsureDefaultEncoders();
	if (seededEncoders || g_canvases.NumbersMigrated()) {
		g_canvases.Save();
	}
	g_streamProfiles.Load();
	g_streamMeta.Load();
	g_streamInfoPresets.Load();
	g_pollTemplates.Load();
	g_outputBindings.Load();
	// Canvases loaded above, so a binding pointing at one that is gone is provably
	// an orphan rather than a load-ordering artifact.
	ObsBootstrap::ReconcileOutputBindings();
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

::StreamInfoPresetStore &ObsBootstrap::StreamInfoPresets()
{
	return g_streamInfoPresets;
}

::PollTemplateStore &ObsBootstrap::PollTemplates()
{
	return g_pollTemplates;
}

History::SessionStore &ObsBootstrap::Sessions()
{
	return g_sessions;
}

History::ScheduleStore &ObsBootstrap::Schedule()
{
	return g_schedule;
}

History::ScheduleRunner &ObsBootstrap::Scheduler()
{
	return g_scheduleRunner;
}

History::ScheduledSetup &ObsBootstrap::ScheduledSetup()
{
	return g_scheduledSetup;
}

History::SessionRecorder &ObsBootstrap::Recorder()
{
	return g_recorder;
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

size_t ObsBootstrap::PruneOutputBindingsForCanvas(const std::string &canvasUuid)
{
	auto &bindings = OutputBindings().Bindings().bindings;
	const size_t before = bindings.size();
	bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
				      [&canvasUuid](const OutputBinding &b) { return b.canvasUuid == canvasUuid; }),
		       bindings.end());
	const size_t removed = before - bindings.size();
	if (removed > 0) {
		OutputBindings().Save();
	}
	return removed;
}

size_t ObsBootstrap::ReconcileOutputBindings()
{
	const auto &defs = Canvases().Definitions();
	if (defs.empty()) {
		return 0;
	}
	auto &bindings = OutputBindings().Bindings().bindings;
	const size_t before = bindings.size();
	bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
				      [&defs](const OutputBinding &b) {
					      return std::none_of(defs.begin(), defs.end(),
								  [&b](const CanvasDefinition &d) {
									  return d.uuid == b.canvasUuid;
								  });
				      }),
		       bindings.end());
	const size_t removed = before - bindings.size();
	if (removed > 0) {
		OutputBindings().Save();
		// Loud: this deletes something the user configured. Silence here is what let
		// the orphans accumulate unnoticed in the first place.
		HostLog("[obs] multistream: dropped " + std::to_string(removed) +
			" output binding(s) routing to a canvas that no longer exists");
	}
	return removed;
}

MultistreamEngine &ObsBootstrap::Multistream()
{
	// Valid only between Start() (constructs g_multistream after the stores load)
	// and Stop() (resets it). Callers reach it either as a bridge method driven by JS,
	// or as a task posted to the CEF UI thread (the bridge's stats sampler). Neither
	// can run before CefRunMessageLoop, which main.cpp enters only after Start()
	// returns, so the pointer is non-null on every reachable path.
	return *g_multistream;
}

bool ObsBootstrap::MultistreamAlive()
{
	return g_multistream != nullptr;
}

bool ObsBootstrap::AnyOutputLive()
{
	return g_anyOutputLive.load(std::memory_order_acquire);
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

// libobs' UI-task handler. obs_queue_task(OBS_TASK_UI, ...) logs and DROPS the task
// when none is installed, which is what silently disabled win-wasapi's response to the
// Windows default output device changing (it queues obs_reset_audio_monitoring).
//
// wait == false is the only shape any caller in this build uses -- win-wasapi's
// device-change callback, on a WASAPI notification thread, and libobs's monitoring
// deduplication handover, from whichever thread made the decision -- and it maps straight
// onto PostToUi, which runs the task at once when the caller is already the UI thread.
//
// wait == true deliberately does not block indefinitely. obs_queue_task(OBS_TASK_UI) is
// reachable from the graphics and audio threads, and the UI thread routinely blocks on
// those (obs_reset_video joins the graphics thread), so an unbounded wait deadlocks the
// moment the two cross. PostToUi also drops the task outright once the bridge is torn
// down, which would turn that wait from long into permanent. So it gets a ceiling and
// gives up loudly; the task may still run afterwards, so an expiry means "not
// confirmed", never "did not happen".
static void UiTaskHandler(obs_task_t task, void *param, bool wait)
{
	if (!wait) {
		AsyncTask::PostToUi([task, param] { task(param); });
		return;
	}

	// A deadlock ceiling rather than a tuned budget: nothing in this build reaches
	// this branch, so the value only has to be long enough that a merely busy UI
	// thread is not mistaken for a wedged one.
	constexpr std::chrono::seconds kUiTaskWait{5};
	const std::optional<bool> ran = AsyncTask::CallOnUiWithTimeout<bool>(
		[task, param] {
			task(param);
			return true;
		},
		kUiTaskWait);
	if (!ran) {
		HostLog("[obs] blocking UI task not confirmed within 5s; it may still run later");
	}
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

	// Stamp the exact build first, unconditionally: every session log then names the tree
	// it ran (git describe), so "was this the fixed binary?" is a one-line check, not
	// timestamp forensics.
	HostLog(std::string("[lifecycle] braidcast build ") + Braidcast::BuildDescribe());

	// Resolve + apply the two-var debug scheme before anything else logs: master
	// BRAIDCAST_DEBUG gates, BRAIDCAST_DEBUG_COMPONENTS selects. Off by default ->
	// DBG() costs nothing. The resolved gpudiag flag is stashed for GpuDiag::Start.
	const Log::DebugComponents dbg = ResolveDebugConfig();
	Log::SetDebugMask(dbg.logMask);
	g_gpuDiagRequested = dbg.gpuDiag;
	DBG(LogCat::Lifecycle, "bootstrap start (debug categories=0x%x gpudiag=%d)", (unsigned)Log::DebugMask(),
	    dbg.gpuDiag ? 1 : 0);

	// Every decision about the remote-debugging port was taken before CefInitialize,
	// back in main.cpp, and this is the first point in the boot where a blog() reaches
	// the session log -- so the findings queued along the way are flushed here, ahead
	// of the state summary they explain.
	DevToolsPort::FlushPendingWarnings();

	if (const uint16_t devToolsPort = DevToolsPort::Active()) {
		// The port is reported because the gate authorized it, and nothing observed at
		// runtime is allowed to talk that down: over-reporting an open port is a
		// harmless false alarm, while under-reporting one is the failure this feature
		// exists to prevent.
		blog(LOG_WARNING,
		     "[devtools] CEF remote debugging is ENABLED on 127.0.0.1:%u. Anything that can reach that "
		     "port executes arbitrary JavaScript inside the app's webview origin -- the origin holding "
		     "your connected accounts' OAuth tokens and your resolved stream keys. It is on because "
		     "BRAIDCAST_DEBUG is set and BRAIDCAST_DEBUG_COMPONENTS names 'devtools'; clearing either "
		     "and relaunching closes it.",
		     static_cast<unsigned>(devToolsPort));
	}

	// One derivation of the boot-time render-debug state, shared with the
	// obs_set_render_debug call below so the two cannot drift apart at startup.
	// Nothing between here and there touches the debug mask. The runtime
	// diagnostics.setDebug toggle can still move the log gate afterwards without
	// moving the profiler; see g_profilerStarted.
	const bool renderDebug = Log::DebugEnabled(LogCat::Render);
	const bool renderGpuDebug = Log::DebugEnabled(LogCat::RenderGpu);

	// Arm the OBS profiler ahead of obs_startup so its own startup and module-load
	// nodes are captured. Opt in with the render category
	// (BRAIDCAST_DEBUG=1 BRAIDCAST_DEBUG_COMPONENTS=render), so an ordinary run
	// pays nothing.
	g_profilerStarted = renderDebug;
	if (g_profilerStarted) {
		profiler_start();
		HostLog("[obs] profiler started");
	}

	// Nothing frees the profiler when Start() bails, deliberately. main.cpp reaches
	// Teardown() with g_obsStarted false, so Stop() -- and with it obs_shutdown --
	// never runs, and the graphics thread obs_reset_video created below is still
	// calling profile_start/profile_end. profiler_free() ends by destroying a
	// statically initialized root mutex that is never re-initialized, so freeing
	// here would race that thread into a use-after-destroy. Leaking the roots as
	// the process exits is strictly safer than the alternative.

	if (!obs_startup("en-US", nullptr, nullptr)) {
		HostLog("[obs] obs_startup failed");
		return false;
	}
	HostLog("[obs] obs_startup ok");

	// Before LoadCuratedModules below: win-wasapi arms its default-device notification
	// inside obs_module_load, and that callback can fire the moment it is armed. With
	// no handler installed obs_queue_task logs and DROPS the task, which is why audio
	// monitoring stopped following the Windows default output device.
	//
	// Registration also queues set_ui_thread through the handler. Start() runs on the
	// thread that called CefInitialize with multi_threaded_message_loop off, which is
	// TID_UI, so PostToUi executes it inline and libobs marks the right thread.
	//
	// Never cleared. obs_set_ui_task_handler(nullptr) would immediately queue that
	// same task into the hole it just made and log the error this fix exists to
	// remove; instead AsyncTask's alive-guard, cleared in Bridge::Shutdown before
	// obs_shutdown and long before CefShutdown, makes every late task a silent drop.
	obs_set_ui_task_handler(UiTaskHandler);

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

	// Nothing has ever published the nit levels, so from the moment graphics exists
	// obs_get_video_sdr_white_level() reports the zero obs_startup left behind rather
	// than its documented 300 fallback, and every SDR<->HDR composite divides by it.
	// The canvas store loads much later (LoadMultistreamModel below), so seed from
	// CanvasColorDef's own initializers -- restating 300/1000 here would be a second
	// copy of the default free to drift from the persisted one -- and let the boot
	// reconcile republish the user's values. No window is left at zero.
	ApplyGlobalVideoLevels(CanvasColorDef{});

	// Re-apply the seeded DEBUG gate now that obs exists: the boot seed above ran
	// through Log::SetDebug before obs_startup, when obs_set_render_debug no-ops.
	obs_set_render_debug(renderDebug);
	obs_set_render_gpu_debug(renderGpuDebug);

	// Loaded here rather than with the other settings below, because the mix reset on the
	// next line needs it: libobs persists no part of the audio mix, so reading the store
	// first is the only thing that makes a 44.1 kHz or 5.1 choice survive a launch. The
	// process-priority, ducking and monitoring-device applies still run further down, where
	// the subsystems they touch exist.
	g_advanced.Load();

	obs_audio_info oai = {};
	oai.samples_per_sec = Audio::SampleRateSupported(g_advanced.audioSampleRate) ? g_advanced.audioSampleRate
										     : 48000;
	// Left at stereo by a name this build does not know: SpeakerLayoutFromName only
	// writes through on a match.
	oai.speakers = SPEAKERS_STEREO;
	Audio::SpeakerLayoutFromName(g_advanced.audioSpeakers, oai.speakers);
	if (!obs_reset_audio(&oai)) {
		HostLog("[obs] obs_reset_audio failed");
		return false;
	}
	HostLog("[obs] obs_reset_audio ok (" + std::to_string(oai.samples_per_sec) + "Hz " +
		Audio::SpeakerLayoutName(oai.speakers) + ")");

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
	// Before any preview surface exists, so the first frame draws the stored overlays.
	Preview::LoadOverlays(g_general);
	HostLog("[obs] general settings loaded");

	// Already loaded above, before the audio mix reset that reads it. Apply the stored
	// process priority once here; the engine reads the rest (stream delay / reconnect /
	// network) per output at StartOutput, and browserHwAccel is handed to obs-browser below.
	// Nothing can be live at startup, and g_multistream is not constructed yet, so
	// resolve "auto" against an idle state (false) rather than calling AnyLive().
	ApplyEffectivePriority(g_advanced.processPriority, false);
	DisableAudioDucking(g_advanced.disableAudioDucking);
	// Before the scene collection loads below: a source with monitoring enabled builds
	// its monitor against whatever device id is current at creation time, so setting it
	// here saves every restored source a reset it would otherwise need.
	ApplyAudioMonitoringDevice(g_advanced.audioMonitoringDeviceName, g_advanced.audioMonitoringDeviceId);
	HostLog("[obs] advanced settings loaded; process priority=" + g_advanced.processPriority +
		"; audio ducking disabled=" + std::string(g_advanced.disableAudioDucking ? "true" : "false"));

	// obs-browser has no settings file of its own: it reads BrowserHWAccel out of
	// libobs' private-data bag once, in its obs_module_load, and a false there pins
	// every browser source to the CPU OnPaint readback for the process lifetime. So
	// this must precede LoadCuratedModules() below, and a change to the setting only
	// takes effect on the next launch (which is what its UI hint says).
	//
	// Acceleration puts every browser source on CEF's GPU shared-texture path, which
	// is what makes it worth having and also what can CHECK()-fail on CrBrowserMain
	// and freeze the UI; BrowserHwAccel::DecideAtBoot arms the crash probe that turns
	// the setting off on the next launch when that happens. It is pointless -- and
	// leaves the sources black -- when the browser process runs with --disable-gpu,
	// since obs-browser derives shared-texture availability from the OBS D3D11 device
	// and never sees that switch.
	const bool hwAccelStored = g_advanced.browserHwAccel;
	const bool softwareMode = GpuSafeMode::SoftwareRendering();
	const BrowserHwAccel::BootDecision hwAccel = BrowserHwAccel::DecideAtBoot(hwAccelStored, softwareMode);
	// obs_set_private_data copies into libobs' own bag rather than taking the
	// reference, so the local one is ours to release.
	{
		OBSDataAutoRelease privateData = obs_data_create();
		obs_data_set_bool(privateData, "BrowserHWAccel", hwAccel.enable);
		obs_set_private_data(privateData);
	}
	if (hwAccel.crashDetected) {
		// The stored setting IS the latch: turning it off is what makes the
		// suppression stick across boots, and it keeps the Settings checkbox an
		// honest picture of what is running -- re-ticking it re-arms the probe. The
		// probe's sentinel is still on disk and only retires once that has landed, so
		// a refused write costs a repeat of this boot rather than the crash loop.
		g_advanced.browserHwAccel = false;
		if (g_advanced.Save()) {
			BrowserHwAccel::ConfirmSuppressionPersisted();
			blog(LOG_WARNING,
			     "[gpu] browser hardware acceleration requested=true applied=false -- the previous "
			     "launch brought browser sources up on the GPU and its CEF UI thread never came "
			     "back. The setting has been turned off; re-enable it in Settings > Advanced to "
			     "retry.");
		} else {
			blog(LOG_WARNING,
			     "[gpu] browser hardware acceleration suppressed for this launch, but the setting "
			     "could not be persisted, so the suppression is not sticky yet. It stays off this "
			     "run and the next launch will detect the same crash and try again.");
		}
	} else {
		HostLog("[gpu] browser hardware acceleration requested=" +
			std::string(hwAccelStored ? "true" : "false") +
			" applied=" + std::string(hwAccel.enable ? "true" : "false") +
			(hwAccelStored && softwareMode ? " (this launch renders CEF in software mode)" : ""));
	}

	// obs-browser's braidcast_overlay source type resolves its URL through these, and
	// a source can be created the moment the module registers the type, so they must
	// be on the handler before the load below.
	Overlay::RegisterProcs();

	// Phase 9.3: bring up the overlay-widget loopback server (127.0.0.1); stopped in
	// Bridge::Shutdown before CEF teardown (alongside the chat/events transports). The
	// bind result feeds the transport-health surface (Connected when listening, Failed
	// when no port in range binds).
	//
	// This must precede anything that can create an overlay source -- the scene
	// collection below is the first -- because a source that resolves no URL loads a
	// blank page and then has to be handed the real one, and each of those swaps
	// destroys and respawns a CEF browser during boot. Start() only reads the store's
	// persisted port, so it has no ordering debt to the model, scenes or providers
	// (which push into the server, and none of which it reads).
	const bool overlayUp = Overlay::Server().Start();
	Transports::Health().Report(Transports::kOverlayTransportId,
				    overlayUp ? Transports::TransportHealth::State::Connected
					      : Transports::TransportHealth::State::Failed,
				    overlayUp ? "" : Overlay::Server().LastError());

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
	// History opens with the other stores, and recovery runs before anything can
	// read: a session that never ended must already say so by the time the UI asks.
	// A failure here is logged and the app continues -- history degrades to
	// unavailable, streaming does not.
	const std::string historyPath = MultistreamBasicPath("history.db");
	// MultistreamBasicPath only joins strings. SaveJsonAtomic is what creates the
	// directory for the JSON stores, and nothing on this path goes through it, so a
	// first run with no basic/ yet would fail to open.
	os_mkdirs(std::filesystem::path(historyPath).parent_path().u8string().c_str());
	if (!g_historyDb.Open(historyPath)) {
		HostLog("[history] database unavailable: " + g_historyDb.LastError());
	} else if (!g_sessions.Attach(historyPath) || !g_recorder.Attach(historyPath) ||
		   !g_schedule.Attach(historyPath)) {
		HostLog("[history] store unavailable: " + g_sessions.LastError());
	} else {
		const int recovered = g_sessions.RecoverCrashed();
		if (recovered > 0) {
			HostLog("[history] recovered " + std::to_string(recovered) +
				" session(s) that ended without a clean stop");
		}
		// `armed` and `live` are what a running process was doing, and this
		// process has not started doing anything yet. Without this a stale
		// armed row would go live the moment the app opens.
		const int adopted = g_schedule.RecoverInterrupted();
		if (adopted > 0) {
			HostLog("[schedule] recovered " + std::to_string(adopted) +
				" entr(ies) left mid-flight by the previous run");
		}
		// Entries whose time passed while the app was closed. Without this the
		// calendar reopens still showing them as upcoming, which is a claim
		// about the present that stopped being true days ago. The runner's own
		// grace applies here too: launching thirty seconds late must not settle
		// an entry a running app would still have started.
		const int missed = g_schedule.SweepMissed(TimeUtil::NowMs() - History::kMissedGraceMs);
		if (missed > 0) {
			HostLog("[schedule] marked " + std::to_string(missed) +
				" entr(ies) missed while the app was closed");
		}
	}

	g_scheduledSetup.log = [](const std::string &line) {
		HostLog(line);
	};
	// Gates Apply and Revert, so a prelude counts: Apply is what rewrites the routing
	// a prelude already snapshotted.
	g_scheduledSetup.isStreaming = [] {
		return StreamingOrGoingLive();
	};
	g_scheduledSetup.routing.read = [] {
		std::vector<History::RoutingBinding> out;
		for (const OutputBinding &b : g_outputBindings.Bindings().bindings) {
			out.push_back({b.uuid, b.profileUuid, b.enabled});
		}
		return out;
	};
	g_scheduledSetup.routing.write = [](const std::string &uuid, bool enabled) {
		return SetBindingEnabled(uuid, enabled);
	};
	g_scheduledSetup.metadata.read = [](const std::string &profileId) {
		return g_streamMeta.StreamOverride(profileId);
	};
	g_scheduledSetup.metadata.write = [](const std::string &profileId, const Bridge::json &fields) {
		g_streamMeta.PutStreamOverride(profileId, fields);
	};
	g_scheduledSetup.metadata.clear = [](const std::string &profileId) {
		g_streamMeta.RemoveStreamOverride(profileId);
	};
	g_scheduledSetup.metadata.save = [] {
		g_streamMeta.Save();
	};

	// The runner's outside world, injected so the state machine itself stays
	// testable against a fake clock. Wired unconditionally: with no database the
	// store is unattached and every pass is a no-op.
	g_scheduleRunner.Attach(&g_schedule);
	g_scheduleRunner.log = [](const std::string &line) {
		HostLog(line);
	};
	g_scheduleRunner.onChanged = [] {
		Bridge::EmitEvent(EventNames::kScheduleChanged, Bridge::json::object());
	};
	g_scheduleRunner.goLive = [] {
		Bridge::StartStreamingAll();
	};
	// Reached through RoutingHeldElsewhere, so a prelude counts here too: this is what
	// makes StartIfDue and StartNow refuse rather than let StartStreamingAll swallow a
	// start whose occurrence flag and routing have already been rewritten.
	g_scheduleRunner.isStreaming = [] {
		return StreamingOrGoingLive();
	};
	g_scheduleRunner.applyEntry = [](const std::vector<History::ScheduleDestination> &destinations,
					 std::string &reason) {
		return g_scheduledSetup.Apply(destinations, reason);
	};
	g_scheduleRunner.revertEntry = [] {
		g_scheduledSetup.Revert();
	};
	g_scheduleRunner.canArm = [](const std::string &profileId, std::string &reason) {
		const StreamProfile *profile = g_streamProfiles.Find(profileId);
		if (!profile) {
			reason = "its stream profile was deleted";
			return false;
		}
		if (BindingForProfile(profileId, false).empty()) {
			reason = "'" + profile->DisplayName() + "' is not routed to any canvas";
			return false;
		}
		// The binding has to be ENABLED, not merely present, because a scheduled entry
		// never switches one on. Enabling a binding whose canvas has no other enabled
		// binding wakes that canvas and starts a whole extra encode -- so an entry
		// naming a destination the user had switched off would put the user's machine
		// under a load they had deliberately turned off, at a time they may not be
		// watching.
		if (BindingForProfile(profileId, true).empty()) {
			reason = "'" + profile->DisplayName() + "' is switched off";
			return false;
		}
		if (profile->accountId.empty()) {
			return true; // a stream-key / custom-RTMP / WHIP destination owns no account
		}
		const std::optional<OAuth::OAuthAccount> account = OAuth::Accounts().Get(profile->accountId);
		if (!account || !OAuth::IsAccountConnected(*account)) {
			reason = "'" + profile->DisplayName() + "' is not connected";
			return false;
		}
		return true;
	};
	g_scheduleRunner.requireAllDestinations = [] {
		return g_general.scheduleRequireAllDestinations;
	};

	// Feed the recorder off the one host-side sampler rather than sampling again:
	// the encode counters are rebased against shared mutable baselines, so a second
	// reader would split the deltas with the Stats dock. Registered unconditionally
	// -- with no database the recorder never records and this no-ops.
	Bridge::SetStatsTickObserver([](const Bridge::json &snapshot) {
		// The schedule runner rides this one tick rather than owning a timer of its
		// own, so it must sit above the recorder's guard -- entries arm and go live
		// while nothing is being recorded, which is the whole point of it.
		g_scheduleRunner.Tick();
		if (!g_recorder.IsRecording()) {
			return;
		}
		g_recorder.OnSample(SampleFromSnapshot(snapshot));
		g_thumbs.OnTick(TimeUtil::NowMs() - g_recorder.StartedAtMs(), g_recorder.CurrentId());
	});

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

	// A target granted after its account was connected (a Page added to the person's
	// Facebook account since) has no destination until something re-runs the reconcile,
	// and only the connect flow does -- so the user had to disconnect and reconnect to
	// see it. Run it once here for the accounts already connected; the pass itself hands
	// the blocking enumeration to a worker, so this call does not delay boot.
	MaterializeTargetDestinationsAtBoot();

	// Which target each destination claims, mirrored down to the providers before the
	// audience poller starts reading them. Synchronous and network-free, unlike the
	// reconcile above -- a provider must not have to wait on a platform enumeration, or on
	// the user re-opening Go Live, to learn where an already-claimed destination points.
	PublishAllTargetClaims();

	// Channel identity: the audience-total poller is always-on (account-lifecycle,
	// not go-live-gated), so follower/subscriber totals refresh before/after
	// streaming. Stopped in Bridge::Shutdown alongside the other always-on workers.
	Chat::Channels().Start();

	// Boot reconcile: the global video pipeline was initialized to a fixed default
	// above (before modules could load), but the persisted Default canvas def is the
	// source of truth for its resolution/FPS -- the Settings UI edits the Default
	// canvas, which drives global video. Re-apply from the def so a saved resolution
	// survives restarts. Inert on first run (seeded def == the fixed init).
	{
		const CanvasDefinition &def = g_canvases.Default();
		// Outside the resolution comparison below: the nit levels reach libobs
		// through obs_set_video_levels, not obs_video_info, so a run whose
		// resolution already matches still has to republish them over the boot seed.
		ApplyGlobalVideoLevels(def.color);
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

	// Release the platform's per-destination live state as soon as THIS binding's output
	// ends, instead of waiting for the account-wide clear that only streaming.stop performs:
	// while a destination stays cached the viewer poller keeps spending one videos.list per
	// cycle on a broadcast that has ended, for the rest of the session. Only the ended
	// destination is cleared -- the account's sibling orientations are still live.
	// Marshaled because an unrequested stop reports from the libobs output thread while the
	// binding/profile stores are UI-thread-owned; the erase it performs is idempotent, which
	// is what makes a doubled report (deliberate stop + its stop signal) harmless.
	g_multistream->onOutputEnded = [](const std::string &bindingUuid) {
		AsyncTask::PostToUi([bindingUuid] {
			if (!MultistreamAlive()) {
				return;
			}
			const OutputBinding *b = OutputBindings().Bindings().Find(bindingUuid);
			if (!b) {
				return; // already removed; its destination went with the binding
			}
			OAuth::DestinationId dest;
			if (!Chat::BindingDestination(*b, dest)) {
				return;
			}
			const std::optional<OAuth::OAuthAccount> acct = OAuth::Accounts().Get(dest.accountId);
			if (!acct) {
				return;
			}
			if (OAuth::StreamProvider *provider = OAuth::Registry().Get(acct->providerId)) {
				provider->clearActiveBroadcastDestination(dest);
			}
			// This destination's chat ends with its output. Left running it would keep
			// reading (and, on YouTube, keep spending quota on) a chat the user is no
			// longer streaming to, and would eventually die on its own and report Failed
			// -- painting a red transport edge on the Multichat/Events chips for a
			// destination that was switched off deliberately. Per-destination rather than
			// a hub re-Start so the account's sibling orientations keep their transports.
			Bridge::FinishPolls(dest); // before the transport it ends the poll with stops
			Chat::Hub().StopDestination(dest);

			// How this destination finished, onto its session row. Idempotent
			// by contract: a deliberate stop whose stop signal also fires
			// reports the same ending twice.
			for (const MultistreamEngine::OutputStatus &st : g_multistream->Statuses()) {
				if (st.bindingUuid == bindingUuid) {
					g_recorder.OnDestinationEnded(
						bindingUuid, MultistreamEngine::StateName(st.state), st.lastError);
					break;
				}
			}
		});
	};

	// Re-pin the process priority at every live transition (the seam UpdateSleepInhibit
	// fires on). "auto" tracks live state -> HIGH live, ABOVE_NORMAL idle; a manual
	// override re-applies its fixed class. SyncProcessPriorityToLiveState marshals the
	// g_advanced read onto the UI thread since this fires off the libobs signal thread.
	g_multistream->onLiveStateChanged = [] {
		// Publish live-ness to the process-lifetime flag the detached account/event
		// pollers read: they may outlive the engine, so they must never dereference it.
		// Safe to call AnyLive() here -- UpdateSleepInhibit, the only firing site, calls
		// it itself before this, so liveMutex is provably not held.
		g_anyOutputLive.store(g_multistream->AnyLive(), std::memory_order_release);
		SyncProcessPriorityToLiveState();

		// Session edges ride the same transition. There is no "broadcast began"
		// callback -- this is the edge. Marshaled because this fires from the
		// libobs signal thread while the history database is UI-thread-only, and
		// the live state is re-read there rather than captured: several
		// transitions can coalesce behind one posted task, so the guard has to
		// test what is true when it runs.
		AsyncTask::PostToUi([] {
			if (!MultistreamAlive()) {
				return;
			}
			const bool live = g_multistream->AnyLive();
			if (live && !g_recorder.IsRecording()) {
				History::SessionStart start;
				start.startedAtMs = TimeUtil::NowMs();
				start.destinations = LiveDestinations(*g_multistream);
				start.canvasUuids = LiveCanvasUuids(*g_multistream);
				start.title = BuildSessionTitle(start.destinations);
				// Empty unless something explicitly asked for this
				// broadcast -- the auto-start, schedule.startNow, or a
				// manual go-live that adopted the armed entry on its way
				// through. A go-live that adopted nothing is an ordinary
				// unscheduled session, not a claim on a plan it never ran.
				start.scheduleId = g_scheduleRunner.ActiveEntryId();
				g_recorder.Begin(start);
				g_scheduleRunner.NoteWentLive();
				g_thumbs.Reset();
				Bridge::EmitEvent(EventNames::kSessionsChanged, Bridge::json::object());
			} else if (!live && g_recorder.IsRecording()) {
				EndSessionWithThumbnail(SessionEndReason(*g_multistream));
				g_scheduleRunner.NoteStoppedStreaming();
				// Unconditional, not only for the entry the runner was
				// tracking: nothing else is live now, so this is the one
				// moment restoring the routing cannot interrupt anything.
				// A no-op when nothing was applied.
				g_scheduledSetup.Revert();
				Bridge::EmitEvent(EventNames::kSessionsChanged, Bridge::json::object());
			}
		});
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
	MainChannel::Set(nullptr);

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
			HostLog("[selftest] " + method + " FAILED: " + Err::Diagnostic(error));
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
	const bool selOk = Preview::SelectFromBridge("", "", std::vector<SceneItemKey>{SceneItemKey(id)}).has_value();
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
	Preview::SelectFromBridge("", "", std::vector<SceneItemKey>{});
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
			HostLog("[selftest] " + method + " FAILED: " + Err::Diagnostic(error));
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

	// 5a) The applied mix must reach the store, not just libobs: nothing in libobs persists
	// the sample rate or the layout, so the boot's own obs_reset_audio took a 44.1 kHz or 5.1
	// choice back to 48 kHz stereo on the next launch. Step 5 could not see that either -- it
	// read the value back out of libobs, which is exactly where it was never the problem. The
	// boot half (reading the store before the reset) needs two launches and is not covered.
	if (ok) {
		const AdvancedSettings &stored = Advanced();
		const bool held = stored.audioSampleRate == 44100 && stored.audioSpeakers == "stereo";
		HostLog("[selftest] setAudio 44100 -> stored " + std::to_string(stored.audioSampleRate) + "Hz " +
			stored.audioSpeakers + (held ? " (PERSISTED)" : " (NOT PERSISTED)"));
	}

	// 5b) The monitoring device must survive a setAudio, both when the mix is untouched and
	// when it is reset. The settings pane sends sampleRate, speakers and monitoringDevice on
	// every apply, so a device change arrives carrying an unchanged rate -- and obs_reset_audio
	// re-seeds the device to Default, which made the control snap back the instant it was used.
	// Step 5 above could not see that: it only ever asserted the rate round-trips.
	json devices = run("audio.listMonitorDevices", json(nullptr), ok);
	if (ok && devices.is_array() && devices.size() > 1) {
		// [0] is always the synthetic "default" entry, which cannot show this defect: a reset
		// re-seeds to exactly that, so the wrong answer and the right one are the same value.
		const json &target = devices[1];
		const std::string wantedId = target.value("id", std::string());
		const auto deviceId = [](const json &audio) {
			const auto it = audio.find("monitoringDevice");
			return it == audio.end() ? std::string() : it->value("id", std::string());
		};

		const uint32_t heldRate = a1.value("sampleRate", 0u);
		const std::string heldSpeakers = a1.value("speakers", std::string("stereo"));

		// Unchanged rate + a device change: the exact shape the settings pane sends.
		json m1 = run("settings.setAudio",
			      json{{"sampleRate", heldRate}, {"speakers", heldSpeakers}, {"monitoringDevice", target}},
			      ok);
		HostLog("[selftest] setAudio monitoring device, mix unchanged -> " +
			(ok ? (deviceId(m1) == wantedId ? std::string("HELD") : "REVERTED to '" + deviceId(m1) + "'")
			    : std::string("call failed")));

		// And across a real mix reset, which tears the device down inside libobs.
		const uint32_t otherRate = heldRate == 48000 ? 44100u : 48000u;
		json m2 = run("settings.setAudio", json{{"sampleRate", otherRate}, {"speakers", heldSpeakers}}, ok);
		HostLog("[selftest] setAudio monitoring device, mix reset -> " +
			(ok ? (deviceId(m2) == wantedId ? std::string("HELD") : "LOST to '" + deviceId(m2) + "'")
			    : std::string("call failed")));
	} else {
		// Not a pass: this machine has no second monitoring endpoint to move to.
		HostLog("[selftest] setAudio monitoring device -> SKIPPED (no device besides Default)");
	}

	// 6) Restore the original audio config.
	run("settings.setAudio", a0, ok);
	HostLog("[selftest] audio restored to " + std::to_string(a0.value("sampleRate", 0u)) + "Hz");

	// 6a) The YouTube video snippet body. videos.update(part=snippet) deletes any property
	// that has a value and is left out, so the body is the video's own snippet with the edit
	// laid over it. Offline: these are the rules, not the network.
	{
		using YouTubeSnippet::Edit;
		using YouTubeSnippet::Merge;
		const json video = json{{"title", "old"},
					{"description", "old"},
					{"categoryId", "20"},
					{"tags", json::array({"keep"})},
					{"defaultLanguage", "ru"},
					{"defaultAudioLanguage", "ru"},
					{"publishedAt", "2026-07-31T00:00:00Z"},
					{"channelId", "UCx"}};
		Edit fallback;
		fallback.title = "new";
		fallback.description = "";
		fallback.language = "en";
		const json kept = Merge(video, fallback);
		Edit picked = fallback;
		picked.languageExplicit = true;
		const json overridden = Merge(video, picked);
		Edit clearTags = fallback;
		clearTags.tagsStated = true;
		const json cleared = Merge(video, clearTags);
		const json blank = Merge(json::object(), fallback);
		const json notApplicable = Merge(json{{"categoryId", "20"}, {"defaultAudioLanguage", "zxx"}}, fallback);

		const bool ok6a =
			// A fallback fills, never overrides a language the video already states.
			kept["defaultLanguage"] == "ru" && kept["defaultAudioLanguage"] == "ru" &&
			// An explicit pick does override it.
			overridden["defaultLanguage"] == "en" && overridden["defaultAudioLanguage"] == "en" &&
			// What the edit does not state survives; what it states wins.
			kept["categoryId"] == "20" && kept["tags"] == json::array({"keep"}) && kept["title"] == "new" &&
			kept["description"] == "" &&
			// A stated empty list is a clear, not an omission.
			cleared["tags"] == json::array() &&
			// Read-only properties are not echoed back into the write.
			!kept.contains("publishedAt") && !kept.contains("channelId") &&
			// A video with nothing still gets the required category, and the fallback lands.
			blank["categoryId"] == "24" && blank["defaultLanguage"] == "en" && !blank.contains("tags") &&
			// "zxx" is readable but not writable: dropped, then filled.
			notApplicable["defaultAudioLanguage"] == "en" &&
			YouTubeSnippet::LanguageFromLocale("en-US") == "en" &&
			YouTubeSnippet::LanguageFromLocale("ru-RU") == "ru" &&
			YouTubeSnippet::LanguageFromLocale("zh-Hant-TW") == "zh-Hant" &&
			YouTubeSnippet::LanguageFromLocale("zh-TW") == "zh-Hant" &&
			YouTubeSnippet::LanguageFromLocale("zh-HK") == "zh-Hant" &&
			YouTubeSnippet::LanguageFromLocale("zh-CN") == "zh-Hans" &&
			YouTubeSnippet::LanguageFromLocale("zh-Hans-SG") == "zh-Hans" &&
			YouTubeSnippet::LanguageFromLocale("zh-Hans-HK") == "zh-Hans" &&
			YouTubeSnippet::LanguageFromLocale("zh_TW") == "zh-Hant" &&
			YouTubeSnippet::LanguageFromLocale("fil-PH") == "fil" &&
			YouTubeSnippet::LanguageFromLocale("x-foo").empty() &&
			YouTubeSnippet::LanguageFromLocale("qps-ploc").empty() &&
			YouTubeSnippet::LanguageFromLocale("").empty();
		HostLog(std::string("[selftest] youtube-snippet merge -> ") + (ok6a ? "OK" : "MISMATCH"));
		if (!ok6a) {
			HostLog("[selftest] youtube-snippet kept=" + kept.dump() + " overridden=" + overridden.dump() +
				" blank=" + blank.dump() + " zxx=" + notApplicable.dump());
		}
	}

	// 6b) YouTube live polls, offline: the insert body carries the poll in the order given,
	// and a liveChatMessage reads back into the neutral shape whatever form the tallies take.
	{
		const json two = YouTubePoll::BuildInsertBody("chat-1", "Best map?", {"A", "B"});
		const json four = YouTubePoll::BuildInsertBody("chat-1", "Q", {"one", "two", "three", "four"});
		const json &snippet = two["snippet"];
		const json &twoOptions = snippet["pollDetails"]["metadata"]["options"];
		const json &fourOptions = four["snippet"]["pollDetails"]["metadata"]["options"];
		const auto message = [](const json &options, const char *status) {
			json metadata = json{{"questionText", "Best map?"}, {"options", options}};
			if (status) {
				metadata["status"] = status;
			}
			return json{{"id", "poll-1"}, {"snippet", json{{"pollDetails", json{{"metadata", metadata}}}}}};
		};
		const json numeric =
			YouTubePoll::Normalize(message(json::array({json{{"optionText", "A"}, {"tally", 3}},
								    json{{"optionText", "B"}, {"tally", 0}}}),
						       "closed"),
					       "active");
		const json stringy =
			YouTubePoll::Normalize(message(json::array({json{{"optionText", "A"}, {"tally", "12"}},
								    json{{"optionText", "B"}, {"tally", "x"}}}),
						       "active"),
					       "closed");
		const json untallied =
			YouTubePoll::Normalize(message(json::array({json{{"optionText", "A"}}}), nullptr), "active");
		const json unknownStatus = YouTubePoll::Normalize(message(json::array(), "unknown"), "closed");
		const json empty = YouTubePoll::Normalize(json::object(), "active");
		// The InnerTube live result, in the shape a real poll delivered (2026-09-22).
		const json emojiRun = json{{"emoji", json{{"emojiId", "\xF0\x9F\x94\xA5"}, {"shortcuts", {":fire:"}}}}};
		const json live = YouTubePoll::FromInnerTube(json{
			{"choices",
			 json::array({json{{"text", json{{"runs", json::array({json{{"text", "Awesome "}}, emojiRun})}}},
					   {"voteRatio", 0.7142857313156128}},
				      json{{"text", json{{"runs", json::array({json{{"text", "Lagging"}}})}}},
					   {"voteRatio", 0}},
				      json{{"text", json{{"simpleText", "Odd"}}}, {"voteRatio", 1.5}}})},
			{"header",
			 json{{"pollHeaderRenderer",
			       json{{"metadataText",
				     json{{"runs", json::array({json{{"text", "Poll \xC2\xB7 1,234 votes"}}})}}}}}}}});
		const json liveNoHeader = YouTubePoll::FromInnerTube(json{{"choices", json::array()}});

		const bool okPoll =
			snippet["type"] == "pollEvent" && snippet["liveChatId"] == "chat-1" &&
			snippet["pollDetails"]["metadata"]["questionText"] == "Best map?" && twoOptions.size() == 2 &&
			twoOptions[0]["optionText"] == "A" && twoOptions[1]["optionText"] == "B" &&
			fourOptions.size() == 4 && fourOptions[0]["optionText"] == "one" &&
			fourOptions[3]["optionText"] == "four" && numeric["id"] == "poll-1" &&
			numeric["question"] == "Best map?" && numeric["status"] == "closed" &&
			numeric["options"][0]["text"] == "A" && numeric["options"][0]["tally"] == 3 &&
			numeric["options"][1]["tally"] == 0 && stringy["status"] == "active" &&
			stringy["options"][0]["tally"] == 12 && stringy["options"][1]["tally"].is_null() &&
			untallied["options"][0]["tally"].is_null() && untallied["status"] == "active" &&
			unknownStatus["status"] == "closed" && empty["id"] == "" && empty["options"].empty() &&
			empty["status"] == "active" && live["options"].size() == 3 &&
			live["options"][0]["text"] == "Awesome \xF0\x9F\x94\xA5" &&
			live["options"][0]["ratio"].get<double>() > 0.71 && live["options"][1]["ratio"] == 0 &&
			live["options"][2]["ratio"].is_null() && live["options"][2]["text"] == "Odd" &&
			live["totalVotes"] == 1234 && liveNoHeader["totalVotes"].is_null() &&
			liveNoHeader["options"].empty();
		HostLog(std::string("[selftest] youtube-poll -> ") + (okPoll ? "OK" : "MISMATCH"));
		if (!okPoll) {
			HostLog("[selftest] youtube-poll body=" + two.dump() + " numeric=" + numeric.dump() +
				" stringy=" + stringy.dump() + " untallied=" + untallied.dump() +
				" live=" + live.dump());
		}
	}

	// 6b2) InnerTube chat items, offline: a Super Chat's line carries its amount and tier colour
	// (with or without a comment), a milestone reads its months, and a gift redemption is a chat
	// line that raises no event of its own -- the purchase already raised the one alert.
	{
		using Chat::YouTubeInnerTube::DecodeChatItem;
		using Chat::YouTubeInnerTube::DecodedItem;
		const auto runs = [](std::initializer_list<const char *> texts) {
			json out = json::array();
			for (const char *t : texts) {
				out.push_back(json{{"text", t}});
			}
			return json{{"runs", out}};
		};
		const auto paidMessage = [&](const char *id, const json &message) {
			json r = json{{"id", id},
				      {"timestampUsec", "1700000000000000"},
				      {"authorName", json{{"simpleText", "Ann"}}},
				      {"authorExternalChannelId", "UCann"},
				      {"purchaseAmountText", json{{"simpleText", "$5.00"}}},
				      {"headerBackgroundColor", 4280191205LL}};
			if (!message.is_null()) {
				r["message"] = message;
			}
			return json{{"liveChatPaidMessageRenderer", r}};
		};
		const auto membership = [&](const char *id, const json &primary, const json &subtext) {
			json r = json{{"id", id},
				      {"authorName", json{{"simpleText", "Bo"}}},
				      {"headerSubtext", subtext}};
			if (!primary.is_null()) {
				r["headerPrimaryText"] = primary;
				r["message"] = runs({"a year!"});
			}
			return json{{"liveChatMembershipItemRenderer", r}};
		};

		DecodedItem sc, bare, sticker, tinted, noAmount, milestone, welcome, redeemed, unknown;
		const bool decoded =
			DecodeChatItem(paidMessage("sc-1", runs({"hi"})), sc) &&
			DecodeChatItem(paidMessage("sc-2", json()), bare) &&
			DecodeChatItem(
				json{{"liveChatPaidStickerRenderer",
				      json{{"id", "st-1"},
					   {"purchaseAmountText", json{{"simpleText", "\xE2\x82\xB9"
										      "450.00"}}},
					   {"sticker",
					    json{{"thumbnails", json::array({json{{"url", "//s.example/a.png"}}})}}}}}},
				sticker) &&
			// The chip colour falls back to backgroundColor, and a colour serialized as a
			// signed 32-bit int (0xFF1E88E5 == -14776091) still reads as its RGB.
			DecodeChatItem(json{{"liveChatPaidStickerRenderer",
					     json{{"id", "st-2"},
						  {"purchaseAmountText", json{{"simpleText", "$2.00"}}},
						  {"backgroundColor", -14776091}}}},
				       tinted) &&
			DecodeChatItem(json{{"liveChatPaidMessageRenderer",
					     json{{"id", "sc-3"}, {"message", runs({"no amount"})}}}},
				       noAmount) &&
			DecodeChatItem(membership("m-1", runs({"Member for ", "12", " months"}),
						  json{{"simpleText", "Gold"}}),
				       milestone) &&
			DecodeChatItem(membership("m-2", json(), runs({"Welcome to ", "Gold", "!"})), welcome) &&
			DecodeChatItem(
				json{{"liveChatSponsorshipsGiftRedemptionAnnouncementRenderer",
				      json{{"id", "g-1"}, {"message", runs({"Cy was gifted a membership by Ann"})}}}},
				redeemed) &&
			!DecodeChatItem(json{{"liveChatViewerEngagementMessageRenderer", json{{"id", "x"}}}}, unknown);

		const bool okItems =
			decoded && sc.fragments.size() == 1 && sc.paid["kind"] == "superchat" &&
			sc.paid["amount"] == "$5.00" && sc.paid["color"] == "#1E88E5" && sc.hasEvent &&
			sc.ev.type == "superchat" && sc.ev.amount == 500 && sc.ev.currency == "USD" &&
			sc.ev.message == "hi" && sc.tsMs == 1700000000000LL && bare.fragments.empty() &&
			bare.paid["amount"] == "$5.00" && bare.hasEvent && sticker.paid["kind"] == "supersticker" &&
			!sticker.paid.contains("color") && sticker.fragments.size() == 1 &&
			sticker.fragments[0]["url"] == "https://s.example/a.png" && sticker.ev.currency == "INR" &&
			milestone.hasEvent && milestone.ev.months == 12 && milestone.ev.tier == "Gold" &&
			milestone.ev.message == "a year!" && welcome.hasEvent && welcome.ev.months == 0 &&
			welcome.ev.tier == "Gold" && welcome.paid.is_null() && !redeemed.hasEvent &&
			redeemed.fragments.size() == 1 && tinted.paid["color"] == "#1E88E5" &&
			tinted.fragments.empty() && noAmount.paid.is_null() && noAmount.fragments.size() == 1;
		HostLog(std::string("[selftest] youtube-chat-items -> ") + (okItems ? "OK" : "MISMATCH"));
		if (!okItems) {
			HostLog("[selftest] youtube-chat-items decoded=" + std::string(decoded ? "1" : "0") +
				" sc.paid=" + sc.paid.dump() + " sc.frags=" + sc.fragments.dump() +
				" bare.frags=" + bare.fragments.dump() + " sticker.paid=" + sticker.paid.dump() +
				" tinted.paid=" + tinted.paid.dump() + " noAmount.paid=" + noAmount.paid.dump() +
				" milestone.months=" + std::to_string(milestone.ev.months) +
				" tier=" + milestone.ev.tier + " welcome.months=" + std::to_string(welcome.ev.months) +
				" redeemed.event=" + std::string(redeemed.hasEvent ? "1" : "0"));
		}
	}

	// 6b3) Twitch IRC chat lines, offline: a USERNOTICE renders Twitch's own system-msg (plus
	// the chatter's words, emotes resolved, when there are any), a shared-chat mirror of another
	// channel's notice renders nothing, a cheer's line carries its bits as `paid`, a /me line
	// places its emotes inside the ACTION wrapper, and tag values unescape per IRCv3.
	{
		const Chat::ThirdPartyEmoteMap noEmotes;
		const auto line = [&](const char *raw) {
			return OAuth::NormalizeTwitchChatLine(raw, "dallas", noEmotes);
		};
		const json resub = line(
			"@badge-info=subscriber/12;badges=subscriber/12,premium/1;color=#008000;display-name=ronni;"
			"emotes=25:13-17;flags=;id=db25007f-7a18-43eb-9379-80131e44d633;login=ronni;mod=0;msg-id=resub;"
			"msg-param-cumulative-months=12;msg-param-months=0;msg-param-should-share-streak=1;"
			"msg-param-streak-months=12;msg-param-sub-plan-name=Channel\\sSubscription\\s(dallas);"
			"msg-param-sub-plan=Prime;room-id=12345678;subscriber=1;"
			"system-msg=ronni\\ssubscribed\\swith\\sPrime.\\sThey've\\ssubscribed\\sfor\\s12\\smonths!;"
			"tmi-sent-ts=1507246572675;user-id=87654321;user-type= :tmi.twitch.tv USERNOTICE #dallas "
			":Great stream Kappa");
		const json subgift = line(
			"@badge-info=;badges=staff/1,premium/1;color=#0000FF;display-name=TWW2;emotes=;flags=;"
			"id=e9176cd8-5e22-4684-ad40-ce53c2561c5e;login=tww2;mod=0;msg-id=subgift;msg-param-months=1;"
			"msg-param-recipient-display-name=Mr_Woodchuck;msg-param-recipient-id=55554444;"
			"msg-param-recipient-user-name=mr_woodchuck;msg-param-sub-plan-name=House\\sof\\sNyoro~n;"
			"msg-param-sub-plan=1000;room-id=19571752;subscriber=0;"
			"system-msg=TWW2\\sgifted\\sa\\sTier\\s1\\ssub\\sto\\sMr_Woodchuck!;tmi-sent-ts=1521159445153;"
			"turbo=0;user-id=87654321;user-type=staff :tmi.twitch.tv USERNOTICE #dallas");
		const json shared =
			line("@badge-info=;badges=;color=#1E90FF;display-name=Other;emotes=;flags=;"
			     "id=0d6a4d77-7e2c-4d5b-9b1f-3c7f2b9d6e11;login=other;mod=0;msg-id=sharedchatnotice;"
			     "msg-param-sub-plan=1000;room-id=12345678;source-badge-info=;source-badges=;"
			     "source-id=8b0c1e2a-51f4-4a8e-9d2a-6f3b7c9e0a44;source-msg-id=sub;source-room-id=22222222;"
			     "subscriber=0;system-msg=Other\\ssubscribed\\sat\\sTier\\s1.;tmi-sent-ts=1507246572675;"
			     "user-id=33333333;user-type= :tmi.twitch.tv USERNOTICE #dallas");
		const json cheer =
			line("@badge-info=;badges=bits/100;bits=100;color=#FF0000;display-name=ronni;emotes=;"
			     "id=b34ccfc7-4977-403a-8a94-33c6bac34fb8;mod=0;room-id=12345678;subscriber=0;"
			     "tmi-sent-ts=1507246572675;turbo=1;user-id=12345678;user-type= "
			     ":ronni!ronni@ronni.tmi.twitch.tv PRIVMSG #dallas :Cheer100 Nice!");
		const json oneBit = line("@bits=1;display-name=ronni;id=c1;tmi-sent-ts=1507246572675;user-id=12345678 "
					 ":ronni!ronni@ronni.tmi.twitch.tv PRIVMSG #dallas :Cheer1");
		const json plain =
			line("@badge-info=;badges=broadcaster/1;color=;display-name=;emotes=;first-msg=0;flags=;"
			     "id=885196de-cb67-427a-baa8-82f9b0fcd05f;mod=0;room-id=713936733;subscriber=0;"
			     "tmi-sent-ts=1643904084794;turbo=0;user-id=713936733;user-type= "
			     ":foo!foo@foo.tmi.twitch.tv PRIVMSG #dallas :bleedPurple");
		// Twitch counts a /me line's emote offsets from the text inside the CTCP ACTION wrapper.
		const json action = line("@badge-info=;badges=;color=#8A2BE2;display-name=ronni;emotes=25:6-10;"
					 "id=2f1f6b0e-6c2d-4a51-9d4e-0b8f3c7a9e15;mod=0;room-id=12345678;subscriber=0;"
					 "tmi-sent-ts=1507246572675;user-id=12345678;user-type= "
					 ":ronni!ronni@ronni.tmi.twitch.tv PRIVMSG #dallas :\x01"
					 "ACTION waves Kappa\x01");
		// An announcement's text is the chatter's own; its system-msg is empty, and an unparsable
		// tmi-sent-ts falls back to now rather than to the epoch.
		const json announcement =
			line("@badge-info=;badges=broadcaster/1;color=#033700;display-name=Dallas;emotes=;flags=;"
			     "id=f4a0c5d9-2f5a-4c47-9c40-2e3f0a6b1d22;login=dallas;mod=0;msg-id=announcement;"
			     "msg-param-color=PRIMARY;room-id=12345678;subscriber=0;system-msg=;tmi-sent-ts=soon;"
			     "user-id=12345678;user-type= :tmi.twitch.tv USERNOTICE #dallas :Hello everyone");
		// Tag-value escapes: `\:` is ';', `\\` is '\', and a lone trailing '\' is dropped.
		const json escaped =
			line("@display-name=Dallas;id=e1;login=dallas;msg-id=viewermilestone;"
			     "system-msg=Tip\\:\\sa\\\\b\\sdone\\;tmi-sent-ts=1507246572675;user-id=12345678 "
			     ":tmi.twitch.tv USERNOTICE #dallas");
		const json blank =
			line("@display-name=Dallas;id=e2;login=dallas;msg-id=viewermilestone;system-msg=\\s\\s;"
			     "tmi-sent-ts=1507246572675;user-id=12345678 :tmi.twitch.tv USERNOTICE #dallas");

		// Reads a nested key without inserting it, so a failed check leaves the dump as produced.
		const auto at = [](const json &j, std::initializer_list<const char *> path) -> json {
			const json *cur = &j;
			for (const char *key : path) {
				if (!cur->is_object()) {
					return json();
				}
				const auto it = cur->find(key);
				if (it == cur->end()) {
					return json();
				}
				cur = &*it;
			}
			return *cur;
		};
		const auto text = [](const char *t) {
			return json{{"type", "text"}, {"text", t}};
		};
		const auto kappa = json{{"type", "emote"},
					{"code", "Kappa"},
					{"url", "https://static-cdn.jtvnw.net/emoticons/v2/25/default/dark/1.0"}};

		const json resubFrags =
			json::array({text("ronni subscribed with Prime. They've subscribed for 12 months!"), text(" "),
				     text("Great stream "), kappa});
		const json resubBadges = json::array({json{{"kind", "subscriber"}}, json{{"kind", "premium"}}});
		const json announcementTs = at(announcement, {"ts"});
		const bool okTwitch =
			at(resub, {"event"}) == EventNames::kChatMessage && at(resub, {"platform"}) == "twitch" &&
			at(resub, {"channelId"}) == "dallas" &&
			at(resub, {"id"}) == "db25007f-7a18-43eb-9379-80131e44d633" &&
			at(resub, {"ts"}) == 1507246572675LL && at(resub, {"author", "name"}) == "ronni" &&
			at(resub, {"author", "id"}) == "87654321" && at(resub, {"author", "color"}) == "#008000" &&
			at(resub, {"author", "badges"}) == resubBadges && at(resub, {"fragments"}) == resubFrags &&
			resub.is_object() && !resub.contains("paid") && at(subgift, {"author", "name"}) == "TWW2" &&
			at(subgift, {"ts"}) == 1521159445153LL &&
			at(subgift, {"fragments"}) ==
				json::array({text("TWW2 gifted a Tier 1 sub to Mr_Woodchuck!")}) &&
			subgift.is_object() && !subgift.contains("paid") && shared.is_null() &&
			at(cheer, {"paid"}) == json{{"kind", "cheer"}, {"amount", "100 bits"}} &&
			at(cheer, {"fragments"}) == json::array({text("Cheer100 Nice!")}) &&
			at(oneBit, {"paid", "amount"}) == "1 bit" && plain.is_object() && !plain.contains("paid") &&
			at(plain, {"author", "name"}) == "foo" && at(plain, {"author", "color"}) == "" &&
			at(plain, {"fragments"}) == json::array({text("bleedPurple")}) &&
			at(action, {"fragments"}) == json::array({text("waves "), kappa}) &&
			at(announcement, {"fragments"}) == json::array({text("Hello everyone")}) &&
			at(announcement, {"author", "name"}) == "Dallas" && announcementTs.is_number_integer() &&
			announcementTs.get<int64_t>() > 1507246572675LL &&
			at(escaped, {"fragments"}) == json::array({text("Tip; a\\b done")}) && blank.is_null();
		HostLog(std::string("[selftest] twitch-chat-lines -> ") + (okTwitch ? "OK" : "MISMATCH"));
		if (!okTwitch) {
			HostLog("[selftest] twitch-chat-lines resub=" + resub.dump() + " subgift=" + subgift.dump() +
				" shared=" + shared.dump() + " cheer=" + cheer.dump() + " oneBit=" + oneBit.dump() +
				" plain=" + plain.dump() + " action=" + action.dump() + " announcement=" +
				announcement.dump() + " escaped=" + escaped.dump() + " blank=" + blank.dump());
		}
	}

	// 6b4) Kick Kicks gifts, offline: a KicksGifted frame normalizes to a `kicks` event whether
	// Pusher's `data` arrives as an object or double-encoded as a string, the sender colour
	// reads from either payload shape and is dropped when malformed, and the channel.<id> and
	// channel_<id> deliveries of one gift normalize alike. KickDeliveryDedupe, not the id, is
	// what drops that twin: without a gift_transaction_id the two ids differ by receipt time.
	{
		const json payload = json{
			{"gift_transaction_id", "01J9KICKS8F2C"},
			{"message", "w"},
			{"sender", json{{"id", 4815162}, {"username", "gifter"}, {"username_color", "#FF9D00"}}},
			{"gift", json{{"gift_id", "rage_quit"},
				      {"name", "Rage Quit"},
				      {"amount", 500},
				      {"type", "LEVEL_UP"},
				      {"tier", "MID"},
				      {"character_limit", 100},
				      {"pinned_time", 600}}},
		};
		const auto frame = [](const char *channel, const json &data) {
			return json{{"event", "KicksGifted"}, {"channel", channel}, {"data", data}};
		};
		const auto with = [&](const char *key, const json &value) {
			json p = payload;
			p[key] = value;
			return p;
		};
		const json identitySender = json{{"username", "gifter"}, {"identity", json{{"color", "#00aaff"}}}};
		const json plainGift = json{{"name", "Hype"}, {"amount", 1}};

		Events::NormalizedEvent dot, underscore, identity, badColor, noTxn, noAmount, chat;
		const bool normalized =
			Events::NormalizeKickEvent("KicksGifted", frame("channel.1234", payload), dot) &&
			Events::NormalizeKickEvent("KicksGifted", frame("channel_1234", payload.dump()), underscore) &&
			Events::NormalizeKickEvent("KicksGifted", frame("channel.1234", with("sender", identitySender)),
						   identity) &&
			Events::NormalizeKickEvent(
				"KicksGifted",
				frame("channel.1234",
				      with("sender", json{{"username", "gifter"}, {"username_color", "red"}})),
				badColor) &&
			Events::NormalizeKickEvent("KicksGifted",
						   frame("channel.1234", json{{"sender", json{{"username", "gifter"}}},
									      {"gift", plainGift}}),
						   noTxn) &&
			!Events::NormalizeKickEvent("KicksGifted",
						    frame("channel.1234", with("gift", json{{"name", "Rage Quit"}})),
						    noAmount) &&
			!Events::NormalizeKickEvent("App\\Events\\ChatMessageEvent", frame("chatrooms.1.v2", payload),
						    chat);

		// The two deliveries differ only in receipt time, so everything else must match.
		const auto sansTs = [](const Events::NormalizedEvent &ev) {
			json j = ev.ToJson();
			j.erase("ts");
			return j;
		};
		const std::string noTxnPrefix = "kick:kicks:gifter:1:";
		const bool okKicks = normalized && dot.type == "kicks" && dot.platform == "kick" && dot.amount == 500 &&
				     dot.actorName == "gifter" && dot.actorColor == "#FF9D00" && dot.message == "w" &&
				     dot.tier == "Rage Quit" && dot.id == "kick:kicks:01J9KICKS8F2C" &&
				     dot.count == 0 && dot.currency.empty() && underscore.id == dot.id &&
				     sansTs(underscore) == sansTs(dot) && identity.actorColor == "#00aaff" &&
				     badColor.actorColor.empty() && noTxn.id.rfind(noTxnPrefix, 0) == 0 &&
				     noTxn.id.size() > noTxnPrefix.size() && noTxn.tier == "Hype" &&
				     noTxn.message.empty() && noTxn.actorColor.empty();
		// The channel.<id> / channel_<id> twin is dropped before normalizing; a repeat on one
		// spelling is a second genuine gift, and an expired or chatroom frame never matches.
		Events::KickDeliveryDedupe dedupe;
		const std::string raw = payload.dump();
		const std::string other = with("message", "gg").dump();
		const auto dup = [&](const char *channel, const json &data, int64_t ms) {
			return dedupe.IsDuplicate(frame(channel, data), ms);
		};
		const std::vector<bool> seen = {
			dup("channel.1234", raw, 1000),    // first delivery
			dup("channel_1234", raw, 1050),    // its twin -> dropped
			dup("channel.1234", raw, 2000),    // a second identical gift
			dup("channel.1234", raw, 2100),    // and a third, same spelling
			dup("channel_1234", raw, 2150),    // twin of the second -> dropped
			dup("channel_1234", raw, 2160),    // twin of the third -> dropped
			dup("channel_1234", raw, 2170),    // nothing left to pair with
			dup("channel.1234", raw, 30000),   // remembered...
			dup("channel_1234", raw, 40001),   // ...but past the window
			dup("chatrooms.1.v2", raw, 45000), // chatroom frames are never collapsed
			dup("chatrooms.1.v2", raw, 45001),
			dup("channel.1234", other, 50000), // different data never matches
			dup("channel_1234", raw, 50010),
			dup("channel.1234", payload, 70000), // object-shaped data pairs the same way
			dup("channel_1234", payload, 70001),
		};
		const std::vector<bool> wantSeen = {false, true,  false, false, true,  true,  false, false,
						    false, false, false, false, false, false, true};
		const bool okDedupe = seen == wantSeen;
		HostLog(std::string("[selftest] kick-delivery-dedupe -> ") + (okDedupe ? "OK" : "MISMATCH"));
		if (!okDedupe) {
			std::string got;
			for (bool b : seen) {
				got += b ? '1' : '0';
			}
			HostLog("[selftest] kick-delivery-dedupe got=" + got);
		}

		HostLog(std::string("[selftest] kick-kicks -> ") + (okKicks ? "OK" : "MISMATCH"));
		if (!okKicks) {
			HostLog("[selftest] kick-kicks normalized=" + std::string(normalized ? "1" : "0") +
				" dot=" + dot.ToJson().dump() + " underscore=" + underscore.ToJson().dump() +
				" identity=" + identity.ToJson().dump() + " badColor=" + badColor.ToJson().dump() +
				" noTxn=" + noTxn.ToJson().dump());
		}
	}

	// 6c) Poll templates: identity is the trimmed question + options, so a re-run bumps one
	// row, and the list never outgrows its cap. A private instance that is never Load()ed or
	// Save()d, so the real poll_templates.json is untouched.
	{
		PollTemplateStore store;
		bool createdFirst = false;
		bool createdAgain = true;
		bool createdOther = false;
		const std::string first = store.Remember("Best map?", {"A", "B"}, createdFirst);
		const std::string other = store.Remember("Best map?", {"B", "A"}, createdOther);
		const std::string again = store.Remember("  Best map? ", {" A", "B  "}, createdAgain);
		const json afterDedupe = store.List();
		const bool renamed = store.Rename(first, "Maps");
		const std::string caseDiffers = store.Remember("best map?", {"A", "B"}, createdOther);
		for (size_t i = 0; i < PollTemplateStore::kMaxTemplates + 5; ++i) {
			bool created = false;
			store.Remember("Filler " + std::to_string(i), {"x", "y"}, created);
		}
		const json capped = store.List();
		const bool okTemplates = createdFirst && !first.empty() && !createdAgain && again == first &&
					 other != first && afterDedupe.size() == 2 && afterDedupe[0]["id"] == first &&
					 afterDedupe[0]["question"] == "Best map?" &&
					 afterDedupe[0]["options"] == json::array({"A", "B"}) && renamed &&
					 caseDiffers != first && capped.size() == PollTemplateStore::kMaxTemplates &&
					 capped[0]["question"] ==
						 "Filler " + std::to_string(PollTemplateStore::kMaxTemplates + 4) &&
					 !store.Touch(first) && store.Remove(capped[0]["id"].get<std::string>());
		HostLog(std::string("[selftest] poll-templates -> ") + (okTemplates ? "OK" : "MISMATCH"));
		if (!okTemplates) {
			HostLog("[selftest] poll-templates deduped=" + afterDedupe.dump() +
				" cappedSize=" + std::to_string(capped.size()));
		}
	}

	// 7) Preview guide overlays. preview.setOverlays and settings.setGeneral end in the
	// same General commit, so a write through either must read back through the other,
	// and a refused write must leave every stored value as it was.
	json g0 = run("settings.getGeneral", json(nullptr), ok);
	if (!ok) {
		return;
	}
	const char *const overlayKeys[] = {"previewOverflow", "previewOverflowInvisible", "previewSafeAreas",
					   "previewSpacingHelpers"};
	const auto field = [](const json &object, const char *key) {
		const auto it = object.find(key);
		return it == object.end() ? json() : *it;
	};
	json saved = json::object();
	for (const char *key : overlayKeys) {
		saved[key] = field(g0, key);
	}
	const auto overlaysAre = [&](const json &general, const json &expected) {
		for (const char *key : overlayKeys) {
			if (field(general, key) != field(expected, key)) {
				return false;
			}
		}
		return true;
	};
	const auto refused = [](const std::string &method, const json &params) {
		json result;
		std::string error;
		const bool accepted = Bridge::Dispatch(method, params, result, error);
		HostLog("[selftest] preview-overlays: " + method + " " +
			(accepted ? std::string("ACCEPTED a bad value") : "refused: " + Err::Diagnostic(error)));
		return !accepted;
	};
	const auto verdict = [](bool pass) {
		return std::string(pass ? "OK" : "MISMATCH");
	};

	const bool invisible0 = g0.value("previewOverflowInvisible", false);
	const bool safe0 = g0.value("previewSafeAreas", false);
	const bool spacing0 = g0.value("previewSpacingHelpers", false);
	const std::string mode1 = g0.value("previewOverflow", std::string()) == "always" ? "hidden" : "always";
	const bool haveSurface = Preview::GetView("").has_value();

	// 7a) setOverlays on the Default surface, read back through getGeneral.
	json expected = {{"previewOverflow", mode1},
			 {"previewOverflowInvisible", !invisible0},
			 {"previewSafeAreas", !safe0},
			 {"previewSpacingHelpers", !spacing0}};
	if (haveSurface) {
		const json view = run("preview.setOverlays",
				      json{{"overflow", mode1},
					   {"overflowInvisible", !invisible0},
					   {"safeAreas", !safe0},
					   {"spacingHelpers", !spacing0}},
				      ok);
		const bool replyOk = ok && view.value("overflow", std::string()) == mode1 &&
				     view.value("overflowInvisible", invisible0) == !invisible0 &&
				     view.value("safeAreas", safe0) == !safe0 &&
				     view.value("spacingHelpers", spacing0) == !spacing0;
		const json g1 = run("settings.getGeneral", json(nullptr), ok);
		HostLog("[selftest] preview-overlays: setOverlays reply " + verdict(replyOk) + ", getGeneral " +
			verdict(ok && overlaysAre(g1, expected)));

		// 7b) Each refused call also carries a valid change, which must not land either.
		const bool badMode =
			refused("preview.setOverlays", json{{"overflow", "sideways"}, {"safeAreas", safe0}});
		const bool badBool = refused("preview.setOverlays",
					     json{{"overflowInvisible", invisible0}, {"spacingHelpers", "yes"}});
		const json g2 = run("settings.getGeneral", json(nullptr), ok);
		HostLog("[selftest] preview-overlays: setOverlays refusals " + verdict(badMode && badBool) +
			", values intact " + verdict(ok && overlaysAre(g2, expected)));
	} else {
		expected = saved;
		HostLog("[selftest] preview-overlays: no Default preview surface, setOverlays checks skipped");
	}

	// 7c) A setOverlays naming no surface is refused before it commits anything, which is
	// the reason the target is resolved first. Runs whether or not a Default surface exists.
	const bool badTarget = refused("preview.setOverlays", json{{"canvas", "no-such-uuid"}, {"safeAreas", !safe0}});
	const json g2b = run("settings.getGeneral", json(nullptr), ok);
	HostLog("[selftest] preview-overlays: setOverlays bad-target refusal " + verdict(badTarget) +
		", values intact " + verdict(ok && overlaysAre(g2b, expected)));

	// 7d) An unknown mode through settings.setGeneral is refused with the rest of the call.
	const bool badStored =
		refused("settings.setGeneral", json{{"previewOverflow", "sideways"}, {"previewSafeAreas", !safe0}});
	const json g3 = run("settings.getGeneral", json(nullptr), ok);
	HostLog("[selftest] preview-overlays: setGeneral refusal " + verdict(badStored) + ", values intact " +
		verdict(ok && overlaysAre(g3, expected)));

	// 7e) setGeneral, read back through the surface's view.
	const std::string mode2 = "selection";
	run("settings.setGeneral",
	    json{{"previewOverflow", mode2},
		 {"previewOverflowInvisible", invisible0},
		 {"previewSafeAreas", !safe0},
		 {"previewSpacingHelpers", spacing0}},
	    ok);
	if (ok && haveSurface) {
		const json view = run("preview.getView", json(nullptr), ok);
		HostLog("[selftest] preview-overlays: setGeneral -> getView " +
			verdict(ok && view.value("overflow", std::string()) == mode2 &&
				view.value("overflowInvisible", !invisible0) == invisible0 &&
				view.value("safeAreas", safe0) == !safe0 &&
				view.value("spacingHelpers", !spacing0) == spacing0));
	}

	// 8) Restore the original overlay settings.
	run("settings.setGeneral", saved, ok);
	const json g4 = run("settings.getGeneral", json(nullptr), ok);
	HostLog("[selftest] preview-overlays: restored " + verdict(ok && overlaysAre(g4, saved)));
}

void ObsBootstrap::RunCanvasBridgeSelfTest()
{
	using Bridge::json;

	auto run = [](const std::string &method, const json &params, bool &ok) -> json {
		json result;
		std::string error;
		ok = Bridge::Dispatch(method, params, result, error);
		if (!ok) {
			HostLog("[selftest] " + method + " FAILED: " + Err::Diagnostic(error));
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
			HostLog("[selftest] " + method + " FAILED: " + Err::Diagnostic(error));
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
			(dupOk ? "ACCEPTED (BUG: should reject)" : "REJECTED (\"" + Err::Diagnostic(dupError) + "\")"));
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
			HostLog("[selftest] " + method + " FAILED: " + Err::Diagnostic(error));
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
			(dupOk ? "ACCEPTED (BUG: should reject)" : "REJECTED (\"" + Err::Diagnostic(dupError) + "\")"));
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

namespace {

// A temp destination pointed at a dead local RTMP host, for the self-tests that need a
// startable binding without a real ingest. In-memory only -- StreamProfileStore::Add does not
// Save -- so the caller removes the profile at the end and the stored file is never touched.
std::string MakeSelfTestProfile(const char *label)
{
	StreamProfile prof;
	prof.label = label;
	prof.serviceId = "rtmp_custom";
	prof.settings = OBSDataAutoRelease(obs_data_create());
	obs_data_set_string(prof.settings, "server", "rtmp://127.0.0.1:1/live");
	obs_data_set_string(prof.settings, "key", "selftest");
	return g_streamProfiles.Add(std::move(prof)).uuid;
}

} // namespace

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

	const std::string profileUuid = MakeSelfTestProfile("selftest-engine");

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

namespace {

// Bring up a temporary ADDITIONAL canvas for a self-test, the way the product
// does it. canvas.create (bridge.cpp) calls EnsureCanvas and then EnsureScenes,
// because seeding a default scene was deliberately decoupled from EnsureCanvas --
// so a fixture that calls only the first gets a canvas with no scene at all, and
// every later step fails with "no scene to add the source to" while the shipped
// path works fine. Six self-tests had copied the setup and all six had copied the
// omission; the ones that passed did so only because they created a scene of
// their own first.
//
// The preview ref is what builds the mix, and a mix is not optional for anything
// that reads geometry back. An item on a mix-less canvas has no canvas size to
// resolve against, so its box comes back degenerate -- which is how three
// transform assertions came to be failing against arithmetic that is correct:
// a 90-degree rotation reported exactly the drift of an uncorrected pivot, and a
// bounds item reported a 0x0 box. The app never edits in that state either, since
// the preview surface a user drags on takes this same ref before it renders.
//
// In-memory only: the caller never Saves, and removes the canvas at the end.
std::string MakeSelfTestCanvas(const char *name)
{
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
	g_canvasRuntime->EnsureScenes();
	g_canvasRuntime->AddPreview(uuid);
	return uuid;
}

// The status row multistream.changed would carry for one binding, or nullopt where the
// report has none -- Statuses() enumerates only ENABLED bindings, so an absent row and a
// row reading Idle are different answers.
std::optional<MultistreamEngine::OutputStatus> SelfTestStatusFor(const std::string &bindingUuid)
{
	for (const MultistreamEngine::OutputStatus &st : g_multistream->Statuses()) {
		if (st.bindingUuid == bindingUuid) {
			return st;
		}
	}
	return std::nullopt;
}

} // namespace

void ObsBootstrap::RunMultistreamArmSelfTest()
{
	using Bridge::json;

	// Every reading below turns on whether a session is running, so one already in flight
	// would make all of them meaningless.
	if (g_multistream->AnyLive()) {
		HostLog("[selftest] arm SKIPPED: something is already streaming");
		return;
	}

	const std::string canvasUuid = g_canvases.Default().uuid;
	const size_t profilesBefore = g_streamProfiles.Profiles().size();
	const size_t bindingsBefore = g_outputBindings.Bindings().bindings.size();

	// Two temp destinations: one to hold a session open, one to arm against it.
	const std::string holderProfile = MakeSelfTestProfile("selftest-arm-holder");
	OutputBinding &holder = g_outputBindings.Bindings().Add(canvasUuid);
	holder.profileUuid = holderProfile;
	holder.enabled = true;
	const std::string holderUuid = holder.uuid;

	// multistream.startOutput with nothing live is refused rather than starting bare: this
	// path joins a broadcast that is running, and starting one destination on its own would
	// go out with none of the preparation go-live does for the rest.
	json refusal;
	std::string dispatchError;
	const bool dispatched =
		Bridge::Dispatch("multistream.startOutput", json{{"uuid", holderUuid}}, refusal, dispatchError);
	const bool refusedIdle = dispatched && refusal.is_object() && !refusal.value("ok", true) &&
				 !refusal.value("error", std::string()).empty() && !g_multistream->IsLive(holderUuid);
	HostLog(std::string("[selftest] arm startOutput with no session -> ") +
		(refusedIdle ? "refused (OK)" : "MISMATCH") + ": " +
		(dispatched ? refusal.dump() : Err::Diagnostic(dispatchError)));

	// Hold a session open. A dead host leaves this Connecting, or Reconnecting once the
	// refusal lands -- both count as live. It can also reach Error before the next line
	// runs if the user has automatic reconnect turned off, which weakens the arm assertion
	// below without invalidating it, so the log says which of the two was proven.
	g_multistream->StartOutput(holderUuid);
	const bool sessionLive = g_multistream->AnyLive();

	// The two session predicates must disagree here, and that disagreement is the whole reason
	// both exist: the holder has an entry (StartedThisSession) but a dead host never signals
	// start, so it never went live. A build reading WentLive off the LiveOutput reports them equal.
	const bool startedSeen = g_multistream->StartedThisSession(holderUuid);
	const bool wentLiveSeen = g_multistream->WentLiveThisSession(holderUuid);
	// And it must survive the entry being replaced. A second StartOutput is the shape a user's
	// Retry takes: it reaps the old entry and pushes a fresh one whenever the first is already
	// dead (it short-circuits while that one is still connecting). If the fact lived on the
	// entry, the reaping branch is where it would be lost.
	g_multistream->StartOutput(holderUuid);
	const bool startedAfter = g_multistream->StartedThisSession(holderUuid);
	const bool wentLiveAfter = g_multistream->WentLiveThisSession(holderUuid);
	HostLog(std::string("[selftest] arm session predicates -> ") +
		((startedSeen && !wentLiveSeen && startedAfter && !wentLiveAfter) ? "OK" : "MISMATCH") +
		"; connecting-only started=" + (startedSeen ? "true" : "false (BUG)") +
		" wentLive=" + (wentLiveSeen ? "true (BUG)" : "false") + "; after a second start started=" +
		(startedAfter ? "true" : "false (BUG)") + " wentLive=" + (wentLiveAfter ? "true (BUG)" : "false") +
		" (a real connection flips wentLive true and it must then STAY true; not headless)");

	const std::string armedProfile = MakeSelfTestProfile("selftest-arm-armed");
	OutputBinding &armed = g_outputBindings.Bindings().Add(canvasUuid);
	armed.profileUuid = armedProfile;
	armed.enabled = false;
	const std::string armedUuid = armed.uuid;

	// Arming starts nothing: the destination has to be prepared against its platform first,
	// and that is gated on the user validating its stream info.
	std::string armError;
	const bool armOk = Bridge::SetOutputBindingEnabled(armedUuid, true, armError);
	const std::optional<MultistreamEngine::OutputStatus> armedRow = SelfTestStatusFor(armedUuid);
	const std::optional<MultistreamEngine::OutputStatus> holderRow = SelfTestStatusFor(holderUuid);
	const bool armedIdle = armOk && !g_multistream->IsLive(armedUuid) && armedRow && !armedRow->startedThisSession;
	// The holder is the positive control, and it is part of the verdict rather than a note
	// beside it: without it "no live entry" would also pass on a report that simply never
	// sets the field.
	const bool holderMarked = holderRow && holderRow->startedThisSession;
	HostLog(std::string("[selftest] arm mid-session (session ") + (sessionLive ? "live" : "already down") +
		") -> " + ((armedIdle && holderMarked) ? "started nothing (OK)" : "MISMATCH") +
		"; startedThisSession armed=" + ((armedRow && armedRow->startedThisSession) ? "true (BUG)" : "false") +
		" holder=" + (holderMarked ? "true" : "false (BUG)") +
		(armOk ? std::string() : "; setEnabled FAILED: " + Err::Diagnostic(armError)));

	// Arming is refused outright while a go-live prelude is in flight: that prelude has
	// already snapshotted the destinations it is creating broadcasts against, and the
	// StartAllEnabled that follows it would start this one against the ingest the last
	// go-live wrote into its profile.
	{
		Bridge::ScopedGoLivePrelude prelude;
		std::string preludeError;
		const bool refused = !Bridge::SetOutputBindingEnabled(armedUuid, true, preludeError);
		HostLog(std::string("[selftest] arm during go-live prelude -> ") +
			(refused ? "refused (OK): " + preludeError : "ALLOWED (BUG)"));
	}

	// Retargeting a LIVE binding onto a different stream profile must not restart it: the new
	// profile carries the ingest the last go-live wrote there and no broadcast of this session's,
	// so a restart would push at a stale endpoint. It is stopped and left staged instead. A
	// canvas-only move still restarts, because that streams to the same place.
	const std::string retargetProfile = MakeSelfTestProfile("selftest-arm-retarget");
	// Read first: the holder can already have reached Error here (see above), and then the edit
	// finds nothing live to stop and this proves nothing. Said out loud rather than passing
	// quietly, because a vacuous OK is what would let the restart come back unnoticed.
	const bool liveBefore = g_multistream->IsLive(holderUuid);
	json retargeted;
	std::string retargetError;
	const bool retargetOk = Bridge::Dispatch("outputBinding.update",
						 json{{"uuid", holderUuid}, {"profileUuid", retargetProfile}},
						 retargeted, retargetError);
	const OutputBinding *holderAfter = g_outputBindings.Bindings().Find(holderUuid);
	const bool stillLive = g_multistream->IsLive(holderUuid);
	const bool staged = retargetOk && holderAfter && holderAfter->profileUuid == retargetProfile && !stillLive;
	HostLog(std::string("[selftest] retarget live binding profile -> ") +
		(staged ? (liveBefore ? "stopped, not restarted (OK)"
				      : "PROVES NOTHING (holder was not live; no restart was possible)")
			: "MISMATCH") +
		"; liveBefore=" + (liveBefore ? "true" : "false") +
		"; profile=" + (holderAfter ? holderAfter->profileUuid : std::string("<gone>")) +
		" live=" + (stillLive ? "true (BUG)" : "false") +
		(retargetOk ? std::string() : "; update FAILED: " + Err::Diagnostic(retargetError)));

	// Restore. SetOutputBindingEnabled Saved for itself, so the removal Saves too -- into the
	// self-test config base, not the developer's, which is what makes writing here safe at
	// all (Env::IsSelfTestRun). Restored anyway, so the tests that run after this one read
	// the model they would have found without it.
	g_multistream->StopOutput(holderUuid);
	g_outputBindings.Bindings().Remove(armedUuid);
	g_outputBindings.Bindings().Remove(holderUuid);
	g_outputBindings.Save();
	g_streamProfiles.Remove(armedProfile);
	g_streamProfiles.Remove(holderProfile);
	g_streamProfiles.Remove(retargetProfile);
	HostLog("[selftest] arm cleanup: profiles " + std::to_string(g_streamProfiles.Profiles().size()) + " (was " +
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
	const std::string canvasUuid = MakeSelfTestCanvas("selftest-runtime-canvas");

	const std::string profileUuid = MakeSelfTestProfile("selftest-runtime");

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
			HostLog("[selftest] " + method + " FAILED: " + Err::Diagnostic(error));
			return json(nullptr);
		}
		return result;
	};

	// Bring up a temporary ADDITIONAL canvas with its own live mix (+ a default
	// channel-0 "Scene"), exactly like the canvas-runtime selftest. The point is to
	// prove the bridge's scene/source ops, when given this canvas's uuid, act on the
	// canvas's OWN scenes -- isolated from the global channel-0 scene list. The
	// reorder steps at the end go through scenes.reorder, which persists; a smoke run
	// writes into its own throwaway config dir, and the temp canvas is torn down here
	// so nothing of it survives into the next save.
	const std::string canvasUuid = MakeSelfTestCanvas("selftest-scene-canvas");

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

	// 5) Reorder within the canvas. Its scene order is tracked per canvas
	// (SceneCollection::SceneOrder), independently of the Default canvas's -- the
	// two must never bleed into each other, so the third scene deliberately takes a
	// Default-canvas scene's NAME. Resolving that name globally would find the MAIN
	// scene instead, so the collision is the assertion.
	auto listNames = [&](const json &listParams) -> std::vector<std::string> {
		bool listOk = false;
		json rows = run("scenes.list", listParams, listOk);
		std::vector<std::string> names;
		if (listOk && rows.is_array()) {
			for (const auto &s : rows) {
				names.push_back(s.value("name", std::string()));
			}
		}
		return names;
	};
	auto joinNames = [](const std::vector<std::string> &names) -> std::string {
		std::string out;
		for (const std::string &n : names) {
			out += (out.empty() ? "" : ",") + n;
		}
		return out;
	};

	// The collision needs a Default-canvas name the canvas does not already use, and
	// the canvas is seeded with one called "Scene" -- so a real config whose only main
	// scene is also "Scene" would leave nothing to pick and quietly reduce the whole
	// step to a no-collision run. Mint a main scene with a name the canvas cannot
	// already hold, so a candidate always exists (removed again in cleanup). It also
	// makes the Default-order comparison below non-vacuous: a one-element list
	// compares equal however it is reordered.
	const char *kOrderMainScene = "selftest-order-main";
	run("scenes.create", json{{"name", kOrderMainScene}}, ok);
	const bool mainSceneAdded = ok;
	HostLog(std::string("[selftest] canvas-scene-order main scene '") + kOrderMainScene + "' -> " +
		(mainSceneAdded ? "true" : "false (BUG)"));

	const std::vector<std::string> mainBefore = listNames(json(nullptr));
	std::vector<std::string> canvasNamesNow = listNames(json{{"canvas", canvasUuid}});
	// A name already taken inside the canvas would be refused by the canvas runtime,
	// so pick the first Default-canvas name that is still free here.
	std::string collidingName = "selftest-canvas-scene-b";
	bool collides = false;
	for (const std::string &n : mainBefore) {
		if (std::find(canvasNamesNow.begin(), canvasNamesNow.end(), n) == canvasNamesNow.end()) {
			collidingName = n;
			collides = true;
			break;
		}
	}
	run("scenes.create", json{{"canvas", canvasUuid}, {"name", collidingName}}, ok);
	// `collides` is the assertion, not a diagnostic: without it the reorders below run
	// on a name no main scene shares, and nothing here tests the resolver at all.
	HostLog(std::string("[selftest] canvas-scene-order third scene '") + collidingName + "' -> " +
		(ok ? "true" : "false (BUG)") +
		"; name also on Default canvas=" + (collides ? "true" : "false (BUG: collision untested)") +
		" (Default has " + std::to_string(mainBefore.size()) + ": " + joinNames(mainBefore) + ")");

	const std::vector<std::string> orderBefore = listNames(json{{"canvas", canvasUuid}});
	const bool haveThree = orderBefore.size() >= 3;

	// direction:"down" on the top scene -- refused outright before per-canvas order.
	run("scenes.reorder",
	    json{{"canvas", canvasUuid}, {"name", haveThree ? orderBefore[0] : collidingName}, {"direction", "down"}},
	    ok);
	HostLog(std::string("[selftest] canvas-scene-order scenes.reorder down -> ") + (ok ? "true" : "false (BUG)"));

	// The load-bearing half: scenes.list must REFLECT the move, not just accept it.
	const std::vector<std::string> afterDown = listNames(json{{"canvas", canvasUuid}});
	const bool downApplied = haveThree && afterDown.size() == orderBefore.size() &&
				 afterDown[0] == orderBefore[1] && afterDown[1] == orderBefore[0] &&
				 afterDown[2] == orderBefore[2];
	HostLog(std::string("[selftest] canvas-scene-order scenes.list after down -> ") +
		(downApplied ? "true" : "false (BUG)") + " (" + joinNames(orderBefore) + " -> " + joinNames(afterDown) +
		")");

	// Absolute move: {to:0} puts the colliding-name scene at the top.
	run("scenes.reorder", json{{"canvas", canvasUuid}, {"name", collidingName}, {"to", 0}}, ok);
	const std::vector<std::string> afterTop = listNames(json{{"canvas", canvasUuid}});
	const bool topApplied = ok && !afterTop.empty() && afterTop[0] == collidingName;
	HostLog(std::string("[selftest] canvas-scene-order scenes.reorder to=0 -> ") +
		(topApplied ? "true" : "false (BUG)") + " (" + joinNames(afterTop) + ")");

	// The Default canvas's own order must be untouched by every move above. Its list
	// holds at least two scenes here, so this can actually fail.
	const std::vector<std::string> mainAfter = listNames(json(nullptr));
	HostLog(std::string("[selftest] canvas-scene-order Default order unchanged -> ") +
		(mainAfter == mainBefore ? "true" : "false (BUG)") + " (" + joinNames(mainBefore) + " -> " +
		joinNames(mainAfter) + ")");

	// Drop the main scene minted above so the Default canvas returns to baseline for
	// the self-tests that run after this one.
	if (mainSceneAdded) {
		run("scenes.remove", json{{"name", kOrderMainScene}}, ok);
		HostLog(std::string("[selftest] canvas-scene-order main scene cleanup -> ") +
			(ok ? "true" : "false (BUG)"));
	}

	// Clean up: remove the source from the canvas scene, then destroy the temp
	// canvas + drop its mix, returning the in-memory model to baseline. Destroying
	// the canvas releases its scenes (including our created ones), and the next
	// Save prunes its now-dead uuid out of the persisted per-canvas scene order.
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
			HostLog("[selftest] " + method + " FAILED: " + Err::Diagnostic(error));
			return json(nullptr);
		}
		return result;
	};

	// Bring up two temporary ADDITIONAL canvases (source + destination), exactly
	// like the canvas-scene selftest. Operate ONLY on the in-memory stores (never
	// Save explicitly) so the user's files stay untouched. The point is to prove
	// scenes.duplicateToCanvas performs a real deep copy across canvases, with
	// undo/redo that preserves the copied source's uuid on restore.
	const std::string srcCanvasUuid = MakeSelfTestCanvas("selftest-duplicate-src-canvas");
	const std::string destCanvasUuid = MakeSelfTestCanvas("selftest-duplicate-dest-canvas");

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

	// Helper: make `sceneName` current on `canvasUuid`, assert it has exactly one
	// item, and return that item's source uuid (empty on any failure).
	auto singleItemSourceUuid = [&](const std::string &canvasUuid, const std::string &sceneName, int64_t &outItemId,
					std::string &outSrcName) -> std::string {
		bool setOk = false;
		run("scenes.setCurrent", json{{"canvas", canvasUuid}, {"name", sceneName}}, setOk);
		if (!setOk) {
			return {};
		}
		bool listOk = false;
		json items = run("sceneItems.list", json{{"canvas", canvasUuid}}, listOk);
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
	const std::string dupSrcUuid =
		foundOnDest ? singleItemSourceUuid(destCanvasUuid, newSceneName, dupItemId, dupSrcName) : std::string();
	const bool isRealCopy = !dupSrcUuid.empty() && dupSrcUuid != origSrcUuid;
	HostLog(std::string("[selftest] scene-duplicate item copy -> uuid=") +
		(dupSrcUuid.empty() ? "MISSING (BUG)" : dupSrcUuid) +
		"; independent-of-original=" + (isRealCopy ? "true" : "false (BUG)"));

	// 3b) scenes.duplicate: a same-canvas ref-duplicate on the SOURCE canvas. This
	// runs before Undo() below on purpose -- scenes.duplicate adds no UndoManager
	// entry, so Undo() must still pop the duplicateToCanvas action from step 2;
	// running this block first makes that a live assertion of the settled
	// no-undo decision, not just a comment.
	json sameDup = run("scenes.duplicate", json{{"name", kSceneName}, {"canvas", srcCanvasUuid}}, ok);
	const std::string sameName = ok ? sameDup.value("name", std::string()) : std::string();
	HostLog(std::string("[selftest] scene-duplicate scenes.duplicate -> ") +
		(ok ? "ok name='" + sameName + "'" : "FAIL (BUG)"));

	const bool sameNameSuffixed = sameName == std::string(kSceneName) + " 2";
	HostLog(std::string("[selftest] scene-duplicate same-canvas name -> ") +
		(sameNameSuffixed ? "'" + sameName + "'" : "WRONG '" + sameName + "' (BUG)"));

	// The copy must land on the SOURCE canvas alongside the original -- the whole
	// premise of "no obs_canvas_move_scene needed" -- and must NOT leak onto the
	// Default canvas.
	json srcScenes = run("scenes.list", json{{"canvas", srcCanvasUuid}}, ok);
	bool srcHasOriginal = false;
	bool srcHasDuplicate = false;
	if (ok && srcScenes.is_array()) {
		for (const auto &s : srcScenes) {
			const std::string n = s.value("name", std::string());
			srcHasOriginal = srcHasOriginal || n == kSceneName;
			srcHasDuplicate = srcHasDuplicate || n == sameName;
		}
	}
	HostLog(std::string("[selftest] scene-duplicate srcCanvas scenes.list -> original=") +
		(srcHasOriginal ? "true" : "false (BUG)") + " duplicate=" + (srcHasDuplicate ? "true" : "false (BUG)"));

	json defaultScenes = run("scenes.list", json(nullptr), ok);
	bool leakedToDefault = false;
	if (ok && defaultScenes.is_array()) {
		for (const auto &s : defaultScenes) {
			if (s.value("name", std::string()) == sameName) {
				leakedToDefault = true;
				break;
			}
		}
	}
	HostLog(std::string("[selftest] scene-duplicate Default scenes.list -> leaked=") +
		(leakedToDefault ? "true (BUG)" : "false"));

	// OBS_SCENE_DUP_REFS: the copy's single item must point at the SAME source
	// uuid as the original -- the contrast to isRealCopy above, which asserts the
	// opposite for scenes.duplicateToCanvas's deep copy.
	int64_t sameItemId = 0;
	std::string sameSrcName;
	const std::string sameSrcUuid = singleItemSourceUuid(srcCanvasUuid, sameName, sameItemId, sameSrcName);
	const bool sameRefIsShared = !sameSrcUuid.empty() && sameSrcUuid == origSrcUuid;
	HostLog(std::string("[selftest] scene-duplicate same-canvas item ref -> uuid=") +
		(sameSrcUuid.empty() ? "MISSING (BUG)" : sameSrcUuid) +
		"; shared-with-original=" + (sameRefIsShared ? "true" : "false (BUG)"));

	// singleItemSourceUuid just made the duplicate current on srcCanvasUuid;
	// restore kSceneName so the srcItemId-based cleanup below still targets the
	// scene it was captured from.
	run("scenes.setCurrent", json{{"canvas", srcCanvasUuid}, {"name", kSceneName}}, ok);

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
	const std::string redoSrcUuid =
		backAfterRedo ? singleItemSourceUuid(destCanvasUuid, newSceneName, redoItemId, redoSrcName)
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

void ObsBootstrap::RunSourceDuplicateSelfTest()
{
	using Bridge::json;

	auto run = [](const std::string &method, const json &params, bool &ok) -> json {
		json result;
		std::string error;
		ok = Bridge::Dispatch(method, params, result, error);
		if (!ok) {
			HostLog("[selftest] " + method + " FAILED: " + Err::Diagnostic(error));
			return json(nullptr);
		}
		return result;
	};

	// Helper: uuid of `s`, or empty if null -- the shared core both the by-name and
	// by-pointer resolvers below reduce to (a canvas-scoped source, e.g. a nested
	// scene, has no obs_get_source_by_name entry to resolve a name against).
	auto uuidOfSource = [](obs_source_t *s) -> std::string {
		if (!s) {
			return {};
		}
		const char *u = obs_source_get_uuid(s);
		return u ? u : std::string();
	};

	// Helper: uuid of the source currently named `name` in the GLOBAL registry, or
	// empty if not found.
	auto sourceUuid = [&](const std::string &name) -> std::string {
		if (name.empty()) {
			return {};
		}
		OBSSourceAutoRelease s = obs_get_source_by_name(name.c_str());
		return uuidOfSource(s);
	};

	// Helper: filters.list's element count for the source named `name`, or -1 on
	// any failure (so a failure never gets silently mistaken for a count of 0).
	auto filterCount = [&](const std::string &name) -> int {
		bool ok = false;
		json filters = run("filters.list", json{{"source", name}}, ok);
		if (!ok || !filters.is_array()) {
			return -1;
		}
		return static_cast<int>(filters.size());
	};

	// Helper: item count of scene source `s`, or -1 when it doesn't resolve to a
	// scene. Read straight off libobs: what matters is what the copy actually holds,
	// not what a scene-addressed bridge method reports for it.
	auto itemCountOfScene = [](obs_source_t *s) -> int {
		obs_scene_t *scene = s ? obs_scene_from_source(s) : nullptr; // borrowed
		if (!scene) {
			return -1;
		}
		int count = 0;
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *, void *param) -> bool {
				*static_cast<int *>(param) += 1;
				return true;
			},
			&count);
		return count;
	};

	// One temporary ADDITIONAL canvas: everything under test happens in a single
	// scene, so there's no need for the source/destination pair the scene-duplicate
	// self-test uses.
	const std::string canvasUuid = MakeSelfTestCanvas("selftest-duplicate-canvas");

	bool ok = false;

	const char *kSceneName = "selftest-source-duplicate-scene";
	run("scenes.create", json{{"canvas", canvasUuid}, {"name", kSceneName}}, ok);
	HostLog(std::string("[selftest] source-duplicate scenes.create -> ") + (ok ? "ok" : "FAIL (BUG)"));

	run("scenes.setCurrent", json{{"canvas", canvasUuid}, {"name", kSceneName}}, ok);
	HostLog(std::string("[selftest] source-duplicate scenes.setCurrent -> ") + (ok ? "ok" : "FAIL (BUG)"));

	// wasapi_output_capture: a flagged (OBS_SOURCE_DO_NOT_DUPLICATE) type already
	// proven headless-safe by RunAudioMixerSelfTest, so this doesn't depend on a
	// real display/GPU the way a video capture type might.
	json wasapiCreated = run(
		"sources.create",
		json{{"canvas", canvasUuid}, {"type", "wasapi_output_capture"}, {"name", "selftest-duplicate-wasapi"}},
		ok);
	const int64_t wasapiItemId = ok ? wasapiCreated.value("id", int64_t(0)) : 0;
	const std::string wasapiSrcName = ok ? wasapiCreated.value("source", std::string()) : std::string();
	HostLog(std::string("[selftest] source-duplicate sources.create(wasapi) -> ") +
		(ok ? "id=" + std::to_string(wasapiItemId) + " source='" + wasapiSrcName + "'" : "FAIL (BUG)"));

	// Assert the type actually carries the flag under test -- a green result below
	// proves nothing if it doesn't.
	uint32_t wasapiFlags = 0;
	{
		OBSSourceAutoRelease s = wasapiSrcName.empty() ? nullptr
							       : obs_get_source_by_name(wasapiSrcName.c_str());
		if (s) {
			wasapiFlags = obs_source_get_output_flags(s);
		}
	}
	const bool wasapiIsFlagged = (wasapiFlags & OBS_SOURCE_DO_NOT_DUPLICATE) != 0;
	HostLog(std::string("[selftest] source-duplicate wasapi output_flags -> ") + std::to_string(wasapiFlags) +
		"; OBS_SOURCE_DO_NOT_DUPLICATE set=" + (wasapiIsFlagged ? "true" : "false (BUG)"));

	const std::string origWasapiUuid = sourceUuid(wasapiSrcName);
	HostLog(std::string("[selftest] source-duplicate original wasapi uuid -> ") +
		(origWasapiUuid.empty() ? "MISSING (BUG)" : origWasapiUuid));

	const int origWasapiFilterCount = filterCount(wasapiSrcName);
	HostLog(std::string("[selftest] source-duplicate original filterCount=") +
		std::to_string(origWasapiFilterCount) + " (expected 0)");

	// Duplicate the flagged source through the same bridge method the UI's
	// "Duplicate" action drives -- never DuplicateSourceObject/obs_source_duplicate
	// directly, since the bridge method is what's actually under test.
	json wasapiDup = run("sources.duplicate", json{{"canvas", canvasUuid}, {"id", wasapiItemId}}, ok);
	const int64_t wasapiDupItemId = ok ? wasapiDup.value("id", int64_t(0)) : 0;
	const std::string wasapiDupSrcName = ok ? wasapiDup.value("source", std::string()) : std::string();
	HostLog(std::string("[selftest] source-duplicate sources.duplicate(wasapi) -> ") +
		(ok ? "id=" + std::to_string(wasapiDupItemId) + " source='" + wasapiDupSrcName + "'" : "FAIL (BUG)"));

	const std::string wasapiDupUuid = sourceUuid(wasapiDupSrcName);
	const bool wasapiIndependent = !wasapiDupUuid.empty() && wasapiDupUuid != origWasapiUuid;
	HostLog(std::string("[selftest] source-duplicate wasapi copy uuid -> ") +
		(wasapiDupUuid.empty() ? "MISSING (BUG)" : wasapiDupUuid) +
		"; independent-of-original=" + (wasapiIndependent ? "true" : "false (BUG)"));

	// Add a filter to the COPY only, then assert the original's chain is untouched
	// while the copy's grew by exactly one -- the exact regression this test
	// guards against, where "duplicate" of a flagged type aliased the original.
	// gain_filter (not crop_filter): wasapi_output_capture is audio-only, and
	// libobs's filter_compatible (obs-source.c) silently refuses to attach a
	// video-only filter to an audio-only source, which would make this assertion
	// pass for the wrong reason (no filter attached to either side).
	run("filters.add", json{{"source", wasapiDupSrcName}, {"type", "gain_filter"}}, ok);
	HostLog(std::string("[selftest] source-duplicate filters.add(copy) -> ") + (ok ? "ok" : "FAIL (BUG)"));

	// Literal 0/1 rather than a comparison against origWasapiFilterCount: filterCount
	// reports -1 for a failed read, and a relative check would call -1 vs -1 "unchanged"
	// and 0 vs -1+1 "grew by one", turning an all-failing sequence green.
	const int finalOrigFilterCount = filterCount(wasapiSrcName);
	const int finalDupFilterCount = filterCount(wasapiDupSrcName);
	HostLog(std::string("[selftest] source-duplicate original filterCount=") +
		std::to_string(finalOrigFilterCount) + " (expected 0)");
	HostLog(std::string("[selftest] source-duplicate copy filterCount=") + std::to_string(finalDupFilterCount) +
		" (expected 1)");
	const bool originalUnaffected = origWasapiFilterCount == 0 && finalOrigFilterCount == 0;
	const bool copyGrewByOne = finalDupFilterCount == 1;
	HostLog(std::string("[selftest] source-duplicate independent filter chains -> ") +
		((originalUnaffected && copyGrewByOne) ? "true" : "false (BUG)"));

	// obs_source_get_settings only addref's the source's OWN obs_data_t, and passing
	// that straight into obs_source_create would just addref it again rather than copy
	// it (obs_data_newref never copies) -- so the "independent" duplicate could end up
	// sharing one settings object with the original, and a properties.set on either one
	// would silently rewrite both. Flip a bool setting on the COPY and assert the
	// ORIGINAL's own properties.get is unchanged, to guard DuplicateSourceObject's
	// settings-copy step against that regression. Fold the key's presence into *Ok
	// (like filterCount's -1 sentinel above) so a future wasapi refactor that drops
	// use_device_timing fails loudly instead of silently comparing two false defaults.
	bool origPropsOk = false;
	json origProps = run("properties.get", json{{"kind", "source"}, {"ref", wasapiSrcName}}, origPropsOk);
	origPropsOk = origPropsOk && origProps["values"].contains("use_device_timing");
	const bool origUseDeviceTiming = origPropsOk ? origProps["values"].value("use_device_timing", false) : false;
	HostLog(std::string("[selftest] source-duplicate original use_device_timing (before) -> ") +
		(origPropsOk ? (origUseDeviceTiming ? "true" : "false") : "FAIL (BUG)"));

	bool setOk = false;
	run("properties.set",
	    json{{"kind", "source"},
		 {"ref", wasapiDupSrcName},
		 {"settings", json{{"use_device_timing", !origUseDeviceTiming}}}},
	    setOk);
	HostLog(std::string("[selftest] source-duplicate properties.set(copy) -> ") + (setOk ? "ok" : "FAIL (BUG)"));

	bool afterPropsOk = false;
	json afterProps = run("properties.get", json{{"kind", "source"}, {"ref", wasapiSrcName}}, afterPropsOk);
	afterPropsOk = afterPropsOk && afterProps["values"].contains("use_device_timing");
	const bool afterUseDeviceTiming = afterPropsOk ? afterProps["values"].value("use_device_timing", false) : false;
	const bool settingsIndependent = origPropsOk && afterPropsOk && afterUseDeviceTiming == origUseDeviceTiming;
	HostLog(std::string("[selftest] source-duplicate original use_device_timing (after copy edit) -> ") +
		(afterPropsOk ? (afterUseDeviceTiming ? "true" : "false") : "FAIL (BUG)") +
		"; unchanged=" + (settingsIndependent ? "true" : "false (BUG)"));

	// Also cover the ordinary (non-flagged) obs_source_duplicate path, so a future
	// regression there is caught too. No filter-count assertion needed here -- the
	// uuid-independence check is what that path already guarantees.
	json colorCreated =
		run("sources.create",
		    json{{"canvas", canvasUuid}, {"type", "color_source"}, {"name", "selftest-duplicate-color"}}, ok);
	const int64_t colorItemId = ok ? colorCreated.value("id", int64_t(0)) : 0;
	const std::string colorSrcName = ok ? colorCreated.value("source", std::string()) : std::string();
	HostLog(std::string("[selftest] source-duplicate sources.create(color) -> ") +
		(ok ? "id=" + std::to_string(colorItemId) + " source='" + colorSrcName + "'" : "FAIL (BUG)"));

	const std::string origColorUuid = sourceUuid(colorSrcName);
	HostLog(std::string("[selftest] source-duplicate original color uuid -> ") +
		(origColorUuid.empty() ? "MISSING (BUG)" : origColorUuid));

	json colorDup = run("sources.duplicate", json{{"canvas", canvasUuid}, {"id", colorItemId}}, ok);
	const int64_t colorDupItemId = ok ? colorDup.value("id", int64_t(0)) : 0;
	const std::string colorDupSrcName = ok ? colorDup.value("source", std::string()) : std::string();
	HostLog(std::string("[selftest] source-duplicate sources.duplicate(color) -> ") +
		(ok ? "id=" + std::to_string(colorDupItemId) + " source='" + colorDupSrcName + "'" : "FAIL (BUG)"));

	const std::string colorDupUuid = sourceUuid(colorDupSrcName);
	const bool colorIndependent = !colorDupUuid.empty() && colorDupUuid != origColorUuid;
	HostLog(std::string("[selftest] source-duplicate color copy uuid -> ") +
		(colorDupUuid.empty() ? "MISSING (BUG)" : colorDupUuid) +
		"; independent-of-original=" + (colorIndependent ? "true" : "false (BUG)"));

	// A nested scene is flagged OBS_SOURCE_DO_NOT_DUPLICATE too, and unlike every other
	// flagged type its content is its item list rather than its settings -- so a copy
	// rebuilt from settings alone comes back EMPTY. Assert both halves: a different
	// obs_source_t, and a copy that actually carries the original's items.
	//
	// A scene created on an ADDITIONAL canvas lives in that canvas's own source
	// namespace (obs_canvas_get_source_by_name), and obs_scene_duplicate preserves
	// that same placement for the copy (it reads the original's owning canvas) --
	// neither is in the global obs_get_source_by_name registry sourceUuid/filterCount
	// resolve against, so this block resolves both scenes directly off the canvas
	// instead of through those name-based helpers, which would silently read back
	// "not found".
	const char *kNestedSceneName = "selftest-source-duplicate-nested";
	run("scenes.create", json{{"canvas", canvasUuid}, {"name", kNestedSceneName}}, ok);
	HostLog(std::string("[selftest] source-duplicate nested scenes.create -> ") + (ok ? "ok" : "FAIL (BUG)"));

	run("scenes.setCurrent", json{{"canvas", canvasUuid}, {"name", kNestedSceneName}}, ok);
	json nestedChild =
		run("sources.create",
		    json{{"canvas", canvasUuid}, {"type", "color_source"}, {"name", "selftest-duplicate-nested-color"}},
		    ok);
	const std::string nestedChildName = ok ? nestedChild.value("source", std::string()) : std::string();
	HostLog(std::string("[selftest] source-duplicate nested scene child -> ") +
		(ok ? "'" + nestedChildName + "'" : "FAIL (BUG)"));

	run("scenes.setCurrent", json{{"canvas", canvasUuid}, {"name", kSceneName}}, ok);

	obs_canvas_t *testCanvas = g_canvasRuntime->Find(canvasUuid);
	OBSSourceAutoRelease origNestedScene = testCanvas ? obs_canvas_get_source_by_name(testCanvas, kNestedSceneName)
							  : nullptr; // addref'd
	const std::string origNestedUuid = uuidOfSource(origNestedScene);
	const int origNestedItems = itemCountOfScene(origNestedScene);
	HostLog(std::string("[selftest] source-duplicate original nested scene -> uuid=") +
		(origNestedUuid.empty() ? "MISSING (BUG)" : origNestedUuid) +
		" itemCount=" + std::to_string(origNestedItems) + " (expected 1)");

	// Nest it into the scene under test directly (obs_scene_add), not through a bridge
	// method -- sources.addExisting resolves by obs_get_source_by_name, which can't see
	// a canvas-scoped source either. This is fixture setup, not the operation under
	// test; sources.duplicate below still goes through Bridge::Dispatch.
	int64_t nestedItemId = 0;
	{
		OBSSourceAutoRelease parentSceneSrc = g_canvasRuntime->CurrentScene(canvasUuid); // addref'd
		obs_scene_t *parentScene = parentSceneSrc ? obs_scene_from_source(parentSceneSrc) : nullptr;
		obs_sceneitem_t *nestedItem =
			(parentScene && origNestedScene) ? obs_scene_add(parentScene, origNestedScene) : nullptr;
		nestedItemId = nestedItem ? obs_sceneitem_get_id(nestedItem) : 0;
	}
	HostLog(std::string("[selftest] source-duplicate nest into parent scene -> ") +
		(nestedItemId ? "id=" + std::to_string(nestedItemId) : "FAIL (BUG)"));

	json nestedDup = run("sources.duplicate", json{{"canvas", canvasUuid}, {"id", nestedItemId}}, ok);
	const int64_t nestedDupItemId = ok ? nestedDup.value("id", int64_t(0)) : 0;
	const std::string nestedDupSrcName = ok ? nestedDup.value("source", std::string()) : std::string();
	HostLog(std::string("[selftest] source-duplicate sources.duplicate(nested scene) -> ") +
		(ok ? "id=" + std::to_string(nestedDupItemId) + " source='" + nestedDupSrcName + "'" : "FAIL (BUG)"));

	OBSSourceAutoRelease nestedDupScene =
		(testCanvas && !nestedDupSrcName.empty())
			? obs_canvas_get_source_by_name(testCanvas, nestedDupSrcName.c_str())
			: nullptr; // addref'd
	const std::string nestedDupUuid = uuidOfSource(nestedDupScene);
	const bool nestedIndependent = !nestedDupUuid.empty() && nestedDupUuid != origNestedUuid;
	const int nestedDupItems = itemCountOfScene(nestedDupScene);
	HostLog(std::string("[selftest] source-duplicate nested scene copy uuid -> ") +
		(nestedDupUuid.empty() ? "MISSING (BUG)" : nestedDupUuid) +
		"; independent-of-original=" + (nestedIndependent ? "true" : "false (BUG)"));
	HostLog(std::string("[selftest] source-duplicate nested scene copy itemCount=") +
		std::to_string(nestedDupItems) + " (expected " + std::to_string(origNestedItems) +
		"); non-empty=" + ((nestedDupItems > 0 && nestedDupItems == origNestedItems) ? "true" : "false (BUG)"));

	// Clean up: remove the scene items + underlying sources created above, then
	// destroy the temp canvas, returning the in-memory model to baseline. Removing the
	// duplicated scene releases the child copies OBS_SCENE_DUP_COPY minted for it (no
	// separate walk needed: obs_source_remove only marks a source removed, destruction
	// is refcount-driven, and the copy's items hold their only strong ref -- the same
	// reason the scene-duplicate self-test cleans up a duplicated scene-with-child
	// without walking it).
	//
	// Detach `itemId`'s scene item (when nonzero) and remove the source named `name`
	// (when non-empty) -- global-registry or canvas-scoped depending on the source's
	// own namespace, matching the nested-scenes-are-canvas-scoped point above.
	auto removeItemAndSource = [&](int64_t itemId, const std::string &name, bool canvasScoped) {
		if (itemId) {
			run("sceneItems.remove", json{{"canvas", canvasUuid}, {"id", itemId}}, ok);
		}
		if (name.empty()) {
			return;
		}
		OBSSourceAutoRelease s =
			canvasScoped ? (testCanvas ? obs_canvas_get_source_by_name(testCanvas, name.c_str()) : nullptr)
				     : obs_get_source_by_name(name.c_str());
		if (s) {
			obs_source_remove(s);
		}
	};
	removeItemAndSource(wasapiDupItemId, wasapiDupSrcName, false);
	removeItemAndSource(wasapiItemId, wasapiSrcName, false);
	removeItemAndSource(colorDupItemId, colorDupSrcName, false);
	removeItemAndSource(colorItemId, colorSrcName, false);
	removeItemAndSource(nestedDupItemId, nestedDupSrcName, true);
	removeItemAndSource(nestedItemId, std::string(), false);
	removeItemAndSource(0, nestedChildName, false);
	removeItemAndSource(0, kNestedSceneName, true);
	g_multistream->InvalidateCanvasEncoders(canvasUuid);
	g_canvasRuntime->RemoveCanvas(canvasUuid);
	g_canvases.Remove(canvasUuid);
	const bool gone = g_canvasRuntime->Find(canvasUuid) == nullptr && g_canvases.Find(canvasUuid) == nullptr;
	HostLog(std::string("[selftest] source-duplicate cleanup: temp canvas ") +
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
			HostLog("[selftest] " + method + " FAILED: " + Err::Diagnostic(error));
			return json(nullptr);
		}
		return result;
	};

	// Bring up a temporary ADDITIONAL canvas, exactly like the canvas-scene
	// selftest. In-memory only (never Save). The point is to prove, against a
	// real libobs scene item, that setTransform/transformAction rotations pivot
	// around the item's visual center (Task 1) and that an off-canvas transform
	// gets nudged back on-screen (Task 2).
	const std::string canvasUuid = MakeSelfTestCanvas("selftest-transform-pivot-canvas");

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
			HostLog("[selftest] " + method + " FAILED: " + Err::Diagnostic(error));
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
	const std::string canvasUuid = MakeSelfTestCanvas("selftest-rotation-bounds-canvas");

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

// Headless proof for the overlay viewport follow (Overlay::PinItemToBounds /
// PinAllItems / ComputeViewport). Three claims, none of which needs obs-browser loaded
// or the overlay server up: a color source stands in, since everything under test is
// source-type agnostic and a color source has the same OBS_SOURCE_VIDEO deferred-update
// semantics plus its own width/height settings.
//
//   1. The pin is pixel-identical. Converting an OBS_BOUNDS_NONE item to
//      OBS_BOUNDS_STRETCH over the box it already occupies must not move a pixel --
//      including for an item INSIDE A GROUP, where group composition rewrites the item's
//      scale (apply_group_transform) and defers the transform update to the graphics
//      thread. That group case is the one part of the design that could not be settled
//      by reading libobs alone, so it is asserted here rather than argued.
//   2. The per-axis max rule, in CANVAS pixels. Items sharing one source must yield a
//      page covering the widest box on each axis independently, plus that item's crop
//      (libobs takes the crop off the page BEFORE scaling what is left into the box) and
//      times its ancestors' draw scale (a group child's box is in group-local units, so
//      a 2x group makes its 200 px child cover 400 canvas px). The group here is
//      deliberately scaled: at the default 1x the ancestor factor is invisible, so the
//      case would pass whether or not it were applied at all.
//   3. It is a fixpoint. A second pass pins nothing and computes the same numbers, which
//      is what terminates the feedback loop through source_size_changed().
//
// Removes the temp canvas afterward; never Saves. Gated by the caller to the smoke path.
void ObsBootstrap::RunOverlayViewportSelfTest()
{
	using Bridge::json;

	auto run = [](const std::string &method, const json &params, bool &ok) -> json {
		json result;
		std::string error;
		ok = Bridge::Dispatch(method, params, result, error);
		if (!ok) {
			HostLog("[selftest] " + method + " FAILED: " + Err::Diagnostic(error));
			return json(nullptr);
		}
		return result;
	};

	const std::string canvasUuid = MakeSelfTestCanvas("selftest-overlay-viewport-canvas");

	auto teardownCanvas = [&canvasUuid]() {
		g_multistream->InvalidateCanvasEncoders(canvasUuid);
		g_canvasRuntime->RemoveCanvas(canvasUuid);
		g_canvases.Remove(canvasUuid);
		return g_canvasRuntime->Find(canvasUuid) == nullptr && g_canvases.Find(canvasUuid) == nullptr;
	};

	bool ok = false;
	const char *kSceneName = "selftest-overlay-viewport-scene";
	run("scenes.create", json{{"canvas", canvasUuid}, {"name", kSceneName}}, ok);
	run("scenes.setCurrent", json{{"canvas", canvasUuid}, {"name", kSceneName}}, ok);

	obs_source_t *sceneSource = g_canvasRuntime->CurrentScene(canvasUuid); // addref'd
	obs_scene_t *scene = sceneSource ? obs_scene_from_source(sceneSource) : nullptr;
	if (!scene) {
		HostLog("[selftest] overlay-viewport: no scene on the temp canvas (skipped)");
		obs_source_release(sceneSource);
		teardownCanvas();
		return;
	}

	// A group child's transform update is deferred to the graphics thread
	// (do_update_transform), so the box has to be forced settled before it is read or the
	// measurement describes the PREVIOUS state. A no-op for an already-current item.
	auto readBox = [](obs_sceneitem_t *item, vec3 &tl, vec3 &br) {
		obs_sceneitem_force_update_transform(item);
		matrix4 boxTransform;
		obs_sceneitem_get_box_transform(item, &boxTransform);
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
	};
	constexpr float kBoxEpsilonPx = 1.0f;
	auto boxesMatch = [&](const vec3 &aTl, const vec3 &aBr, const vec3 &bTl, const vec3 &bBr) {
		return std::fabs(aTl.x - bTl.x) <= kBoxEpsilonPx && std::fabs(aTl.y - bTl.y) <= kBoxEpsilonPx &&
		       std::fabs(aBr.x - bBr.x) <= kBoxEpsilonPx && std::fabs(aBr.y - bBr.y) <= kBoxEpsilonPx;
	};
	auto boxSize = [](const vec3 &tl, const vec3 &br) {
		return std::to_string(br.x - tl.x) + "x" + std::to_string(br.y - tl.y);
	};

	// One 320x180 source drawn by every item below, so the per-axis max rule has several
	// boxes over a single page to reconcile.
	constexpr uint32_t kSrcW = 320;
	constexpr uint32_t kSrcH = 180;
	OBSDataAutoRelease srcSettings = obs_data_create();
	obs_data_set_int(srcSettings, "width", int64_t(kSrcW));
	obs_data_set_int(srcSettings, "height", int64_t(kSrcH));
	obs_source_t *src =
		obs_source_create("color_source", "selftest-overlay-viewport-src", srcSettings, nullptr); // create-ref
	if (!src) {
		HostLog("[selftest] overlay-viewport: color_source create FAILED (BUG)");
		obs_source_release(sceneSource);
		teardownCanvas();
		return;
	}

	// Size a scene item's box by scale alone, i.e. leave it in OBS_BOUNDS_NONE -- exactly
	// the pre-pin state every already-placed overlay is in today.
	auto placeUnpinned = [&](obs_sceneitem_t *item, float boxW, float boxH, float posX, float posY, float rot) {
		vec2 v;
		vec2_set(&v, posX, posY);
		obs_sceneitem_set_pos(item, &v);
		obs_sceneitem_set_rot(item, rot);
		vec2_set(&v, boxW / float(kSrcW), boxH / float(kSrcH));
		obs_sceneitem_set_scale(item, &v);
	};

	bool allPass = true;

	// --- 1a. pin is pixel-identical, top-level item. Rotated, so that what the pin has
	// to reproduce is the rotation-invariant box and not a rotated AABB. --------------
	obs_sceneitem_t *wide = obs_scene_add(scene, src); // scene takes its own ref
	placeUnpinned(wide, 400.0f, 100.0f, 120.0f, 300.0f, 30.0f);

	vec3 beforeTl, beforeBr, afterTl, afterBr;
	readBox(wide, beforeTl, beforeBr);
	const bool pinnedWide = Overlay::PinItemToBounds(wide);
	readBox(wide, afterTl, afterBr);
	const bool wideIdentical = pinnedWide && boxesMatch(beforeTl, beforeBr, afterTl, afterBr);
	allPass = allPass && wideIdentical;
	HostLog(std::string("[selftest] overlay-viewport pin top-level -> identical=") +
		(wideIdentical ? "true" : "false (BUG)") + " (before=" + boxSize(beforeTl, beforeBr) +
		" after=" + boxSize(afterTl, afterBr) + ")");

	// Idempotent: a second pin reports no change and leaves the item alone.
	const bool pinAgain = Overlay::PinItemToBounds(wide);
	allPass = allPass && !pinAgain;
	HostLog(std::string("[selftest] overlay-viewport pin idempotent -> ") + (!pinAgain ? "true" : "false (BUG)"));

	// --- 1b. pin is pixel-identical for an item INSIDE A GROUP -----------------------
	obs_sceneitem_t *grouped = obs_scene_add(scene, src); // scene takes its own ref
	placeUnpinned(grouped, 100.0f, 200.0f, 40.0f, 60.0f, 0.0f);
	obs_sceneitem_t *group = obs_scene_add_group(scene, "selftest-overlay-viewport-group");
	bool groupIdentical = false;
	if (group) {
		// Moving the item under a group is what switches libobs to the deferred
		// transform path and rewrites the item's scale into group-local space; both
		// readings below are taken after that, so they compare like with like.
		obs_sceneitem_group_add_item(group, grouped);

		vec3 gBeforeTl, gBeforeBr, gAfterTl, gAfterBr;
		readBox(grouped, gBeforeTl, gBeforeBr);
		const bool pinnedGrouped = Overlay::PinItemToBounds(grouped);
		readBox(grouped, gAfterTl, gAfterBr);
		groupIdentical = pinnedGrouped && boxesMatch(gBeforeTl, gBeforeBr, gAfterTl, gAfterBr);
		HostLog(std::string("[selftest] overlay-viewport pin in-group -> identical=") +
			(groupIdentical ? "true" : "false (BUG)") + " (before=" + boxSize(gBeforeTl, gBeforeBr) +
			" after=" + boxSize(gAfterTl, gAfterBr) + ")");

		// Now scale the group 2x, so the child's 100x200 GROUP-LOCAL box covers 200x400
		// CANVAS px. Done after the pin comparison above so that check still measures a
		// like-for-like group-local box, and before the max rule below so the ancestor
		// factor is the thing that decides the expected height.
		vec2 groupScale;
		vec2_set(&groupScale, 2.0f, 2.0f);
		obs_sceneitem_set_scale(group, &groupScale);
	} else {
		HostLog("[selftest] overlay-viewport group create FAILED (BUG)");
	}
	allPass = allPass && groupIdentical;

	// --- 2. per-axis max over three items sharing one source, in canvas pixels --------
	// wide is 400x100 with a 12px left crop, so its page has to be 12px wider still for
	// the visible remainder to fill the same box -> 412 wide, and it owns the width.
	// tall is 240x300 and still UNPINNED, so PinAllItems has real work to do here.
	// grouped is 100x200 inside the 2x group -> 200x400 on canvas, so it owns the
	// height, and only at 400 (not 200) is the ancestor scale actually being applied.
	obs_sceneitem_t *tall = obs_scene_add(scene, src); // scene takes its own ref
	placeUnpinned(tall, 240.0f, 300.0f, 600.0f, 40.0f, 0.0f);

	obs_sceneitem_crop crop = {};
	crop.left = 12;
	obs_sceneitem_set_crop(wide, &crop);

	const bool pinnedRest = Overlay::PinAllItems(src);
	uint32_t vw = 0, vh = 0;
	const bool computed = Overlay::ComputeViewport(src, vw, vh);
	const bool maxRule = pinnedRest && computed && vw == 412u && vh == 400u;
	allPass = allPass && maxRule;
	HostLog(std::string("[selftest] overlay-viewport max rule -> ") + (maxRule ? "true" : "false (BUG)") +
		" (want 412x400, got " + (computed ? std::to_string(vw) + "x" + std::to_string(vh) : "none") +
		", pinned-remaining=" + (pinnedRest ? "true" : "false") + ")");

	// --- 3. fixpoint: nothing left to pin and the same numbers come back --------------
	const bool pinnedTwice = Overlay::PinAllItems(src);
	uint32_t vw2 = 0, vh2 = 0;
	const bool computed2 = Overlay::ComputeViewport(src, vw2, vh2);
	const bool fixpoint = !pinnedTwice && computed2 && vw2 == vw && vh2 == vh;
	allPass = allPass && fixpoint;
	HostLog(std::string("[selftest] overlay-viewport fixpoint -> ") + (fixpoint ? "true" : "false (BUG)") +
		" (second pass " + (computed2 ? std::to_string(vw2) + "x" + std::to_string(vh2) : "none") + ")");

	// --- cleanup: items, group, source, temp canvas ----------------------------------
	obs_sceneitem_remove(tall);
	obs_sceneitem_remove(grouped);
	if (group) {
		obs_sceneitem_remove(group);
	}
	obs_sceneitem_remove(wide);
	obs_source_remove(src);
	obs_source_release(src);
	obs_source_release(sceneSource);
	const bool gone = teardownCanvas();
	HostLog(std::string("[selftest] overlay-viewport cleanup: temp canvas ") +
		(gone ? "removed" : "STILL PRESENT (BUG)"));
	HostLog(std::string("[selftest] overlay-viewport overall -> ") + (allPass ? "PASS" : "FAIL (BUG)"));
}

// Headless proof that a group's child is addressable and undoable as {id, group}. The
// group's children are added straight into the group's own scene, whose id counter is
// independent of the top-level scene's, so the first child and the first top-level item
// both get id 1: every bare-id route that could reach the wrong item is live here.
// Positions are read after obs_scene_prune_sources, which runs the same transform update
// and group re-fit a render does, so each reading is the settled state.
//
// Removes the temp canvas afterward; never Saves. Gated by the caller to the smoke path.
void ObsBootstrap::RunSceneItemGroupSelfTest()
{
	using Bridge::json;

	auto run = [](const std::string &method, const json &params, bool &ok) -> json {
		json result;
		std::string error;
		ok = Bridge::Dispatch(method, params, result, error);
		if (!ok) {
			HostLog("[selftest] " + method + " FAILED: " + Err::Diagnostic(error));
			return json(nullptr);
		}
		return result;
	};

	const std::string canvasUuid = MakeSelfTestCanvas("selftest-scene-item-group-canvas");
	auto teardownCanvas = [&canvasUuid]() {
		g_multistream->InvalidateCanvasEncoders(canvasUuid);
		g_canvasRuntime->RemoveCanvas(canvasUuid);
		g_canvases.Remove(canvasUuid);
		return g_canvasRuntime->Find(canvasUuid) == nullptr && g_canvases.Find(canvasUuid) == nullptr;
	};

	bool ok = false;
	const char *kSceneName = "selftest-scene-item-group-scene";
	run("scenes.create", json{{"canvas", canvasUuid}, {"name", kSceneName}}, ok);
	run("scenes.setCurrent", json{{"canvas", canvasUuid}, {"name", kSceneName}}, ok);

	obs_source_t *sceneSource = g_canvasRuntime->CurrentScene(canvasUuid); // addref'd
	obs_scene_t *scene = sceneSource ? obs_scene_from_source(sceneSource) : nullptr;
	if (!scene) {
		HostLog("[selftest] scene-item-group: no scene on the temp canvas (skipped)");
		obs_source_release(sceneSource);
		teardownCanvas();
		return;
	}

	bool allPass = true;
	auto check = [&allPass](const std::string &label, bool pass, const std::string &detail) {
		allPass = allPass && pass;
		HostLog("[selftest] scene-item-group " + label + " -> " + (pass ? "OK" : "MISMATCH") +
			(detail.empty() ? std::string() : " (" + detail + ")"));
	};
	auto settle = [scene]() {
		obs_scene_prune_sources(scene);
	};
	auto posOf = [](obs_sceneitem_t *item) {
		vec2 pos;
		obs_sceneitem_get_pos(item, &pos);
		return pos;
	};
	auto posText = [](const vec2 &p) {
		return std::to_string(p.x) + "," + std::to_string(p.y);
	};
	auto samePos = [](const vec2 &a, const vec2 &b) {
		return std::fabs(a.x - b.x) <= 0.01f && std::fabs(a.y - b.y) <= 0.01f;
	};
	auto setPos = [](obs_sceneitem_t *item, float x, float y) {
		vec2 pos;
		vec2_set(&pos, x, y);
		obs_sceneitem_set_pos(item, &pos);
	};
	auto makeSource = [](const char *name) {
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_int(settings, "width", 100);
		obs_data_set_int(settings, "height", 100);
		return obs_source_create("color_source", name, settings, nullptr); // create-ref
	};
	auto undoState = []() {
		return ObsBootstrap::Undo().GetState();
	};
	// Whether the undo stack reads exactly as `before` did: an action that records nothing
	// must neither add an entry nor spend the redo one.
	auto undoUnchanged = [&undoState](const UndoManager::State &before) {
		const UndoManager::State now = undoState();
		return now.canUndo == before.canUndo && now.canRedo == before.canRedo &&
		       now.undoName == before.undoName && now.redoName == before.redoName;
	};
	// UndoManager can only be emptied, not rolled back, so the stack is restored only when
	// the test started from an empty one.
	const UndoManager::State undoAtStart = undoState();

	std::vector<std::pair<std::string, std::string>> events;
	std::vector<json> selections;
	Bridge::SetEventObserver([&events, &selections](const std::string &name, const std::string &payload) {
		if (name == EventNames::kSceneItemsChanged) {
			events.emplace_back(name, payload);
		} else if (name == EventNames::kSceneItemSelected) {
			selections.push_back(json::parse(payload, nullptr, false));
		}
	});

	obs_source_t *topSrc = makeSource("selftest-scene-item-group-top");
	obs_source_t *childASrc = makeSource("selftest-scene-item-group-child-a");
	obs_source_t *childBSrc = makeSource("selftest-scene-item-group-child-b");

	obs_sceneitem_t *top = topSrc ? obs_scene_add(scene, topSrc) : nullptr;
	json groupCreated = run("sceneItems.createGroup",
				json{{"canvas", canvasUuid}, {"name", "selftest-scene-item-group-group"}}, ok);
	const int64_t groupId = ok ? groupCreated.value("id", int64_t(0)) : 0;
	const std::string groupUuid = ok ? groupCreated.value("source", std::string()) : std::string();
	obs_sceneitem_t *groupItem = groupId ? obs_scene_find_sceneitem_by_id(scene, groupId) : nullptr;
	obs_source_t *groupSrc = groupItem ? obs_sceneitem_get_source(groupItem) : nullptr; // borrowed
	obs_scene_t *groupScene = groupSrc ? obs_group_from_source(groupSrc) : nullptr;
	obs_sceneitem_t *childA = groupScene && childASrc ? obs_scene_add(groupScene, childASrc) : nullptr;
	obs_sceneitem_t *childB = groupScene && childBSrc ? obs_scene_add(groupScene, childBSrc) : nullptr;
	if (!top || !groupItem || !childA || !childB) {
		HostLog("[selftest] scene-item-group setup FAILED (BUG)");
		allPass = false;
	} else {
		setPos(top, 600.0f, 400.0f);
		setPos(groupItem, 100.0f, 100.0f);
		setPos(childA, 0.0f, 0.0f);
		setPos(childB, 200.0f, 0.0f);
		settle();

		const int64_t topId = obs_sceneitem_get_id(top);
		int64_t childAId = obs_sceneitem_get_id(childA);
		const int64_t childBId = obs_sceneitem_get_id(childB);
		check("id collision set up", childAId == topId,
		      "child " + std::to_string(childAId) + ", top-level " + std::to_string(topId));
		const json base{{"canvas", canvasUuid}, {"scene", kSceneName}};
		auto childParams = [&](int64_t id) {
			json p = base;
			p["id"] = id;
			p["group"] = groupUuid;
			return p;
		};
		auto topParams = [&](int64_t id) {
			json p = base;
			p["id"] = id;
			return p;
		};

		// --- 1. sceneItems.list: group rows carry children (top-first) + collapsed ------
		auto findRow = [](const json &rows, int64_t id) -> const json * {
			for (const json &row : rows) {
				if (row.value("id", int64_t(-1)) == id) {
					return &row;
				}
			}
			return nullptr;
		};
		json rows = run("sceneItems.list", base, ok);
		const json *groupRow = ok ? findRow(rows, groupId) : nullptr;
		const json *topRow = ok ? findRow(rows, topId) : nullptr;
		const json *children = groupRow && groupRow->contains("children") ? &groupRow->at("children") : nullptr;
		const bool childrenOk = children && children->is_array() && children->size() == 2 &&
					(*children)[0].value("id", int64_t(-1)) == childBId &&
					(*children)[1].value("id", int64_t(-1)) == childAId &&
					JsonUtil::Str((*children)[0], "group") == groupUuid &&
					JsonUtil::Str((*children)[1], "group") == groupUuid;
		auto topLevelOwner = [](const json *row) {
			return row && row->contains("group") && row->at("group").is_null();
		};
		const bool ownersOk = topLevelOwner(groupRow) && topLevelOwner(topRow) &&
				      !topRow->contains("children") && !topRow->contains("collapsed");
		const bool collapsedFalse = groupRow && groupRow->value("collapsed", true) == false;
		check("list children top-first", childrenOk, groupRow ? groupRow->dump() : "no group row");
		check("list owners", ownersOk, "");
		{
			OBSDataAutoRelease priv = obs_sceneitem_get_private_settings(groupItem);
			obs_data_set_bool(priv, "collapsed", true);
			json collapsedRows = run("sceneItems.list", base, ok);
			const json *collapsedRow = ok ? findRow(collapsedRows, groupId) : nullptr;
			check("list collapsed",
			      collapsedFalse && collapsedRow && collapsedRow->value("collapsed", false), "");
			obs_data_erase(priv, "collapsed");
		}
		{
			// Round-trips both values, so a setter that always writes true, or writes a key list
			// does not read, fails; and records no undo entry.
			const UndoManager::State undoBeforeCollapse = undoState();
			auto listedCollapsed = [&](bool fallback) {
				bool listOk = false;
				json listed = run("sceneItems.list", base, listOk);
				const json *row = listOk ? findRow(listed, groupId) : nullptr;
				return row ? row->value("collapsed", fallback) : fallback;
			};
			json collapseParams = topParams(groupId);
			collapseParams["collapsed"] = true;
			events.clear();
			bool collapseOk = false;
			const json collapseResult = run("sceneItems.setCollapsed", collapseParams, collapseOk);
			const bool listedOn = listedCollapsed(false);
			const bool announced = !events.empty();
			collapseParams["collapsed"] = false;
			bool expandOk = false;
			run("sceneItems.setCollapsed", collapseParams, expandOk);
			const bool listedOff = !listedCollapsed(true);
			check("setCollapsed round-trips through list without an undo entry",
			      collapseOk && collapseResult.is_object() && collapseResult.empty() && listedOn &&
				      announced && expandOk && listedOff && undoUnchanged(undoBeforeCollapse),
			      std::string("on ") + (listedOn ? "yes" : "no") + ", off " + (listedOff ? "yes" : "no") +
				      ", event " + (announced ? "yes" : "no"));

			// Child B's id is the group's own id, so a setter that ignored `group` would collapse
			// the group rather than refuse the child.
			auto collapseRefused = [&](const json &params) {
				json unusedResult;
				std::string refusal;
				return !Bridge::Dispatch("sceneItems.setCollapsed", params, unusedResult, refusal);
			};
			json topCollapse = topParams(topId);
			topCollapse["collapsed"] = true;
			json childCollapse = childParams(childBId);
			childCollapse["collapsed"] = true;
			const bool refusals = collapseRefused(topCollapse) && collapseRefused(childCollapse) &&
					      collapseRefused(topParams(groupId));
			OBSDataAutoRelease groupPriv = obs_sceneitem_get_private_settings(groupItem);
			OBSDataAutoRelease topPriv = obs_sceneitem_get_private_settings(top);
			OBSDataAutoRelease childBPriv = obs_sceneitem_get_private_settings(childB);
			check("setCollapsed refuses a non-group, a child sharing the group's id and a missing flag",
			      refusals && !obs_data_get_bool(groupPriv, "collapsed") &&
				      !obs_data_has_user_value(topPriv, "collapsed") &&
				      !obs_data_has_user_value(childBPriv, "collapsed"),
			      "");
			obs_data_erase(groupPriv, "collapsed");
		}

		// --- 2. the colliding id reaches each item only through its own owner ----------
		auto xOf = [](const json &xform) {
			return xform.is_object() && xform.contains("pos") ? xform.at("pos").value("x", -1.0) : -1.0;
		};
		json childXform = run("sceneItems.getTransform", childParams(childAId), ok);
		const bool childAddressed = ok && xOf(childXform) == 0.0;
		json topXform = run("sceneItems.getTransform", topParams(topId), ok);
		const bool topAddressed = ok && xOf(topXform) == 600.0;
		check("addressing by owner", childAddressed && topAddressed,
		      "child x=" + std::to_string(xOf(childXform)) + ", top-level x=" + std::to_string(xOf(topXform)));
		json wrongGroup = childParams(childAId);
		wrongGroup["group"] = "00000000-0000-0000-0000-000000000000";
		json unused;
		std::string wrongError;
		check("unknown group refused",
		      !Bridge::Dispatch("sceneItems.getTransform", wrongGroup, unused, wrongError), wrongError);

		// preview.select by refs. Child A and the top-level item share an id, so a selection
		// keyed by id alone collapses them into one member; one that does not walk into groups
		// leaves the child's libobs flag clear.
		if (Preview::Instance()) {
			const json topSelRef{{"id", topId}, {"group", nullptr}};
			auto childSelRef = [&](int64_t id) {
				return json{{"id", id}, {"group", groupUuid}};
			};
			auto selectRefs = [&](const json &refs, bool &selectOk) {
				json p = base;
				p["refs"] = refs;
				selections.clear();
				return run("preview.select", p, selectOk);
			};
			auto lastSelection = [&]() {
				return selections.empty() ? json() : selections.back();
			};
			auto selectionText = [&](const json &reply) {
				return "reply " + reply.dump() + ", event " + lastSelection().dump();
			};
			const json idsOfMixed = json::array({childAId, topId});

			bool selectOk = false;
			const json mixedRefs = json::array({childSelRef(childAId), topSelRef});
			json reply = selectRefs(mixedRefs, selectOk);
			json event = lastSelection();
			check("preview.select keeps a child and a top-level item sharing its id, focus last",
			      selectOk && reply.value("selectedRefs", json()) == mixedRefs &&
				      reply.value("selectedIds", json()) == idsOfMixed &&
				      reply.value("selected", int64_t(-1)) == topId && event.is_object() &&
				      event.value("refs", json()) == mixedRefs &&
				      event.value("ids", json()) == idsOfMixed &&
				      event.value("id", int64_t(-1)) == topId && event.contains("group") &&
				      event.at("group").is_null(),
			      selectionText(reply));
			check("preview.select mirrors a child's selection into libobs",
			      obs_sceneitem_selected(childA) && obs_sceneitem_selected(top) &&
				      !obs_sceneitem_selected(childB) && !obs_sceneitem_selected(groupItem),
			      "");

			// Reversed, the focus is the child: only `group` tells it from the top-level item.
			const json childFocusRefs = json::array({topSelRef, childSelRef(childAId)});
			reply = selectRefs(childFocusRefs, selectOk);
			event = lastSelection();
			check("preview.select names a child focus's group",
			      selectOk && reply.value("selectedRefs", json()) == childFocusRefs && event.is_object() &&
				      event.value("refs", json()) == childFocusRefs &&
				      JsonUtil::Str(event, "group") == groupUuid,
			      selectionText(reply));

			// A repeat keeps its LAST position; keeping the first would echo the input order.
			reply = selectRefs(json::array({childSelRef(childAId), topSelRef, childSelRef(childAId)}),
					   selectOk);
			check("preview.select dedupes a repeat to its last position",
			      selectOk && reply.value("selectedRefs", json()) == childFocusRefs &&
				      lastSelection().value("refs", json()) == childFocusRefs,
			      selectionText(reply));

			// A child that names no item, or a group that is not in the scene, is dropped
			// rather than echoed.
			const json missingGroupRef{{"id", childAId}, {"group", "00000000-0000-0000-0000-000000000000"}};
			reply = selectRefs(json::array({childSelRef(987654), missingGroupRef, topSelRef}), selectOk);
			check("preview.select drops children that do not resolve",
			      selectOk && reply.value("selectedRefs", json()) == json::array({topSelRef}) &&
				      lastSelection().value("refs", json()) == json::array({topSelRef}) &&
				      !obs_sceneitem_selected(childA),
			      selectionText(reply));

			json refsOverIds = base;
			refsOverIds["refs"] = json::array({childSelRef(childBId)});
			refsOverIds["ids"] = json::array({topId});
			reply = run("preview.select", refsOverIds, selectOk);
			check("preview.select prefers refs over ids",
			      selectOk && reply.value("selectedRefs", json()) == json::array({childSelRef(childBId)}),
			      reply.dump());

			std::string badRefs;
			for (const json &bad :
			     {json::array({json{{"id", -1}}}), json::array({json{{"id", topId}, {"group", 5}}}),
			      json::array({json("x")}), json{{"id", topId}}, json("refs")}) {
				json p = base;
				p["refs"] = bad;
				json badResult;
				std::string badError;
				if (Bridge::Dispatch("preview.select", p, badResult, badError)) {
					badRefs += bad.dump() + " accepted; ";
				}
			}
			check("preview.select refuses malformed refs", badRefs.empty(), badRefs);
			run("preview.select", base, selectOk);
		} else {
			HostLog("[selftest] scene-item-group preview.select: no preview manager (skipped)");
		}

		// --- 3. a child's change is announced for the scene the dock lists -------------
		events.clear();
		json lockParams = childParams(childAId);
		lockParams["locked"] = true;
		run("sceneItems.setLocked", lockParams, ok);
		const bool lockedChild = ok && obs_sceneitem_locked(childA) && !obs_sceneitem_locked(top);
		auto listedSceneEvent = [&]() {
			bool seen = false;
			for (const auto &event : events) {
				json payload = json::parse(event.second, nullptr, false);
				seen = seen || (JsonUtil::Str(payload, "scene") == kSceneName &&
						JsonUtil::Str(payload, "canvas") == canvasUuid);
			}
			return seen;
		};
		auto eventsText = [&]() {
			return std::to_string(events.size()) + " event(s)" +
			       (events.empty() ? std::string() : ", first " + events.front().second);
		};
		check("child lock hits the child only", lockedChild, "");
		check("change event names the listed scene", listedSceneEvent(), eventsText());
		lockParams["locked"] = false;
		run("sceneItems.setLocked", lockParams, ok);

		// Child B's id is the group's own id, so a bare id would rename the group.
		const std::string groupNameBeforeRename = obs_source_get_name(groupSrc);
		const std::string childBName = obs_source_get_name(childBSrc);
		events.clear();
		json renameParams = childParams(childBId);
		renameParams["name"] = "selftest-scene-item-group-child-b-renamed";
		run("sources.rename", renameParams, ok);
		check("child rename by owner",
		      ok && childBId == groupId &&
			      std::string(obs_source_get_name(childBSrc)) ==
				      "selftest-scene-item-group-child-b-renamed" &&
			      std::string(obs_source_get_name(groupSrc)) == groupNameBeforeRename,
		      std::string("child '") + obs_source_get_name(childBSrc) + "', group '" +
			      obs_source_get_name(groupSrc) + "'");
		check("child rename event names the listed scene", listedSceneEvent(), eventsText());
		ObsBootstrap::Undo().Undo();
		check("child rename undo", std::string(obs_source_get_name(childBSrc)) == childBName,
		      obs_source_get_name(childBSrc));

		// --- 4. child transform, undo, redo: the whole group comes back ----------------
		const vec2 groupBefore = posOf(groupItem), aBefore = posOf(childA), bBefore = posOf(childB);
		const vec2 topBefore = posOf(top);
		json moveParams = childParams(childAId);
		moveParams["transform"] = json{{"pos", json{{"x", -50.0}, {"y", 0.0}}}};
		run("sceneItems.setTransform", moveParams, ok);
		settle();
		const vec2 groupMoved = posOf(groupItem), aMoved = posOf(childA), bMoved = posOf(childB);
		// The re-fit is what makes a child-only undo land wrong; without it this case would
		// pass whether or not the group travels with the child.
		vec2 refitGroup, refitA, refitB;
		vec2_set(&refitGroup, groupBefore.x - 50.0f, groupBefore.y);
		vec2_set(&refitA, 0.0f, 0.0f);
		vec2_set(&refitB, bBefore.x + 50.0f, bBefore.y);
		check("child move re-fits the group",
		      ok && samePos(groupMoved, refitGroup) && samePos(aMoved, refitA) && samePos(bMoved, refitB),
		      "group " + posText(groupMoved) + " a " + posText(aMoved) + " b " + posText(bMoved));

		const std::string moveLabel = undoState().undoName;
		ObsBootstrap::Undo().Undo();
		settle();
		check("child undo restores group + siblings",
		      samePos(posOf(groupItem), groupBefore) && samePos(posOf(childA), aBefore) &&
			      samePos(posOf(childB), bBefore) && samePos(posOf(top), topBefore),
		      "group " + posText(posOf(groupItem)) + " a " + posText(posOf(childA)) + " b " +
			      posText(posOf(childB)) + " top " + posText(posOf(top)));

		ObsBootstrap::Undo().Redo();
		settle();
		check("child redo reapplies",
		      samePos(posOf(groupItem), groupMoved) && samePos(posOf(childA), aMoved) &&
			      samePos(posOf(childB), bMoved) && samePos(posOf(top), topBefore),
		      "group " + posText(posOf(groupItem)) + " a " + posText(posOf(childA)) + " b " +
			      posText(posOf(childB)));

		// --- 5. undo after renaming the group: the entry keys on the uuid, not the name --
		const std::string groupName = obs_source_get_name(groupSrc);
		obs_source_set_name(groupSrc, "selftest-scene-item-group-renamed");
		ObsBootstrap::Undo().Undo();
		settle();
		check("child undo after group rename",
		      samePos(posOf(groupItem), groupBefore) && samePos(posOf(childA), aBefore) &&
			      samePos(posOf(childB), bBefore),
		      "group " + posText(posOf(groupItem)) + " a " + posText(posOf(childA)) + " b " +
			      posText(posOf(childB)));
		obs_source_set_name(groupSrc, groupName.c_str());

		// --- 6. remove a child, then undo: it returns to its place on the canvas ----------
		// Removing the child that defines the group's frame lets any later re-fit move that
		// frame, so a child re-added at its group-space position alone lands elsewhere.
		auto canvasPosOf = [&](obs_sceneitem_t *child) {
			vec2 pos = posOf(child);
			const vec2 group = posOf(groupItem);
			vec2_add(&pos, &pos, &group);
			return pos;
		};
		auto childNamed = [groupScene](const char *name) {
			return obs_scene_find_source(groupScene, name); // borrowed or null
		};
		const char *kChildAName = "selftest-scene-item-group-child-a";
		const vec2 aCanvas = canvasPosOf(childA), bCanvas = canvasPosOf(childB);
		auto canvasText = [&]() {
			obs_sceneitem_t *a = childNamed(kChildAName);
			return "group " + posText(posOf(groupItem)) + " a " + (a ? posText(canvasPosOf(a)) : "gone") +
			       " b " + posText(canvasPosOf(childB)) + " (canvas)";
		};
		run("sceneItems.remove", childParams(childAId), ok);
		settle();
		json moveB = childParams(childBId);
		moveB["transform"] = json{{"pos", json{{"x", bBefore.x + 50.0}, {"y", bBefore.y}}}};
		run("sceneItems.setTransform", moveB, ok);
		settle();
		ObsBootstrap::Undo().Undo();
		settle();
		vec2 origin;
		vec2_zero(&origin);
		check("child remove precondition: the group re-fits around the survivor",
		      ok && !childNamed(kChildAName) && samePos(posOf(groupItem), bCanvas) &&
			      samePos(posOf(childB), origin),
		      canvasText());
		ObsBootstrap::Undo().Undo();
		settle();
		childA = childNamed(kChildAName);
		check("child remove undo restores it in place",
		      childA && samePos(canvasPosOf(childA), aCanvas) && samePos(canvasPosOf(childB), bCanvas),
		      canvasText());
		ObsBootstrap::Undo().Redo();
		settle();
		check("child remove redo leaves the sibling in place",
		      !childNamed(kChildAName) && samePos(canvasPosOf(childB), bCanvas), canvasText());
		ObsBootstrap::Undo().Undo();
		settle();
		childA = childNamed(kChildAName);
		childAId = childA ? obs_sceneitem_get_id(childA) : 0;
		check("child remove undo after redo",
		      childA && samePos(posOf(groupItem), groupBefore) && samePos(posOf(childA), aBefore) &&
			      samePos(posOf(childB), bBefore),
		      canvasText());

		// --- 7. duplicate a child: the copy joins the group, undo/redo keep siblings -------
		if (childA) {
			json duplicated = run("sources.duplicate", childParams(childAId), ok);
			const std::string copyName = ok ? JsonUtil::Str(duplicated, "source") : std::string();
			settle();
			auto copyText = [&]() {
				obs_sceneitem_t *copy = childNamed(copyName.c_str());
				return "copy " + (copy ? posText(canvasPosOf(copy)) : std::string("gone")) + ", " +
				       canvasText();
			};
			auto siblingsInPlace = [&]() {
				return samePos(canvasPosOf(childA), aCanvas) && samePos(canvasPosOf(childB), bCanvas);
			};
			obs_sceneitem_t *copy = copyName.empty() ? nullptr : childNamed(copyName.c_str());
			check("child duplicate lands in the group over the original",
			      copy && samePos(canvasPosOf(copy), aCanvas) && siblingsInPlace(), copyText());
			ObsBootstrap::Undo().Undo();
			settle();
			check("child duplicate undo removes the copy only",
			      !childNamed(copyName.c_str()) && siblingsInPlace(), copyText());
			ObsBootstrap::Undo().Redo();
			settle();
			copy = childNamed(copyName.c_str());
			check("child duplicate redo restores the copy in place",
			      copy && samePos(canvasPosOf(copy), aCanvas) && siblingsInPlace(), copyText());
			ObsBootstrap::Undo().Undo();
			settle();
		} else {
			check("child duplicate", false, "child a did not come back");
		}

		// --- 8. canvas-space actions place a child as the canvas shows it -----------------
		const CanvasDefinition *groupCanvasDef = g_canvases.Find(canvasUuid);
		const float canvasWidth = groupCanvasDef ? float(groupCanvasDef->width) : 0.0f;
		const float canvasHeight = groupCanvasDef ? float(groupCanvasDef->height) : 0.0f;
		auto setGroupTransform = [&](float rot, float sx, float sy) {
			obs_sceneitem_set_rot(groupItem, rot);
			vec2 groupScaleNow;
			vec2_set(&groupScaleNow, sx, sy);
			obs_sceneitem_set_scale(groupItem, &groupScaleNow);
			settle();
		};
		auto within = [](float a, float b, float tolerance) {
			return std::fabs(a - b) <= tolerance;
		};
		// The canvas-space extent of an item's box, through SceneItems::ItemBoxToCanvas, which the
		// first case below checks against corners computed without it.
		auto canvasBoxOf = [&](obs_sceneitem_t *item, vec3 &tl, vec3 &br) {
			matrix4 box;
			const bool found = SceneItems::ItemBoxToCanvas(item, box, scene);
			SceneItems::BoxExtent(box, tl, br);
			return found;
		};
		auto boxText = [](const vec3 &tl, const vec3 &br) {
			return "(" + std::to_string(tl.x) + "," + std::to_string(tl.y) + ")-(" + std::to_string(br.x) +
			       "," + std::to_string(br.y) + ")";
		};
		auto childrenAsBefore = [&]() {
			vec2 bScale;
			obs_sceneitem_get_scale(childB, &bScale);
			return samePos(posOf(childA), aBefore) && samePos(posOf(childB), bBefore) &&
			       obs_sceneitem_get_rot(childB) == 0.0f && bScale.x == 1.0f && bScale.y == 1.0f &&
			       obs_sceneitem_get_bounds_type(childB) == OBS_BOUNDS_NONE;
		};
		auto groupAsBefore = [&]() {
			return samePos(posOf(groupItem), groupBefore) && childrenAsBefore();
		};
		auto actionOnB = [&](const char *action, bool &actionOk) {
			json p = childParams(childBId);
			p["action"] = action;
			run("sceneItems.transformAction", p, actionOk);
			settle();
		};
		auto undoAndSettle = [&]() {
			ObsBootstrap::Undo().Undo();
			settle();
		};
		const uint32_t topLeft = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
		auto turn = [](float x, float y, float radians) {
			vec2 out;
			vec2_set(&out, std::cos(radians) * x - std::sin(radians) * y,
				 std::sin(radians) * x + std::cos(radians) * y);
			return out;
		};
		// Where the point (u, v) of a child's unit box lands on the canvas, composed from the
		// child's and the group's own pos, rot and scale and the group's crop, with no matrix
		// libobs built. A cropped group draws its content shifted by the crop, so the crop comes
		// off the child's group-space point before the group's scale and turn. Both items are
		// top-left aligned and unbounded, which the callers check.
		auto expectedCanvasPoint = [&](obs_sceneitem_t *child, float u, float v) {
			vec2 gScale, cScale;
			obs_sceneitem_get_scale(groupItem, &gScale);
			obs_sceneitem_get_scale(child, &cScale);
			obs_sceneitem_crop crop;
			obs_sceneitem_get_crop(groupItem, &crop);
			obs_source_t *childSource = obs_sceneitem_get_source(child);
			vec2 inGroup = turn(u * float(obs_source_get_width(childSource)) * cScale.x,
					    v * float(obs_source_get_height(childSource)) * cScale.y,
					    RAD(obs_sceneitem_get_rot(child)));
			const vec2 cPos = posOf(child);
			vec2_add(&inGroup, &inGroup, &cPos);
			vec2 expected = turn((inGroup.x - float(crop.left)) * gScale.x,
					     (inGroup.y - float(crop.top)) * gScale.y,
					     RAD(obs_sceneitem_get_rot(groupItem)));
			const vec2 gPos = posOf(groupItem);
			vec2_add(&expected, &expected, &gPos);
			return expected;
		};
		auto topLeftAligned = [&](obs_sceneitem_t *child) {
			return obs_sceneitem_get_alignment(child) == topLeft &&
			       obs_sceneitem_get_alignment(groupItem) == topLeft &&
			       obs_sceneitem_get_bounds_type(child) == OBS_BOUNDS_NONE &&
			       obs_sceneitem_get_bounds_type(groupItem) == OBS_BOUNDS_NONE;
		};
		// Every corner of `child`'s box, read through SceneItems::ItemBoxToCanvas both with the
		// scene and by the canvas walk, against expectedCanvasPoint. `text` collects each pair.
		auto cornersAsComposed = [&](obs_sceneitem_t *child, std::string &text) {
			matrix4 viaScene, viaCanvas;
			bool match = topLeftAligned(child) && SceneItems::ItemBoxToCanvas(child, viaScene, scene) &&
				     SceneItems::ItemBoxToCanvas(child, viaCanvas);
			for (float u : {0.0f, 1.0f}) {
				for (float v : {0.0f, 1.0f}) {
					const vec2 expected = expectedCanvasPoint(child, u, v);
					for (const matrix4 *m : {&viaScene, &viaCanvas}) {
						vec3 corner;
						vec3_set(&corner, u, v, 0.0f);
						vec3_transform(&corner, &corner, m);
						match = match && within(corner.x, expected.x, 0.05f) &&
							within(corner.y, expected.y, 0.05f);
						text += "(" + std::to_string(corner.x) + "," +
							std::to_string(corner.y) + " vs " + posText(expected) + ") ";
					}
				}
			}
			return match;
		};

		// The corners are composed here from the items' own pos, rot and scale, with no matrix
		// libobs built: a box read without its group, or composed in the wrong order, or without
		// the mirror, lands elsewhere. Child A is turned and scaled under an identity group, so
		// the group's re-fit runs exactly, and the group is then turned, scaled unevenly and
		// mirrored.
		{
			obs_sceneitem_set_rot(childA, 15.0f);
			vec2 childScale;
			vec2_set(&childScale, 1.2f, 0.8f);
			obs_sceneitem_set_scale(childA, &childScale);
			settle();
			setGroupTransform(30.0f, -1.5f, 0.75f);
			std::string cornerText;
			const bool cornersMatch = cornersAsComposed(childA, cornerText);
			check("ItemBoxToCanvas carries a child through a rotated, scaled, mirrored group", cornersMatch,
			      cornerText);
			setGroupTransform(0.0f, 1.0f, 1.0f);
			obs_sceneitem_set_rot(childA, 0.0f);
			vec2_set(&childScale, 1.0f, 1.0f);
			obs_sceneitem_set_scale(childA, &childScale);
			settle();
			check("ItemBoxToCanvas case leaves the group as it was", groupAsBefore(), canvasText());
		}

		// A cropped group: the outline and a centred child both come from the crop-shifted map,
		// checked here against the composition rather than against that map itself, which would
		// agree with a map that left the crop out.
		{
			setGroupTransform(30.0f, 1.5f, 0.75f);
			obs_sceneitem_crop groupCrop{};
			groupCrop.left = 20;
			groupCrop.top = 10;
			obs_sceneitem_set_crop(groupItem, &groupCrop);
			settle();
			std::string cornerText;
			const bool cornersMatch = cornersAsComposed(childA, cornerText);
			check("ItemBoxToCanvas carries a child through a cropped group", cornersMatch, cornerText);

			const vec2 aCorner = expectedCanvasPoint(childA, 0.0f, 0.0f);
			const vec2 bFrom = expectedCanvasPoint(childB, 0.5f, 0.5f);
			bool actionOk = false;
			actionOnB("center", actionOk);
			const vec2 bCentre = expectedCanvasPoint(childB, 0.5f, 0.5f);
			const vec2 aCornerAfter = expectedCanvasPoint(childA, 0.0f, 0.0f);
			check("center places a child against the canvas through a cropped group",
			      actionOk && topLeftAligned(childB) && obs_sceneitem_get_rot(childB) == 0.0f &&
				      !within(bFrom.x, canvasWidth * 0.5f, 1.0f) &&
				      within(bCentre.x, canvasWidth * 0.5f, 0.1f) &&
				      within(bCentre.y, canvasHeight * 0.5f, 0.1f) && samePos(aCornerAfter, aCorner),
			      "child b centre " + posText(bFrom) + " -> " + posText(bCentre) + ", sibling corner " +
				      posText(aCorner) + " -> " + posText(aCornerAfter));
			undoAndSettle();
			check("center through a cropped group undoes in one step", groupAsBefore(), canvasText());
			obs_sceneitem_crop noCrop{};
			obs_sceneitem_set_crop(groupItem, &noCrop);
			setGroupTransform(0.0f, 1.0f, 1.0f);
			check("cropped-group case leaves the group as it was", groupAsBefore(), canvasText());
		}

		// A group anchored anywhere but its top-left corner puts that anchor at a whole pixel of
		// its ceiled extent (add_alignment and resize_scene_base in obs-scene.c) while its re-fit
		// aims at the exact point, so every re-fit moves its content by under one scaled pixel of
		// the group plus one canvas pixel along a right- or bottom-anchored axis, and half that
		// along a centred one, which the larger of the two is a bound on. Zero for a top-left
		// anchored group.
		auto refitRounding = [&]() {
			const uint32_t alignment = obs_sceneitem_get_alignment(groupItem);
			vec2 groupScale;
			obs_sceneitem_get_scale(groupItem, &groupScale);
			auto axis = [](uint32_t alignment, uint32_t start, uint32_t end, float scale) {
				if (alignment & start) {
					return 0.0f;
				}
				const float scaled = std::fabs(scale);
				return (alignment & end) ? scaled + 1.0f : std::max(scaled, 1.0f);
			};
			return std::hypot(axis(alignment, OBS_ALIGN_LEFT, OBS_ALIGN_RIGHT, groupScale.x),
					  axis(alignment, OBS_ALIGN_TOP, OBS_ALIGN_BOTTOM, groupScale.y));
		};
		// `exact` is the slack for a top-left anchored group.
		auto placementSlack = [&](float exact) {
			return std::max(exact, refitRounding());
		};

		// Where the group sits and every item's box as the canvas shows it: what an undo or redo of
		// a child write has to bring back, whatever the group is anchored at.
		struct Arrangement {
			vec2 group;
			vec2 top;
			vec3 aTl, aBr, bTl, bBr;
		};
		auto arrangement = [&]() {
			Arrangement out;
			out.group = posOf(groupItem);
			out.top = posOf(top);
			canvasBoxOf(childA, out.aTl, out.aBr);
			canvasBoxOf(childB, out.bTl, out.bBr);
			return out;
		};
		auto arrangementText = [&](const Arrangement &a) {
			return "group " + posText(a.group) + " a " + boxText(a.aTl, a.aBr) + " b " +
			       boxText(a.bTl, a.bBr);
		};
		auto sameBox = [&](const vec3 &tl, const vec3 &br, const vec3 &otherTl, const vec3 &otherBr,
				   float tolerance) {
			return within(tl.x, otherTl.x, tolerance) && within(tl.y, otherTl.y, tolerance) &&
			       within(br.x, otherBr.x, tolerance) && within(br.y, otherBr.y, tolerance);
		};
		auto sameArrangement = [&](const Arrangement &a, const Arrangement &b, float tolerance) {
			return within(a.group.x, b.group.x, tolerance) && within(a.group.y, b.group.y, tolerance) &&
			       samePos(a.top, b.top) && sameBox(a.aTl, a.aBr, b.aTl, b.aBr, tolerance) &&
			       sameBox(a.bTl, a.bBr, b.bTl, b.bBr, tolerance);
		};
		// Each re-fit of a group anchored anywhere but its top-left corner can leave it up to
		// refitRounding from where its content puts it, which is libobs's rounding rather than the
		// undo's. Checks the group is within that of where every case starts, then puts it back
		// there so the next case starts there too.
		auto clearRefitResidue = [&](const std::string &label) {
			const vec2 pos = posOf(groupItem);
			const float offset = std::hypot(pos.x - groupBefore.x, pos.y - groupBefore.y);
			check(label + " leaves the group within one re-fit's rounding of where it started",
			      offset <= refitRounding() + 0.01f,
			      "group " + posText(pos) + ", off by " + std::to_string(offset));
			obs_sceneitem_set_pos(groupItem, &groupBefore);
			settle();
		};
		// Undo and redo of the child write just made, over three cycles: each undo lands within
		// `tolerance` of `before` with the children's own state exact, each redo within it of
		// `after`, and the last of each where the first landed, so nothing builds up. Leaves the
		// write undone. A top-left anchored group's position is also held to its exact slack.
		auto undoRedoCycles = [&](const std::string &label, const Arrangement &before, const Arrangement &after,
					  float tolerance) {
			const bool anchoredTopLeft = obs_sceneitem_get_alignment(groupItem) == topLeft;
			std::string undoMiss, redoMiss;
			Arrangement firstUndone{}, firstRedone{}, redone{};
			for (int cycle = 0; cycle < 3; cycle++) {
				undoAndSettle();
				const Arrangement undone = arrangement();
				const bool childrenBack = anchoredTopLeft ? groupAsBefore() : childrenAsBefore();
				if (undoMiss.empty() && !(childrenBack && sameArrangement(undone, before, tolerance))) {
					undoMiss = "cycle " + std::to_string(cycle + 1) + " " +
						   arrangementText(undone) + ", " + canvasText();
				}
				ObsBootstrap::Undo().Redo();
				settle();
				redone = arrangement();
				if (redoMiss.empty() && !sameArrangement(redone, after, tolerance)) {
					redoMiss = "cycle " + std::to_string(cycle + 1) + " " + arrangementText(redone);
				}
				if (cycle == 0) {
					firstUndone = undone;
					firstRedone = redone;
				}
			}
			undoAndSettle();
			const Arrangement lastUndone = arrangement();
			check(label + " undo restores the group and siblings in one step", undoMiss.empty(),
			      "before " + arrangementText(before) + "; " + undoMiss);
			check(label + " redo reapplies it", redoMiss.empty(),
			      "after " + arrangementText(after) + "; " + redoMiss);
			check(label + " undo and redo do not drift over three cycles",
			      sameArrangement(lastUndone, firstUndone, 0.01f) &&
				      sameArrangement(redone, firstRedone, 0.01f),
			      "first undo " + arrangementText(firstUndone) + ", last " + arrangementText(lastUndone) +
				      "; first redo " + arrangementText(firstRedone) + ", last " +
				      arrangementText(redone));
		};

		// Centering through a rotated, unevenly scaled group: a group-space offset applied as if
		// it were a canvas one misses the centre, and on one axis moves the other canvas axis.
		auto centerCase = [&](const char *action, const std::string &groupLabel, float rot, float sx,
				      float sy) {
			setGroupTransform(rot, sx, sy);
			vec3 bTl, bBr, aTl, aBr;
			canvasBoxOf(childB, bTl, bBr);
			canvasBoxOf(childA, aTl, aBr);
			const float fromX = (bTl.x + bBr.x) * 0.5f, fromY = (bTl.y + bBr.y) * 0.5f;
			const bool alongX = std::string(action) != "centerVertical";
			const bool alongY = std::string(action) != "centerHorizontal";
			const float wantX = alongX ? canvasWidth * 0.5f : fromX;
			const float wantY = alongY ? canvasHeight * 0.5f : fromY;
			const float slack = placementSlack(0.1f);
			const Arrangement before = arrangement();
			bool actionOk = false;
			actionOnB(action, actionOk);
			vec3 bTlAfter, bBrAfter, aTlAfter, aBrAfter;
			canvasBoxOf(childB, bTlAfter, bBrAfter);
			canvasBoxOf(childA, aTlAfter, aBrAfter);
			check(std::string(action) + " places a child against the canvas through a " + groupLabel,
			      actionOk && !within(fromX, canvasWidth * 0.5f, 1.0f) &&
				      !within(fromY, canvasHeight * 0.5f, 1.0f) &&
				      within((bTlAfter.x + bBrAfter.x) * 0.5f, wantX, slack) &&
				      within((bTlAfter.y + bBrAfter.y) * 0.5f, wantY, slack) &&
				      within(aTlAfter.x, aTl.x, slack) && within(aTlAfter.y, aTl.y, slack),
			      "child b " + boxText(bTl, bBr) + " -> " + boxText(bTlAfter, bBrAfter) + ", sibling " +
				      boxText(aTl, aBr) + " -> " + boxText(aTlAfter, aBrAfter));
			undoRedoCycles(std::string(action) + " through a " + groupLabel, before, arrangement(),
				       placementSlack(0.05f));
		};
		for (const char *action : {"center", "centerHorizontal", "centerVertical"}) {
			centerCase(action, "rotated group", 30.0f, 1.5f, 0.75f);
		}

		// Fit and stretch cover the canvas with the child's content upright: through a mirrored
		// group only a flipped child reads unmirrored, and through a quarter-turned group only a
		// turned one covers the canvas rather than its transpose.
		auto fillCase = [&](const char *action, obs_bounds_type type, const std::string &groupLabel, float rot,
				    float sx, float sy) {
			setGroupTransform(rot, sx, sy);
			const float slack = placementSlack(0.25f);
			vec3 aTl, aBr;
			canvasBoxOf(childA, aTl, aBr);
			const Arrangement before = arrangement();
			bool actionOk = false;
			actionOnB(action, actionOk);
			matrix4 box;
			const bool found = SceneItems::ItemBoxToCanvas(childB, box, scene);
			vec3 bTl, bBr, aTlAfter, aBrAfter;
			canvasBoxOf(childB, bTl, bBr);
			canvasBoxOf(childA, aTlAfter, aBrAfter);
			const bool covers = found && within(bTl.x, 0.0f, slack) && within(bTl.y, 0.0f, slack) &&
					    within(bBr.x, canvasWidth, slack) && within(bBr.y, canvasHeight, slack) &&
					    within(box.x.y, 0.0f, 0.01f) && within(box.y.x, 0.0f, 0.01f);
			matrix4 draw, groupDraw;
			obs_sceneitem_get_draw_transform(childB, &draw);
			obs_sceneitem_get_draw_transform(groupItem, &groupDraw);
			matrix4_mul(&draw, &draw, &groupDraw);
			const bool upright = draw.x.x > 0.0f && draw.y.y > 0.0f &&
					     std::fabs(draw.x.y) <= 1e-3f * draw.x.x &&
					     std::fabs(draw.y.x) <= 1e-3f * draw.y.y;
			check(std::string(action) + " covers the canvas with a child upright through a " + groupLabel,
			      actionOk && covers && upright && obs_sceneitem_get_bounds_type(childB) == type &&
				      within(aTlAfter.x, aTl.x, slack) && within(aTlAfter.y, aTl.y, slack),
			      "child b " + boxText(bTl, bBr) + ", content axes (" + std::to_string(draw.x.x) + "," +
				      std::to_string(draw.x.y) + ") (" + std::to_string(draw.y.x) + "," +
				      std::to_string(draw.y.y) + "), sibling " + boxText(aTl, aBr) + " -> " +
				      boxText(aTlAfter, aBrAfter));
			undoRedoCycles(std::string(action) + " through a " + groupLabel, before, arrangement(),
				       placementSlack(0.05f));
		};
		fillCase("fitToScreen", OBS_BOUNDS_SCALE_INNER, "rotated, mirrored group", 30.0f, -1.5f, 1.5f);
		fillCase("stretchToScreen", OBS_BOUNDS_STRETCH, "quarter-turned, scaled group", 90.0f, 0.5f, 0.5f);

		// A group anchored anywhere but its top-left corner is anchored at a fraction of its extent,
		// which a child write changes, so an undo has to put the group back against the extent it
		// has when the undo runs rather than the one it was captured with.
		auto nudgeUndoCase = [&](const std::string &groupLabel) {
			setGroupTransform(30.0f, 1.5f, 0.75f);
			const Arrangement before = arrangement();
			const uint32_t widthBefore = obs_source_get_width(groupSrc);
			const uint32_t heightBefore = obs_source_get_height(groupSrc);
			json p = base;
			p["refs"] = json::array({json{{"id", childAId}, {"group", groupUuid}}});
			p["dx"] = 7.0f;
			p["dy"] = -4.0f;
			bool nudged = false;
			run("sceneItems.nudge", p, nudged);
			settle();
			check("nudge resizes a " + groupLabel,
			      nudged && (obs_source_get_width(groupSrc) != widthBefore ||
					 obs_source_get_height(groupSrc) != heightBefore),
			      arrangementText(arrangement()));
			undoRedoCycles("nudge in a " + groupLabel, before, arrangement(), placementSlack(0.05f));
		};
		auto removeUndoCase = [&](const std::string &groupLabel) {
			setGroupTransform(30.0f, 1.5f, 0.75f);
			const float tolerance = placementSlack(0.05f);
			const Arrangement before = arrangement();
			const uint32_t widthBefore = obs_source_get_width(groupSrc);
			run("sceneItems.remove", childParams(childAId), ok);
			settle();
			vec3 bTlRemoved, bBrRemoved;
			canvasBoxOf(childB, bTlRemoved, bBrRemoved);
			const bool shrank = ok && !childNamed(kChildAName) &&
					    obs_source_get_width(groupSrc) != widthBefore;
			auto restoredText = [&]() {
				return childA ? arrangementText(arrangement()) + ", " + canvasText()
					      : std::string("a gone");
			};
			ObsBootstrap::Undo().Undo();
			settle();
			childA = childNamed(kChildAName);
			const bool restored = childA && childrenAsBefore() &&
					      sameArrangement(arrangement(), before, tolerance);
			check("child remove undo in a " + groupLabel + " restores it and the group in place",
			      shrank && restored, "before " + arrangementText(before) + "; " + restoredText());
			ObsBootstrap::Undo().Redo();
			settle();
			vec3 bTl, bBr;
			canvasBoxOf(childB, bTl, bBr);
			const bool removedAgain = !childNamed(kChildAName) &&
						  sameBox(bTl, bBr, bTlRemoved, bBrRemoved, tolerance);
			const std::string redoneText =
				"b " + boxText(bTl, bBr) + ", was " + boxText(bTlRemoved, bBrRemoved);
			ObsBootstrap::Undo().Undo();
			settle();
			childA = childNamed(kChildAName);
			childAId = childA ? obs_sceneitem_get_id(childA) : 0;
			check("child remove redo and undo again in a " + groupLabel,
			      removedAgain && childA && childrenAsBefore() &&
				      sameArrangement(arrangement(), before, tolerance),
			      redoneText + "; " + restoredText());
		};
		// A cropped group draws its content shifted by the crop, which its re-fit has to keep in
		// place like any other child write.
		auto croppedCase = [&](const std::string &groupLabel) {
			obs_sceneitem_crop groupCrop{};
			groupCrop.left = 20;
			groupCrop.top = 10;
			obs_sceneitem_set_crop(groupItem, &groupCrop);
			setGroupTransform(30.0f, 1.5f, 0.75f);
			const Arrangement before = arrangement();
			setPos(childB, bBefore.x, bBefore.y);
			settle();
			check("a child write that keeps a cropped, " + groupLabel + "'s extent leaves it in place",
			      sameArrangement(arrangement(), before, placementSlack(0.05f)),
			      "before " + arrangementText(before) + ", after " + arrangementText(arrangement()));
			clearRefitResidue("a child write in a cropped, " + groupLabel);
			centerCase("center", "cropped, " + groupLabel, 30.0f, 1.5f, 0.75f);
			clearRefitResidue("center through a cropped, " + groupLabel);
			obs_sceneitem_crop noCrop{};
			obs_sceneitem_set_crop(groupItem, &noCrop);
			settle();
		};

		const std::string centreLabel = "centre-aligned, rotated group";
		obs_sceneitem_set_alignment(groupItem, OBS_ALIGN_CENTER);
		settle();
		centerCase("center", centreLabel, 30.0f, 1.5f, 0.75f);
		clearRefitResidue("center through a " + centreLabel);
		fillCase("fitToScreen", OBS_BOUNDS_SCALE_INNER, centreLabel, 30.0f, 1.25f, 1.25f);
		clearRefitResidue("fitToScreen through a " + centreLabel);
		nudgeUndoCase(centreLabel);
		clearRefitResidue("nudge in a " + centreLabel);
		removeUndoCase(centreLabel);
		clearRefitResidue("child remove in a " + centreLabel);
		croppedCase(centreLabel);
		const std::string bottomRightLabel = "bottom-right-aligned, rotated group";
		obs_sceneitem_set_alignment(groupItem, OBS_ALIGN_BOTTOM | OBS_ALIGN_RIGHT);
		settle();
		centerCase("center", bottomRightLabel, 30.0f, 1.5f, 0.75f);
		clearRefitResidue("center through a " + bottomRightLabel);
		fillCase("fitToScreen", OBS_BOUNDS_SCALE_INNER, bottomRightLabel, 30.0f, 1.25f, 1.25f);
		clearRefitResidue("fitToScreen through a " + bottomRightLabel);
		croppedCase(bottomRightLabel);
		obs_sceneitem_set_alignment(groupItem, topLeft);
		setGroupTransform(0.0f, 1.0f, 1.0f);
		check("non-top-left-aligned cases leave the group as it was", groupAsBefore(), canvasText());

		// A bounded group keeps its box where it is and rescales its content into it, so its undo
		// takes no correction for a changed extent.
		{
			obs_sceneitem_set_alignment(groupItem, OBS_ALIGN_CENTER);
			vec2 groupBounds;
			vec2_set(&groupBounds, float(obs_source_get_width(groupSrc)),
				 float(obs_source_get_height(groupSrc)));
			obs_sceneitem_set_bounds(groupItem, &groupBounds);
			obs_sceneitem_set_bounds_type(groupItem, OBS_BOUNDS_STRETCH);
			setGroupTransform(30.0f, 1.0f, 1.0f);
			const Arrangement before = arrangement();
			json p = childParams(childAId);
			p["transform"] = json{{"pos", json{{"x", -50.0}, {"y", 0.0}}}};
			run("sceneItems.setTransform", p, ok);
			settle();
			const Arrangement after = arrangement();
			check("child move rescales a bounded group's content",
			      ok && !sameArrangement(after, before, 1.0f), arrangementText(after));
			undoRedoCycles("child move in a bounded, centre-aligned group", before, after, 0.05f);
			obs_sceneitem_set_bounds_type(groupItem, OBS_BOUNDS_NONE);
			obs_sceneitem_set_alignment(groupItem, topLeft);
			setGroupTransform(0.0f, 1.0f, 1.0f);
			check("bounded-group case leaves the group as it was", groupAsBefore(), canvasText());
		}

		// Refused before anything is written: a rotated group scaled unevenly draws no rectangle
		// as the canvas, and a bounded group rescales its content after any child write.
		{
			const UndoManager::State undoBeforeRefusals = undoState();
			auto actionRefused = [&](const char *action, std::string &refusal) {
				json p = childParams(childBId);
				p["action"] = action;
				json refusedResult;
				return !Bridge::Dispatch("sceneItems.transformAction", p, refusedResult, refusal);
			};
			setGroupTransform(30.0f, 1.5f, 0.75f);
			std::string unevenError;
			const bool unevenRefused = actionRefused("fitToScreen", unevenError);
			setGroupTransform(0.0f, 1.0f, 1.0f);
			vec2 groupBounds;
			vec2_set(&groupBounds, float(obs_source_get_width(groupSrc)),
				 float(obs_source_get_height(groupSrc)));
			obs_sceneitem_set_bounds(groupItem, &groupBounds);
			obs_sceneitem_set_bounds_type(groupItem, OBS_BOUNDS_STRETCH);
			settle();
			std::string boundedError;
			const bool boundedRefused = actionRefused("center", boundedError);
			obs_sceneitem_set_bounds_type(groupItem, OBS_BOUNDS_NONE);
			settle();
			check("canvas-space actions refuse a child they cannot place",
			      unevenRefused && boundedRefused && undoUnchanged(undoBeforeRefusals) && groupAsBefore(),
			      unevenError + "; " + boundedError);
		}
		// A rotation pivots on the item's visual centre, read from its box. A child's box is
		// only recomputed on the next tick, and a square child's centre hides a stale read,
		// so B is made non-square and each rotation is checked for the centre it keeps.
		auto canvasCentreOf = [&](obs_sceneitem_t *child) {
			matrix4 box;
			obs_sceneitem_get_box_transform(child, &box);
			vec3 centre;
			vec3_set(&centre, 0.5f, 0.5f, 0.0f);
			vec3_transform(&centre, &centre, &box);
			vec2 out;
			vec2_set(&out, centre.x, centre.y);
			const vec2 group = posOf(groupItem);
			vec2_add(&out, &out, &group);
			return out;
		};
		vec2 wide;
		vec2_set(&wide, 2.0f, 1.0f);
		obs_sceneitem_set_scale(childB, &wide);
		settle();
		const vec2 bCentre = canvasCentreOf(childB);
		json rotateParams = childParams(childBId);
		rotateParams["action"] = "rotate90cw";
		run("sceneItems.transformAction", rotateParams, ok);
		settle();
		check("child rotate action keeps its visual centre",
		      ok && obs_sceneitem_get_rot(childB) == 90.0f && samePos(canvasCentreOf(childB), bCentre),
		      "centre " + posText(canvasCentreOf(childB)) + ", was " + posText(bCentre));
		ObsBootstrap::Undo().Undo();
		settle();
		json rotParams = childParams(childBId);
		rotParams["transform"] = json{{"rot", 90.0}};
		run("sceneItems.setTransform", rotParams, ok);
		settle();
		check("child rot transform keeps its visual centre",
		      ok && obs_sceneitem_get_rot(childB) == 90.0f && samePos(canvasCentreOf(childB), bCentre),
		      "centre " + posText(canvasCentreOf(childB)) + ", was " + posText(bCentre));
		ObsBootstrap::Undo().Undo();
		settle();
		vec2 unit;
		vec2_set(&unit, 1.0f, 1.0f);
		obs_sceneitem_set_scale(childB, &unit);
		settle();
		// The clamp measures a child as the canvas shows it and carries the correction back
		// through its group, so a child pushed off the right edge comes back kMinVisiblePx inside
		// it without moving vertically on the canvas. A clamp that skipped children leaves it
		// off the canvas; one that added the canvas correction to a group-space position moves
		// it diagonally.
		setGroupTransform(30.0f, 1.5f, 0.75f);
		{
			auto nudgeBy = [&](const json &refs, float dx) {
				json p = base;
				p["refs"] = refs;
				p["dx"] = dx;
				p["dy"] = 0.0f;
				bool nudged = false;
				run("sceneItems.nudge", p, nudged);
				settle();
				return nudged;
			};
			const json childBSel{{"id", childBId}, {"group", groupUuid}};
			const json topSel{{"id", topId}};
			vec3 bTl, bBr;
			canvasBoxOf(childB, bTl, bBr);
			bool clampOk = nudgeBy(json::array({childBSel}), 5000.0f);
			vec3 bTlFar, bBrFar;
			canvasBoxOf(childB, bTlFar, bBrFar);
			check("canvas clamp returns a child through a rotated group",
			      clampOk && within(bTlFar.x, canvasWidth - Bridge::kMinVisiblePx, 0.1f) &&
				      within(bTlFar.y, bTl.y, 0.1f),
			      boxText(bTl, bBr) + " -> " + boxText(bTlFar, bBrFar));
			undoAndSettle();
			check("canvas clamp undo restores the group", groupAsBefore(), canvasText());

			// The top-level item and the child leave together and come back as one formation: the
			// nearer one ends kMinVisiblePx onto the canvas and the other keeps its offset from it.
			// Skipping the child, or correcting each member separately, breaks the offset.
			const vec2 topStartPos = posOf(top);
			vec3 tTl, tBr;
			canvasBoxOf(top, tTl, tBr);
			clampOk = nudgeBy(json::array({topSel, childBSel}), -5000.0f);
			vec3 tTlFar, tBrFar;
			canvasBoxOf(top, tTlFar, tBrFar);
			canvasBoxOf(childB, bTlFar, bBrFar);
			const float setRight = std::max(tBrFar.x, bBrFar.x);
			check("canvas clamp returns a mixed set as one formation",
			      clampOk && within(setRight, Bridge::kMinVisiblePx, 0.1f) &&
				      within(tTlFar.x - bTlFar.x, tTl.x - bTl.x, 0.1f) &&
				      within(tTlFar.y, tTl.y, 0.1f) && within(bTlFar.y, bTl.y, 0.1f),
			      "top " + boxText(tTl, tBr) + " -> " + boxText(tTlFar, tBrFar) + ", child b " +
				      boxText(bTl, bBr) + " -> " + boxText(bTlFar, bBrFar));
			undoAndSettle();
			check("canvas clamp undo restores a mixed set in one step",
			      samePos(posOf(top), topStartPos) && groupAsBefore(), canvasText());
		}
		setGroupTransform(0.0f, 1.0f, 1.0f);
		check("canvas-space actions leave the group in place", groupAsBefore(), canvasText());

		// --- 9. two children drawing one source: each entry touches only its own item ------
		// The twin shares A's source and sits below it, so a lookup by source alone finds the
		// twin before A.
		obs_sceneitem_t *twin = obs_scene_add(groupScene, childASrc);
		const int64_t twinId = twin ? obs_sceneitem_get_id(twin) : 0;
		if (twin) {
			setPos(twin, 200.0f, 150.0f);
			obs_sceneitem_set_order(twin, OBS_ORDER_MOVE_BOTTOM);
			settle();
			const vec2 twinCanvas = canvasPosOf(twin);
			auto twinNow = [&]() {
				return obs_scene_find_sceneitem_by_id(groupScene, twinId);
			};
			auto childANow = [&]() {
				struct Find {
					obs_source_t *source;
					int64_t skipId;
					obs_sceneitem_t *found;
				} find{childASrc, twinId, nullptr};
				obs_scene_enum_items(
					groupScene,
					[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
						auto *f = static_cast<Find *>(param);
						if (obs_sceneitem_get_source(item) == f->source &&
						    obs_sceneitem_get_id(item) != f->skipId) {
							f->found = item;
							return false;
						}
						return true;
					},
					&find);
				return find.found;
			};
			auto twinText = [&]() {
				obs_sceneitem_t *a = childANow();
				obs_sceneitem_t *t = twinNow();
				return "a " + (a ? posText(canvasPosOf(a)) : std::string("gone")) + " twin " +
				       (t ? posText(canvasPosOf(t)) : std::string("gone")) + " b " +
				       posText(canvasPosOf(childB)) + " (canvas)";
			};
			auto twinAndBInPlace = [&]() {
				obs_sceneitem_t *t = twinNow();
				return t && samePos(canvasPosOf(t), twinCanvas) &&
				       samePos(canvasPosOf(childB), bCanvas);
			};
			run("sceneItems.remove", childParams(childAId), ok);
			settle();
			ObsBootstrap::Undo().Undo();
			settle();
			obs_sceneitem_t *restoredA = childANow();
			check("shared-source remove undo restores each item",
			      ok && restoredA && samePos(canvasPosOf(restoredA), aCanvas) && twinAndBInPlace(),
			      twinText());
			ObsBootstrap::Undo().Redo();
			settle();
			check("shared-source remove redo removes only its own item", !childANow() && twinAndBInPlace(),
			      twinText());
			ObsBootstrap::Undo().Undo();
			settle();
			childA = childANow();
			childAId = childA ? obs_sceneitem_get_id(childA) : 0;
			check("shared-source remove undo after redo",
			      childA && samePos(canvasPosOf(childA), aCanvas) && twinAndBInPlace(), twinText());
			if (obs_sceneitem_t *t = twinNow()) {
				obs_sceneitem_remove(t);
			}
			settle();
		} else {
			check("shared-source setup", false, "");
		}

		// --- 10. nudge: one canvas offset, one undo step, a child through a rotated group -----
		if (childA) {
			obs_sceneitem_set_rot(groupItem, 30.0f);
			vec2 groupScale;
			vec2_set(&groupScale, 1.5f, 0.75f);
			obs_sceneitem_set_scale(groupItem, &groupScale);
			settle();
			// Where an item's own origin lands on the canvas, through its group for a child.
			auto canvasPointOf = [&](obs_sceneitem_t *item) {
				matrix4 draw;
				obs_sceneitem_get_draw_transform(item, &draw);
				vec3 point;
				vec3_zero(&point);
				vec3_transform(&point, &point, &draw);
				if (obs_sceneitem_t *owner = obs_sceneitem_get_group(scene, item)) {
					obs_sceneitem_get_draw_transform(owner, &draw);
					vec3_transform(&point, &point, &draw);
				}
				vec2 out;
				vec2_set(&out, point.x, point.y);
				return out;
			};
			auto nearPos = [](const vec2 &a, const vec2 &b) {
				return std::fabs(a.x - b.x) <= 0.05f && std::fabs(a.y - b.y) <= 0.05f;
			};
			auto shifted = [](const vec2 &p, float dx, float dy) {
				vec2 out;
				vec2_set(&out, p.x + dx, p.y + dy);
				return out;
			};
			auto nudgeParams = [&](const json &refs, float dx, float dy) {
				json p = base;
				p["refs"] = refs;
				p["dx"] = dx;
				p["dy"] = dy;
				return p;
			};
			const json topRef{{"id", topId}};
			const json groupRef{{"id", groupId}};
			auto childRef = [&](int64_t id) {
				return json{{"id", id}, {"group", groupUuid}};
			};
			auto nudgeText = [&]() {
				return "top " + posText(posOf(top)) + " group " + posText(posOf(groupItem)) + " a " +
				       posText(canvasPointOf(childA)) + " b " + posText(canvasPointOf(childB)) +
				       " (canvas)";
			};

			const vec2 topStart = posOf(top), groupStart = posOf(groupItem);
			const vec2 aLocal = posOf(childA), bLocal = posOf(childB);
			const vec2 aStart = canvasPointOf(childA), bStart = canvasPointOf(childB);
			json moved = run("sceneItems.nudge",
					 nudgeParams(json::array({topRef, childRef(childAId)}), 7.0f, -4.0f), ok);
			settle();
			const vec2 topNudged = posOf(top), groupNudged = posOf(groupItem);
			const vec2 aLocalNudged = posOf(childA), bLocalNudged = posOf(childB);
			check("nudge moves a top-level item and a rotated group's child by the canvas offset",
			      ok && moved.value("moved", 0) == 2 &&
				      samePos(topNudged, shifted(topStart, 7.0f, -4.0f)) &&
				      nearPos(canvasPointOf(childA), shifted(aStart, 7.0f, -4.0f)),
			      nudgeText());
			check("nudge leaves the child's sibling in place on the canvas",
			      nearPos(canvasPointOf(childB), bStart), nudgeText());

			const std::string nudgeLabel = undoState().undoName;
			ObsBootstrap::Undo().Undo();
			settle();
			check("nudge undo restores every item in one step",
			      nudgeLabel == "Transform 2 Items" && samePos(posOf(top), topStart) &&
				      samePos(posOf(groupItem), groupStart) && samePos(posOf(childA), aLocal) &&
				      samePos(posOf(childB), bLocal),
			      "label '" + nudgeLabel + "', " + nudgeText());
			ObsBootstrap::Undo().Redo();
			settle();
			check("nudge redo reapplies",
			      samePos(posOf(top), topNudged) && samePos(posOf(groupItem), groupNudged) &&
				      samePos(posOf(childA), aLocalNudged) && samePos(posOf(childB), bLocalNudged),
			      nudgeText());
			ObsBootstrap::Undo().Undo();
			settle();

			// Child B's id is the group's own id, so the two refs differ only by owner.
			json once = run("sceneItems.nudge",
					nudgeParams(json::array({childRef(childBId), groupRef}), 5.0f, 0.0f), ok);
			settle();
			check("nudge of a group and its own child moves them once",
			      ok && once.value("moved", 0) == 1 &&
				      samePos(posOf(groupItem), shifted(groupStart, 5.0f, 0.0f)) &&
				      samePos(posOf(childB), bLocal) &&
				      nearPos(canvasPointOf(childB), shifted(bStart, 5.0f, 0.0f)),
			      nudgeText());
			ObsBootstrap::Undo().Undo();
			settle();

			events.clear();
			run("sceneItems.nudge", nudgeParams(json::array({childRef(childBId)}), 0.0f, 3.0f), ok);
			settle();
			check("nudge of a child alone moves it on the canvas",
			      ok && nearPos(canvasPointOf(childB), shifted(bStart, 0.0f, 3.0f)) &&
				      nearPos(canvasPointOf(childA), aStart),
			      nudgeText());
			check("nudge of a child names the listed scene", listedSceneEvent(), eventsText());
			ObsBootstrap::Undo().Undo();
			settle();

			const UndoManager::State undoBeforeStale = undoState();
			json staleResult;
			std::string staleError;
			const bool staleRefused = !Bridge::Dispatch(
				"sceneItems.nudge", nudgeParams(json::array({topRef, childRef(987654)}), 9.0f, 9.0f),
				staleResult, staleError);
			settle();
			check("nudge with a stale ref refuses and moves nothing",
			      staleRefused && samePos(posOf(top), topStart) && undoUnchanged(undoBeforeStale),
			      staleError);

			// A mirrored group flips one axis, and a near-zero scale needs a group-space offset
			// far larger than the canvas one; either way the child lands on the canvas offset.
			auto nudgeChildThrough = [&](const char *label, float rot, float sx, float sy) {
				obs_sceneitem_set_rot(groupItem, rot);
				vec2_set(&groupScale, sx, sy);
				obs_sceneitem_set_scale(groupItem, &groupScale);
				settle();
				const vec2 aFrom = canvasPointOf(childA), bFrom = canvasPointOf(childB);
				json through = run("sceneItems.nudge",
						   nudgeParams(json::array({childRef(childAId)}), 7.0f, -4.0f), ok);
				settle();
				check(std::string("nudge through a ") + label +
					      " group moves the child by the canvas offset",
				      ok && through.value("moved", 0) == 1 &&
					      nearPos(canvasPointOf(childA), shifted(aFrom, 7.0f, -4.0f)) &&
					      nearPos(canvasPointOf(childB), bFrom),
				      nudgeText());
				ObsBootstrap::Undo().Undo();
				settle();
			};
			nudgeChildThrough("mirrored", 30.0f, -1.5f, 0.75f);
			nudgeChildThrough("near-zero-scale", 0.0f, 0.02f, 0.02f);

			obs_sceneitem_set_rot(groupItem, 0.0f);
			vec2_set(&groupScale, 1.0f, 1.0f);
			obs_sceneitem_set_scale(groupItem, &groupScale);
			settle();
			check("nudge leaves the group as it was",
			      samePos(posOf(groupItem), groupStart) && samePos(posOf(childA), aLocal) &&
				      samePos(posOf(childB), bLocal) && samePos(posOf(top), topStart),
			      nudgeText());

			auto boxOf = [](obs_sceneitem_t *item, vec3 &tl, vec3 &br) {
				matrix4 box;
				obs_sceneitem_get_box_transform(item, &box);
				SceneItems::BoxExtent(box, tl, br);
			};
			// Off the left edge, the clamp leaves kMinVisiblePx of the moved set on the canvas.
			auto boxRightOf = [&](obs_sceneitem_t *item) {
				vec3 tl, br;
				boxOf(item, tl, br);
				return br.x;
			};
			const float kFarLeft = -5000.0f;
			run("sceneItems.nudge", nudgeParams(json::array({topRef}), kFarLeft, 0.0f), ok);
			settle();
			check("nudge clamps a single item back onto the canvas",
			      ok && std::fabs(boxRightOf(top) - Bridge::kMinVisiblePx) <= 0.01f &&
				      std::fabs(posOf(top).y - topStart.y) <= 0.01f,
			      "top " + posText(posOf(top)) + ", right edge " + std::to_string(boxRightOf(top)));
			ObsBootstrap::Undo().Undo();
			settle();
			run("sceneItems.nudge", nudgeParams(json::array({topRef, groupRef}), kFarLeft, 0.0f), ok);
			settle();
			const float setRight = std::max(boxRightOf(top), boxRightOf(groupItem));
			check("nudge clamps a set as one formation",
			      ok && std::fabs(setRight - Bridge::kMinVisiblePx) <= 0.01f &&
				      std::fabs((posOf(top).x - posOf(groupItem).x) - (topStart.x - groupStart.x)) <=
					      0.01f &&
				      std::fabs(posOf(top).y - topStart.y) <= 0.01f &&
				      std::fabs(posOf(groupItem).y - groupStart.y) <= 0.01f,
			      nudgeText() + ", right edge " + std::to_string(setRight));
			ObsBootstrap::Undo().Undo();
			settle();
			check("nudge clamp undo restores the set",
			      samePos(posOf(top), topStart) && samePos(posOf(groupItem), groupStart) &&
				      samePos(posOf(childA), aLocal) && samePos(posOf(childB), bLocal),
			      nudgeText());

			// Each member leaves by a different edge while their union still spans the canvas:
			// the top item above it, the group to its left. The nearest one has to come back.
			const CanvasDefinition *canvasDef = g_canvases.Find(canvasUuid);
			const float canvasW = canvasDef ? float(canvasDef->width) : 0.0f;
			const float canvasH = canvasDef ? float(canvasDef->height) : 0.0f;
			auto onCanvas = [&](obs_sceneitem_t *item) {
				vec3 tl, br;
				boxOf(item, tl, br);
				return br.x > 0.0f && tl.x < canvasW && br.y > 0.0f && tl.y < canvasH;
			};
			setPos(top, 100.0f, -50.0f);
			setPos(groupItem, -250.0f, 500.0f);
			settle();
			const vec2 topSplit = posOf(top), groupSplit = posOf(groupItem);
			run("sceneItems.nudge", nudgeParams(json::array({topRef, groupRef}), -60.0f, -60.0f), ok);
			settle();
			check("nudge clamp returns a set whose members left by different edges",
			      ok && (onCanvas(top) || onCanvas(groupItem)) &&
				      std::fabs((posOf(top).x - posOf(groupItem).x) - (topSplit.x - groupSplit.x)) <=
					      0.01f &&
				      std::fabs((posOf(top).y - posOf(groupItem).y) - (topSplit.y - groupSplit.y)) <=
					      0.01f,
			      nudgeText());
			ObsBootstrap::Undo().Undo();
			setPos(top, topStart.x, topStart.y);
			setPos(groupItem, groupStart.x, groupStart.y);
			settle();
		} else {
			check("nudge setup", false, "child a did not come back");
		}

		// --- 11. preview gestures on a child: move, resize, rotate and Alt-crop -----------
		// Driven through the surface's own OnLeftDown/OnMouseMove/OnLeftUp, so every case
		// exercises the real press, the real conversions and the real drag-end path. The
		// group is turned and scaled unevenly throughout: a gesture that treated a child's
		// group space as the canvas would land somewhere else on every one of them.
		if (Preview::Instance() && childA) {
			const SceneItemKey childAKey(childAId, groupUuid);
			const float gestureSlack = placementSlack(1.5f);

			json selectChild = base;
			selectChild["refs"] = json::array({json{{"id", childAId}, {"group", groupUuid}}});
			bool childSelected = false;
			run("preview.select", selectChild, childSelected);
			check("preview gesture setup selects the child", childSelected, "");

			// Whether libobs still re-fits the group around its children. A re-fit hold a
			// gesture failed to release suppresses this silently for the rest of the
			// session, and nothing else in the app would notice; the child that defines
			// the group's left edge is moved, the group's frame is watched for the follow,
			// and every item is put back where it was.
			auto refitRuns = [&]() {
				const vec2 groupAt = posOf(groupItem);
				const vec2 aAt = posOf(childA), bAt = posOf(childB);
				setPos(childA, aAt.x - 60.0f, aAt.y);
				settle();
				const bool followed = !samePos(posOf(groupItem), groupAt);
				setPos(childA, aAt.x, aAt.y);
				setPos(childB, bAt.x, bAt.y);
				obs_sceneitem_set_pos(groupItem, &groupAt);
				settle();
				return followed;
			};
			auto afterGesture = [&](const std::string &label, const Arrangement &before) {
				undoAndSettle();
				check("preview " + label + " undo restores the group and siblings in one step",
				      sameArrangement(arrangement(), before, gestureSlack),
				      "before " + arrangementText(before) + ", after " +
					      arrangementText(arrangement()));
				const bool released = refitRuns();
				check("preview " + label + " releases the group's re-fit deferral",
				      released && sameArrangement(arrangement(), before, gestureSlack),
				      released ? arrangementText(arrangement())
					       : std::string("the group stopped re-fitting"));
			};
			auto centreOf = [](const vec3 &tl, const vec3 &br) {
				vec2 out;
				vec2_set(&out, (tl.x + br.x) * 0.5f, (tl.y + br.y) * 0.5f);
				return out;
			};
			// A canvas point in the GROUP's space, which is where a child's own geometry lives.
			auto inGroupSpace = [&](const vec2 &canvasPoint) {
				matrix4 canvasToGroup;
				vec3 out;
				vec3_set(&out, canvasPoint.x, canvasPoint.y, 0.0f);
				if (SceneItems::CanvasToGroup(groupItem, canvasToGroup)) {
					vec3_transform(&out, &out, &canvasToGroup);
				}
				vec2 flat;
				vec2_set(&flat, out.x, out.y);
				return flat;
			};
			// The angle a canvas point subtends at a canvas pivot, both read in group space.
			auto groupAngle = [&](const vec2 &canvasPoint, const vec2 &canvasPivot) {
				const vec2 p = inGroupSpace(canvasPoint), c = inGroupSpace(canvasPivot);
				return float(DEG(std::atan2(p.y - c.y, p.x - c.x)));
			};
			auto wrapDegrees = [](float degrees) {
				while (degrees > 180.0f) {
					degrees -= 360.0f;
				}
				while (degrees <= -180.0f) {
					degrees += 360.0f;
				}
				return degrees;
			};
			// One corner of an item's box on the canvas, through the group.
			auto boxCorner = [&](obs_sceneitem_t *item, float u, float v) {
				matrix4 box;
				vec3 corner;
				vec3_set(&corner, u, v, 0.0f);
				if (SceneItems::ItemBoxToCanvas(item, box, scene)) {
					vec3_transform(&corner, &corner, &box);
				}
				vec2 flat;
				vec2_set(&flat, corner.x, corner.y);
				return flat;
			};

			// Every case restores what it touched, so the battery runs whole against more
			// than one group transform. `pass` names which one in the log.
			auto gestureBattery = [&](const std::string &pass) {
				{
					const Arrangement before = arrangement();
					const vec2 from = centreOf(before.aTl, before.aBr);
					const bool driven = Preview::DragForTest(
						canvasUuid, childAKey, PreviewTestGesture::Move, 40.0f, 25.0f);
					settle();
					const Arrangement after = arrangement();
					const vec2 to = centreOf(after.aTl, after.aBr);
					check("preview move carries a child by the CANVAS offset and leaves its sibling" +
						      pass,
					      driven && within(to.x, from.x + 40.0f, gestureSlack) &&
						      within(to.y, from.y + 25.0f, gestureSlack) &&
						      sameBox(after.bTl, after.bBr, before.bTl, before.bBr,
							      gestureSlack),
					      posText(from) + " -> " + posText(to) + ", " + arrangementText(after));
					afterGesture("move" + pass, before);
				}

				{
					// The grabbed corner has to end up UNDER THE POINTER. Free aspect is held for
					// the scripted grab (ResolveTestGrab), so that is exact rather than a slide
					// along a constrained line -- and it is the assertion a dropped mirror fails:
					// such a conversion drives the corner the other way along the group's x axis
					// while still changing the scale and the box, which is all the old check asked.
					const Arrangement before = arrangement();
					vec2 grab;
					vec2_zero(&grab);
					const bool driven = Preview::DragForTest(canvasUuid, childAKey,
										 PreviewTestGesture::ResizeBottomRight,
										 -40.0f, -30.0f, 0, &grab);
					settle();
					const Arrangement after = arrangement();
					vec2 aScale;
					obs_sceneitem_get_scale(childA, &aScale);
					const vec2 corner = boxCorner(childA, 1.0f, 1.0f);
					// StretchItem rounds the box to whole ITEM-LOCAL units, which the group scale
					// then stretches, so the corner lands within a pixel or so rather than on top.
					const float cornerSlack = placementSlack(2.0f);
					check("preview resize puts the grabbed corner under the pointer" + pass,
					      driven && (aScale.x != 1.0f || aScale.y != 1.0f) &&
						      within(corner.x, grab.x - 40.0f, cornerSlack) &&
						      within(corner.y, grab.y - 30.0f, cornerSlack) &&
						      sameBox(after.bTl, after.bBr, before.bTl, before.bBr,
							      gestureSlack),
					      "scale " + posText(aScale) + ", pointer " + posText(grab) +
						      " - (40,30), corner " + posText(corner) + ", " +
						      boxText(before.aTl, before.aBr) + " -> " +
						      boxText(after.aTl, after.aBr));
					afterGesture("resize" + pass, before);
				}

				{
					// The item must turn by the angle the pointer SWEPT ABOUT ITS CENTRE, measured
					// in the group space, which is where the item own rotation lives. Measuring
					// that sweep on the canvas instead, or dropping the group mirror, still turns
					// the item by something and still holds the pivot -- so "it turned at all" let
					// both through. Ctrl is held, so SnapRotation is a no-op and the angle is raw.
					const Arrangement before = arrangement();
					const float rotBefore = obs_sceneitem_get_rot(childA);
					const vec2 pivot = centreOf(before.aTl, before.aBr);
					vec2 grab;
					vec2_zero(&grab);
					const bool driven = Preview::DragForTest(canvasUuid, childAKey,
										 PreviewTestGesture::Rotate, 50.0f,
										 50.0f, 0, &grab);
					settle();
					const Arrangement after = arrangement();
					const float rotAfter = obs_sceneitem_get_rot(childA);
					vec2 grabTo;
					vec2_set(&grabTo, grab.x + 50.0f, grab.y + 50.0f);
					const float swept =
						wrapDegrees(groupAngle(grabTo, pivot) - groupAngle(grab, pivot));
					const float turned = wrapDegrees(rotAfter - rotBefore);
					check("preview rotate turns a child by the pointer sweep in GROUP space" + pass,
					      driven && std::fabs(swept) > 1.0f && within(turned, swept, 1.5f) &&
						      within(centreOf(after.aTl, after.aBr).x, pivot.x, gestureSlack) &&
						      within(centreOf(after.aTl, after.aBr).y, pivot.y, gestureSlack) &&
						      sameBox(after.bTl, after.bBr, before.bTl, before.bBr,
							      gestureSlack),
					      "rot " + std::to_string(rotBefore) + " -> " + std::to_string(rotAfter) +
						      " (turned " + std::to_string(turned) + ", swept " +
						      std::to_string(swept) + "), centre " + posText(pivot) + " -> " +
						      posText(centreOf(after.aTl, after.aBr)));
					afterGesture("rotate" + pass, before);
				}

				{
					// Inward along the box's own +u axis on the canvas, which is what the
					// left edge has to travel for the crop to bite whatever the group's turn.
					matrix4 aBox;
					vec2 inward;
					vec2_zero(&inward);
					if (SceneItems::ItemBoxToCanvas(childA, aBox, scene)) {
						vec2_set(&inward, aBox.x.x, aBox.x.y);
						const float length = vec2_len(&inward);
						vec2_mulf(&inward, &inward, length > 0.0f ? 24.0f / length : 0.0f);
					}
					const Arrangement before = arrangement();
					obs_sceneitem_crop cropBefore;
					obs_sceneitem_get_crop(childA, &cropBefore);
					const bool driven = Preview::DragForTest(canvasUuid, childAKey,
										 PreviewTestGesture::CropLeft, inward.x,
										 inward.y);
					settle();
					obs_sceneitem_crop cropAfter;
					obs_sceneitem_get_crop(childA, &cropAfter);
					const Arrangement after = arrangement();
					check("preview Alt-crop crops a child from its left edge and leaves its sibling" +
						      pass,
					      driven && cropBefore.left == 0 && cropAfter.left > 0 &&
						      cropAfter.right == 0 &&
						      sameBox(after.bTl, after.bBr, before.bTl, before.bBr,
							      gestureSlack),
					      "crop.left " + std::to_string(cropBefore.left) + " -> " +
						      std::to_string(cropAfter.left) + ", " + arrangementText(after));
					afterGesture("crop" + pass, before);
				}
			};

			setGroupTransform(30.0f, 1.5f, 0.75f);
			gestureBattery("");
			// A NEGATIVE axis is the one configuration where a sign error is silent: mirroring the
			// group leaves every offset the same length and only reverses its direction, so a
			// conversion that drops the mirror still lands the right distance from where it began.
			setGroupTransform(30.0f, -1.5f, 0.75f);
			gestureBattery(" through a mirrored group");
			setGroupTransform(30.0f, 1.5f, 0.75f);

			// A locked group locks what it holds. What these two cases pin down is the PREDICATE,
			// ItemTakesGesture: DragForTest reports its refusal and presses nothing, because with
			// no drill-in a press on a refused child is not a no-op -- the body hit-test answers
			// with the top-level item under the pointer, so the press would select and drag THAT,
			// which is a different gesture and belongs to Phase 5 to pin down. So: no handle is
			// offered, no gesture opens on the child, and nothing about the child moves.
			obs_sceneitem_set_locked(groupItem, true);
			const Arrangement lockedBefore = arrangement();
			const bool lockedMove =
				Preview::DragForTest(canvasUuid, childAKey, PreviewTestGesture::Move, 40.0f, 25.0f);
			const bool lockedResize = Preview::DragForTest(
				canvasUuid, childAKey, PreviewTestGesture::ResizeBottomRight, -40.0f, -30.0f);
			settle();
			obs_sceneitem_set_locked(groupItem, false);
			check("a child of a locked group takes no preview gesture",
			      !lockedMove && !lockedResize &&
				      sameArrangement(arrangement(), lockedBefore, gestureSlack),
			      arrangementText(arrangement()));

			// A bounded group rescales its content into its bounds after every child write, so a
			// canvas-space gesture cannot hold: the predicate refuses it rather than let it be
			// applied and undone by the re-fit. Same scope as the locked case above.
			vec2 gestureBounds;
			vec2_set(&gestureBounds, float(obs_source_get_width(groupSrc)),
				 float(obs_source_get_height(groupSrc)));
			obs_sceneitem_set_bounds(groupItem, &gestureBounds);
			obs_sceneitem_set_bounds_type(groupItem, OBS_BOUNDS_STRETCH);
			settle();
			const Arrangement boundedBefore = arrangement();
			const UndoManager::State undoBeforeBounded = undoState();
			const bool boundedMove =
				Preview::DragForTest(canvasUuid, childAKey, PreviewTestGesture::Move, 40.0f, 25.0f);
			const bool boundedRotate =
				Preview::DragForTest(canvasUuid, childAKey, PreviewTestGesture::Rotate, 50.0f, 50.0f);
			settle();
			check("a child of a bounded group takes no preview gesture and records no undo entry",
			      !boundedMove && !boundedRotate && undoUnchanged(undoBeforeBounded) &&
				      sameArrangement(arrangement(), boundedBefore, gestureSlack),
			      arrangementText(arrangement()));
			obs_sceneitem_set_bounds_type(groupItem, OBS_BOUNDS_NONE);
			settle();

			// Square on and unscaled from here: the two cases below are about canvas coordinates
			// landing exactly, and a turned group would only add slack to read through.
			setGroupTransform(0.0f, 1.0f, 1.0f);

			// The group's OWN resize still works. Every gesture now resolves its target through
			// one path, and a top-level item has to come out of that path unchanged.
			{
				json selectGroup = base;
				selectGroup["refs"] = json::array({json{{"id", groupId}, {"group", nullptr}}});
				bool groupSelected = false;
				run("preview.select", selectGroup, groupSelected);
				const Arrangement before = arrangement();
				const bool driven = Preview::DragForTest(canvasUuid, SceneItemKey(groupId),
									 PreviewTestGesture::ResizeBottomRight, -40.0f,
									 -30.0f);
				settle();
				const Arrangement after = arrangement();
				check("the group's own resize still works and carries both children",
				      groupSelected && driven &&
					      !sameBox(after.aTl, after.aBr, before.aTl, before.aBr, 1.0f) &&
					      !sameBox(after.bTl, after.bBr, before.bTl, before.bBr, 1.0f),
				      arrangementText(before) + " -> " + arrangementText(after));
				undoAndSettle();
				check("the group's own resize undoes in one step",
				      sameArrangement(arrangement(), before, 1.0f), arrangementText(arrangement()));
				run("preview.select", selectChild, childSelected);
			}

			// Snapping must not pull a child onto its own group's edges. The group's box is fitted
			// AROUND its children, so every edge a child can be dragged up to is an edge it helped
			// place; left in the snap set the group would keep catching its own content. The re-fit
			// is held for the whole gesture, so the group's box stays where the child started while
			// the child walks up to it -- which is exactly the tempting geometry.
			//
			// SourceSnapCb pairs OPPOSITE edges (a box's left against the dragged box's right), so
			// the temptation here is childA's LEFT edge arriving at the group's RIGHT edge, not the
			// left edge it sits on. Its sibling cannot stand in for the group: the snap walks the
			// scene's own items, and childB is inside the group.
			{
				// Forced rather than asserted, so the case measures the code and not the owner's
				// settings: with snapping off it would go red instead of testing anything, and the
				// distance has to be one whose half is a whole pixel (DragForTest rounds both
				// endpoints to ints, so a fractional step is not the offset that gets applied).
				GeneralSettings &gs = ObsBootstrap::General();
				const double snapDistanceWas = gs.snapDistance;
				const bool snapEnabledWas = gs.snapEnabled, snapToEdgeWas = gs.snapToEdge;
				const bool snapToSourceWas = gs.snapToSource, snapToCenterWas = gs.snapToCenter;
				gs.snapDistance = 10.0;
				gs.snapEnabled = true;
				gs.snapToEdge = true;
				gs.snapToSource = true;
				// Off, not on: the canvas centre line is a snap target like any other, and the walk
				// up to the group edge would be caught by it rather than by what is under test.
				gs.snapToCenter = false;
				const float snapDistance = float(gs.snapDistance);
				const float step = std::max(1.0f, std::floor(snapDistance * 0.5f));

				// The other top-level item starts at the origin, where its right edge is exactly on
				// childA's left and its left edge is on the canvas edge -- it would answer for both
				// of the cases below. Parked clear of them, and the third case then snaps to it.
				const vec2 topStart = posOf(top);
				setPos(top, std::floor(canvasWidth * 0.78f), 120.0f);
				settle();
				vec3 topTl, topBr;
				canvasBoxOf(top, topTl, topBr);

				vec3 aTl, aBr, groupTl, groupBr;
				canvasBoxOf(childA, aTl, aBr);
				canvasBoxOf(groupItem, groupTl, groupBr);
				const float toOwnGroup = (groupBr.x - step) - aTl.x;
				const bool tempted = step >= 1.0f && step < snapDistance && toOwnGroup > snapDistance &&
						     aTl.y < groupBr.y && aBr.y > groupTl.y;
				const bool drivenOff = Preview::DragForTest(
					canvasUuid, childAKey, PreviewTestGesture::MoveSnapping, toOwnGroup, 0.0f);
				settle();
				vec3 offTl, offBr;
				canvasBoxOf(childA, offTl, offBr);
				check("a child does not snap to its own group's edge",
				      drivenOff && tempted && within(offTl.x, aTl.x + toOwnGroup, 0.5f),
				      "left " + std::to_string(aTl.x) + " -> " + std::to_string(offTl.x) +
					      ", stopping " + std::to_string(step) +
					      " short of the group's right edge " + std::to_string(groupBr.x));
				undoAndSettle();

				// Control one: the same gesture still snaps to the canvas edge, so the case above
				// cannot pass by snapping being off or by the drag never reaching CanvasSnapOffset.
				canvasBoxOf(childA, aTl, aBr);
				const bool drivenEdge = Preview::DragForTest(
					canvasUuid, childAKey, PreviewTestGesture::MoveSnapping, step - aTl.x, 0.0f);
				settle();
				vec3 edgeTl, edgeBr;
				canvasBoxOf(childA, edgeTl, edgeBr);
				check("the same drag still snaps a child to the canvas edge",
				      drivenEdge && within(edgeTl.x, 0.0f, 0.5f),
				      "left " + std::to_string(aTl.x) + " -> " + std::to_string(edgeTl.x));
				undoAndSettle();

				// Control two: and still snaps to another SOURCE's edge. Without this, a change that
				// excluded every item rather than just the child's own group would leave the first
				// case green, because the canvas edge above is a different branch of CanvasSnapOffset.
				canvasBoxOf(childA, aTl, aBr);
				const float toTop = (topTl.x - step) - aBr.x;
				// Where the drag alone would leave the edge. The snap has to pull it off this and onto
				// the other item's edge, and the two are `step` apart, so no single pixel of slack in
				// the position rounding can satisfy both halves of the check at once.
				const float unsnapped = aBr.x + toTop;
				const bool reachesTop = toTop > snapDistance && aTl.y < topBr.y && aBr.y > topTl.y;
				const bool drivenSource = Preview::DragForTest(
					canvasUuid, childAKey, PreviewTestGesture::MoveSnapping, toTop, 0.0f);
				settle();
				vec3 srcTl, srcBr;
				canvasBoxOf(childA, srcTl, srcBr);
				// A whole pixel of tolerance, not a half: a move writes each member a ROUNDED
				// position, so an edge snapped onto a target that is not itself on a whole pixel
				// lands beside it. The `unsnapped` half below is what makes the case discriminating.
				check("the same drag still snaps a child to another source's edge",
				      drivenSource && reachesTop && within(srcBr.x, topTl.x, 1.0f) &&
					      !within(srcBr.x, unsnapped, 1.0f),
				      "right " + std::to_string(aBr.x) + " -> " + std::to_string(srcBr.x) +
					      " (unsnapped " + std::to_string(unsnapped) + "), other item's left " +
					      std::to_string(topTl.x));
				undoAndSettle();

				setPos(top, topStart.x, topStart.y);
				settle();
				gs.snapDistance = snapDistanceWas;
				gs.snapEnabled = snapEnabledWas;
				gs.snapToEdge = snapToEdgeWas;
				gs.snapToSource = snapToSourceWas;
				gs.snapToCenter = snapToCenterWas;
			}

			run("preview.select", base, ok);
			setGroupTransform(0.0f, 1.0f, 1.0f);
			check("preview gesture cases leave the group as they found it", groupAsBefore(), canvasText());
		} else if (!childA) {
			check("preview gesture setup", false, "child a did not come back");
		} else {
			HostLog("[selftest] scene-item-group preview gestures: no preview manager (skipped)");
		}

		// --- 11b. drill-in: double click enters a group, and every way back out -----------
		// Driven through the surface's own OnLeftDown / OnLeftDblClk / OnLeftUp, so each case
		// sees the message sequence Windows actually sends on a CS_DBLCLKS window class -- the
		// second press of a double click as WM_LBUTTONDBLCLK, never as another WM_LBUTTONDOWN.
		if (Preview::Instance() && childA) {
			auto keysText = [](const std::vector<SceneItemKey> &keys) {
				std::string out;
				for (const SceneItemKey &key : keys) {
					out += (out.empty() ? "" : " ") + std::to_string(key.id) +
					       (key.IsTopLevel() ? "@top" : "@group");
				}
				return out.empty() ? std::string("(none)") : out;
			};
			auto selectedKeys = [&]() {
				return Preview::SelectedKeysForTest(canvasUuid);
			};
			auto enteredUuid = [&]() {
				return Preview::EnteredGroupForTest(canvasUuid);
			};
			auto childKeys = [&](std::initializer_list<int64_t> ids) {
				std::vector<SceneItemKey> keys;
				for (const int64_t id : ids) {
					keys.emplace_back(id, groupUuid);
				}
				return keys;
			};
			auto boxCentre = [&](obs_sceneitem_t *item) {
				vec3 tl, br;
				canvasBoxOf(item, tl, br);
				vec2 out;
				vec2_set(&out, (tl.x + br.x) * 0.5f, (tl.y + br.y) * 0.5f);
				return out;
			};
			auto drillState = [&]() {
				return "entered '" + enteredUuid() + "', selection " + keysText(selectedKeys());
			};

			run("preview.select", base, ok);
			setGroupTransform(0.0f, 1.0f, 1.0f);
			const vec2 topStartPos = posOf(top);
			vec3 gTl, gBr, aTl, aBr, bTl, bBr;
			canvasBoxOf(groupItem, gTl, gBr);
			canvasBoxOf(childA, aTl, aBr);
			canvasBoxOf(childB, bTl, bBr);
			// Inside the group's box but over neither child: where a band that stays in the
			// group has to start, since a press outside that box is itself an exit.
			vec2 gapInGroup;
			vec2_set(&gapInGroup, (aBr.x + bTl.x) * 0.5f, (gTl.y + gBr.y) * 0.5f);
			// Empty canvas, well clear of every item this scene holds.
			vec2 emptySpot;
			vec2_set(&emptySpot, canvasWidth - 80.0f, 80.0f);
			check("drill-in geometry: a gap inside the group and empty canvas outside it",
			      bTl.x - aBr.x > 8.0f &&
				      Preview::HitTestForTest(canvasUuid, gapInGroup.x, gapInGroup.y) == groupId &&
				      Preview::HitTestForTest(canvasUuid, emptySpot.x, emptySpot.y) < 0,
			      "group " + boxText(gTl, gBr) + " gap " + posText(gapInGroup) + " empty " +
				      posText(emptySpot));

			// Enter. The first press selects the group; the second, arriving as
			// WM_LBUTTONDBLCLK, drills in and takes the child under the pointer.
			const vec2 drillACentre = boxCentre(childA);
			selections.clear();
			Preview::ClickForTest(canvasUuid, drillACentre.x, drillACentre.y, true);
			const json enterEvent = selections.empty() ? json() : selections.back();
			check("double click on a group enters it and selects the child under the pointer",
			      enteredUuid() == groupUuid && selectedKeys() == childKeys({childAId}), drillState());
			check("the enter is announced as enteredGroup on sceneItem.selected",
			      enterEvent.is_object() && enterEvent.value("enteredGroup", json()) ==
								json{{"id", groupId}, {"group", nullptr}},
			      enterEvent.dump());

			// Hit-testing while entered runs over the group's children. Without the scope
			// this press would answer with the GROUP, which is what covers the point at top
			// level.
			const vec2 drillBCentre = boxCentre(childB);
			Preview::ClickForTest(canvasUuid, drillBCentre.x, drillBCentre.y, false);
			check("a click while entered picks the group's child, not the group",
			      enteredUuid() == groupUuid && selectedKeys() == childKeys({childBId}), drillState());

			// The band, twice: it must reach either child and must not pick up a top-level
			// item lying in the swept area. `top` is parked inside the first sweep for
			// exactly that -- and its id equals child A's, so a band that fell back to the
			// scene's own items would look like a correct answer on ids alone.
			setPos(top, aTl.x, aBr.y + 50.0f);
			settle();
			run("preview.select", base, ok);
			Preview::BandForTest(canvasUuid, gapInGroup.x, gapInGroup.y, aTl.x - 10.0f, aBr.y + 200.0f);
			const std::vector<SceneItemKey> bandLeft = selectedKeys();
			setPos(top, topStartPos.x, topStartPos.y);
			settle();
			check("a band while entered takes the group's child and not the top-level item over it",
			      enteredUuid() == groupUuid && bandLeft == childKeys({childAId}),
			      keysText(bandLeft) + ", top-level id " + std::to_string(topId));
			run("preview.select", base, ok);
			Preview::BandForTest(canvasUuid, gapInGroup.x, gapInGroup.y, bBr.x + 10.0f, aTl.y - 10.0f);
			check("a band while entered reaches the other child too",
			      enteredUuid() == groupUuid && selectedKeys() == childKeys({childBId}), drillState());

			// Exit 1: Esc, which arrives over the bridge because the overlay never gets keys.
			json exitReply = run("preview.exitGroup", base, ok);
			check("preview.exitGroup leaves the group and selects it",
			      ok && exitReply.value("exited", false) && enteredUuid().empty() &&
				      selectedKeys() == std::vector<SceneItemKey>{SceneItemKey(groupId)},
			      exitReply.dump() + ", " + drillState());
			exitReply = run("preview.exitGroup", base, ok);
			check("preview.exitGroup outside a group reports nothing to leave",
			      ok && !exitReply.value("exited", true), exitReply.dump());

			// Exit 2: a click outside the group's box, which then applies at top level. The
			// item it lands on shares child A's id, so a press that stayed in the group
			// would select a child and still answer with the same number.
			Preview::ClickForTest(canvasUuid, drillACentre.x, drillACentre.y, true);
			const bool enteredForOutside = enteredUuid() == groupUuid;
			Preview::ClickForTest(canvasUuid, topStartPos.x + 10.0f, topStartPos.y + 10.0f, false);
			check("a click outside the group leaves it and selects at top level",
			      enteredForOutside && enteredUuid().empty() &&
				      selectedKeys() == std::vector<SceneItemKey>{SceneItemKey(topId)},
			      drillState());

			// Exit 3: a click on empty canvas outside the group leaves it and clears.
			Preview::ClickForTest(canvasUuid, drillACentre.x, drillACentre.y, true);
			const bool enteredForEmpty = enteredUuid() == groupUuid;
			Preview::ClickForTest(canvasUuid, emptySpot.x, emptySpot.y, false);
			check("a click on empty canvas leaves the group and clears the selection",
			      enteredForEmpty && enteredUuid().empty() && selectedKeys().empty(), drillState());

			// Exit 4: a scene switch. The drill-in is scoped to the scene it was made in, so
			// the switch drops it and switching back does NOT restore it. A scene-collection
			// change ends up here too: it replaces the scenes, so the uuid stops matching.
			//
			// NOTHING may read the drill-in while the other scene is up: EnteredGroupForTest
			// resolves, and a resolve is itself the exit, so a probe there would be what
			// cleared the state and the read after the switch back would prove nothing. The
			// sceneItem.selected the switch emits is the only witness that does not mutate.
			const char *kOtherScene = "selftest-scene-item-group-scene-2";
			run("scenes.create", json{{"canvas", canvasUuid}, {"name", kOtherScene}}, ok);
			Preview::ClickForTest(canvasUuid, drillACentre.x, drillACentre.y, true);
			const bool enteredForSwitch = enteredUuid() == groupUuid;
			selections.clear();
			run("scenes.setCurrent", json{{"canvas", canvasUuid}, {"name", kOtherScene}}, ok);
			const json switchEvent = selections.empty() ? json() : selections.back();
			run("scenes.setCurrent", json{{"canvas", canvasUuid}, {"name", kSceneName}}, ok);
			const std::string enteredBackAgain = enteredUuid();
			run("scenes.remove", json{{"canvas", canvasUuid}, {"name", kOtherScene}}, ok);
			check("a scene switch leaves the group with no pointer event to notice it",
			      enteredForSwitch && switchEvent.is_object() && switchEvent.contains("enteredGroup") &&
				      switchEvent["enteredGroup"].is_null(),
			      switchEvent.dump());
			check("switching back does not re-enter the group", enteredBackAgain.empty(),
			      "back again '" + enteredBackAgain + "'");

			// A rename is announced the same way a switch is, and must NOT exit: the scene
			// the drill-in was made in is still the one on screen.
			Preview::ClickForTest(canvasUuid, drillACentre.x, drillACentre.y, true);
			const bool enteredForRename = enteredUuid() == groupUuid;
			const char *kRenamedScene = "selftest-scene-item-group-scene-renamed";
			bool renameOk = false;
			run("scenes.rename", json{{"canvas", canvasUuid}, {"from", kSceneName}, {"to", kRenamedScene}},
			    renameOk);
			const std::string enteredAfterRename = enteredUuid();
			run("scenes.rename", json{{"canvas", canvasUuid}, {"from", kRenamedScene}, {"to", kSceneName}},
			    ok);
			check("renaming the scene leaves the drill-in alone",
			      enteredForRename && renameOk && enteredAfterRename == groupUuid, drillState());
			run("preview.exitGroup", base, ok);

			// Exits 5 and 6: the group stops existing while it is entered. Two throwaway
			// one-child groups rather than the shared one, because neither an ungroup nor a
			// removal can be undone.
			struct DrillProbe {
				obs_source_t *src = nullptr; // create-ref, released below
				int64_t groupId = 0;
				std::string groupUuid;
				obs_sceneitem_t *groupItem = nullptr;
				vec2 centre = {};
			};
			auto makeDrillProbe = [&](const char *name, float x, float y) {
				DrillProbe probe;
				probe.src = makeSource(name);
				json created = run("sceneItems.createGroup",
						   json{{"canvas", canvasUuid}, {"name", std::string(name) + "-group"}},
						   ok);
				probe.groupId = ok ? created.value("id", int64_t(0)) : 0;
				probe.groupUuid = ok ? created.value("source", std::string()) : std::string();
				probe.groupItem = probe.groupId ? obs_scene_find_sceneitem_by_id(scene, probe.groupId)
								: nullptr;
				obs_scene_t *inner =
					probe.groupItem
						? obs_group_from_source(obs_sceneitem_get_source(probe.groupItem))
						: nullptr;
				obs_sceneitem_t *child = inner && probe.src ? obs_scene_add(inner, probe.src) : nullptr;
				if (!child) {
					return probe;
				}
				setPos(child, 0.0f, 0.0f);
				setPos(probe.groupItem, x, y);
				settle();
				probe.centre = boxCentre(child);
				return probe;
			};
			DrillProbe ungroupProbe = makeDrillProbe("selftest-scene-item-group-drill-a",
								 canvasWidth * 0.62f, canvasHeight * 0.75f);
			DrillProbe removeProbe = makeDrillProbe("selftest-scene-item-group-drill-b",
								canvasWidth * 0.82f, canvasHeight * 0.75f);
			if (ungroupProbe.groupItem && removeProbe.groupItem) {
				Preview::ClickForTest(canvasUuid, ungroupProbe.centre.x, ungroupProbe.centre.y, true);
				const bool enteredUngroupProbe = enteredUuid() == ungroupProbe.groupUuid;
				run("sceneItems.ungroup", topParams(ungroupProbe.groupId), ok);
				while (obs_wait_for_destroy_queue()) {
				}
				settle();
				check("ungrouping the entered group leaves it",
				      ok && enteredUngroupProbe && enteredUuid().empty(), drillState());

				Preview::ClickForTest(canvasUuid, removeProbe.centre.x, removeProbe.centre.y, true);
				const bool enteredRemoveProbe = enteredUuid() == removeProbe.groupUuid;
				obs_sceneitem_remove(removeProbe.groupItem);
				while (obs_wait_for_destroy_queue()) {
				}
				settle();
				check("deleting the entered group leaves it",
				      enteredRemoveProbe && enteredUuid().empty(), drillState());
			} else {
				check("drill-in probe groups", false, "");
			}
			// Whatever survived: the ungrouped probe's item is top level now, the removed
			// one's group took its child with it. A probe that failed half way through still
			// created its group, and on that path neither the ungroup nor the removal ran --
			// so look the group up by uuid and take it out before the source, or it would
			// stay in the scene for sections 12-14 to trip over.
			for (DrillProbe *probe : {&ungroupProbe, &removeProbe}) {
				obs_sceneitem_t *survivor =
					probe->groupUuid.empty() ? nullptr
								 : SceneItems::FindGroupItem(scene, probe->groupUuid);
				if (survivor) {
					obs_sceneitem_remove(survivor);
				}
				if (probe->src) {
					obs_source_remove(probe->src);
					obs_source_release(probe->src);
				}
			}
			while (obs_wait_for_destroy_queue()) {
			}
			settle();

			// Risk 4: with CS_DBLCLKS set the SECOND of two clicks in one spot arrives as
			// WM_LBUTTONDBLCLK. Unrouted, it would do nothing and the click-through cycle
			// would stop dead on the first item. Two identically placed, identically sized
			// top-level items, neither a group, so the double press cycles instead of
			// entering; which of them is on top is read off the first click rather than
			// assumed from the add order.
			obs_source_t *cycleSrc = makeSource("selftest-scene-item-group-cycle");
			obs_sceneitem_t *cycleItem = cycleSrc ? obs_scene_add(scene, cycleSrc) : nullptr;
			if (cycleItem) {
				setPos(cycleItem, topStartPos.x, topStartPos.y);
				settle();
				const vec2 stack = boxCentre(cycleItem);
				const int64_t cycleId = obs_sceneitem_get_id(cycleItem);
				// One press from an empty selection: the topmost of the two.
				run("preview.select", base, ok);
				Preview::ClickForTest(canvasUuid, stack.x, stack.y, false);
				const std::vector<SceneItemKey> first = selectedKeys();
				// Two presses from an empty selection, the second of them the
				// WM_LBUTTONDBLCLK: one step down the stack, so the OTHER item. This is
				// the case that fails outright when that message is not routed -- the
				// second press would then do nothing and the answer would still be `first`.
				run("preview.select", base, ok);
				Preview::ClickForTest(canvasUuid, stack.x, stack.y, true);
				const std::vector<SceneItemKey> second = selectedKeys();
				// A third press wraps the cycle back to the top, as it does in a run with
				// no double click in it.
				Preview::ClickForTest(canvasUuid, stack.x, stack.y, false);
				const std::vector<SceneItemKey> third = selectedKeys();
				const bool stacked = first.size() == 1 && second.size() == 1 && third.size() == 1 &&
						     first.front().IsTopLevel() &&
						     (first.front().id == cycleId || first.front().id == topId);
				check("the click-through cycle survives the double-click message",
				      stacked && second.front() != first.front() && third == first &&
					      enteredUuid().empty(),
				      keysText(first) + " -> " + keysText(second) + " -> " + keysText(third) +
					      " (stacked ids " + std::to_string(cycleId) + "," + std::to_string(topId) +
					      ")");
				obs_sceneitem_remove(cycleItem);
			} else {
				check("click-cycle probe item", false, "");
			}
			if (cycleSrc) {
				obs_source_remove(cycleSrc);
				obs_source_release(cycleSrc);
			}
			while (obs_wait_for_destroy_queue()) {
			}

			run("preview.select", base, ok);
			setPos(top, topStartPos.x, topStartPos.y);
			settle();
			check("drill-in cases leave the group and the scene as they found them",
			      groupAsBefore() && enteredUuid().empty() && samePos(posOf(top), topStartPos),
			      canvasText());
		}

		// --- 12. a re-fit hold keeps a group item alive past the prune that releases it ----
		{
			json probeGroup = run("sceneItems.createGroup",
					      json{{"canvas", canvasUuid}, {"name", "selftest-scene-item-group-probe"}},
					      ok);
			const int64_t probeId = ok ? probeGroup.value("id", int64_t(0)) : 0;
			obs_sceneitem_t *probeItem = probeId ? obs_scene_find_sceneitem_by_id(scene, probeId) : nullptr;
			if (probeItem) {
				obs_source_t *probeSrc = obs_sceneitem_get_source(probeItem); // borrowed
				OBSWeakSourceAutoRelease weak = obs_source_get_weak_source(probeSrc);
				bool aliveWhileHeld = false;
				Bridge::WithGroupResizeHeldForTest(probeItem, [&]() {
					obs_source_remove(probeSrc);
					obs_scene_prune_sources(scene);
					while (obs_wait_for_destroy_queue()) {
					}
					aliveWhileHeld = !obs_weak_source_expired(weak);
				});
				while (obs_wait_for_destroy_queue()) {
				}
				const bool releasedAfter = obs_weak_source_expired(weak);
				check("re-fit hold outlives the prune of its group item",
				      aliveWhileHeld && releasedAfter,
				      std::string("alive while held ") + (aliveWhileHeld ? "yes" : "no") +
					      ", released after " + (releasedAfter ? "yes" : "no"));
			} else {
				check("re-fit hold probe setup", false, "");
			}
		}

		// --- 13. ungroup: children get new top-level ids, the old entry must not follow --
		moveParams["id"] = childAId;
		moveParams["transform"] = json{{"pos", json{{"x", -20.0}, {"y", 0.0}}}};
		run("sceneItems.setTransform", moveParams, ok);
		settle();
		run("sceneItems.ungroup", topParams(groupId), ok);
		while (obs_wait_for_destroy_queue()) {
		}
		settle();
		std::vector<std::pair<int64_t, vec2>> ungrouped;
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
				vec2 pos;
				obs_sceneitem_get_pos(item, &pos);
				static_cast<std::vector<std::pair<int64_t, vec2>> *>(param)->emplace_back(
					obs_sceneitem_get_id(item), pos);
				return true;
			},
			&ungrouped);
		{
			OBSSourceAutoRelease stale = obs_get_source_by_uuid(groupUuid.c_str());
			check("ungroup", ok && ungrouped.size() == 3,
			      std::to_string(ungrouped.size()) + " top-level item(s), group " +
				      (stale ? "still resolves" : "gone"));
		}
		ObsBootstrap::Undo().Undo();
		settle();
		bool untouched = true;
		for (const auto &entry : ungrouped) {
			obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, entry.first);
			untouched = untouched && item && samePos(posOf(item), entry.second);
		}
		const UndoManager::State afterUngroupUndo = undoState();
		check("undo after ungroup consumes its slot and moves nothing",
		      untouched && afterUngroupUndo.redoName == moveLabel && afterUngroupUndo.undoName != moveLabel,
		      "redo='" + afterUngroupUndo.redoName + "'");

		// --- 14. undo after deleting a group: logs, spends the slot, touches nothing ----
		std::vector<int64_t> formerChildren;
		for (const auto &entry : ungrouped) {
			if (entry.first != topId) {
				formerChildren.push_back(entry.first);
			}
		}
		json regrouped = run("sceneItems.group", json{{"canvas", canvasUuid}, {"ids", formerChildren}}, ok);
		const int64_t group2Id = ok ? regrouped.value("id", int64_t(0)) : 0;
		const std::string group2Uuid = ok ? regrouped.value("source", std::string()) : std::string();
		obs_sceneitem_t *group2Item = group2Id ? obs_scene_find_sceneitem_by_id(scene, group2Id) : nullptr;
		if (group2Item && !formerChildren.empty()) {
			json move2 = base;
			move2["id"] = formerChildren.front();
			move2["group"] = group2Uuid;
			move2["transform"] = json{{"pos", json{{"x", -30.0}, {"y", 0.0}}}};
			run("sceneItems.setTransform", move2, ok);
			settle();
			const std::string move2Label = undoState().undoName;
			obs_scene_t *group2Scene = obs_group_from_source(obs_sceneitem_get_source(group2Item));
			obs_sceneitem_t *moved2 = obs_scene_find_sceneitem_by_id(group2Scene, formerChildren.front());
			OBSSourceAutoRelease moved2Src = moved2 ? obs_source_get_ref(obs_sceneitem_get_source(moved2))
								: nullptr;
			// A child selected in the preview when its group is deleted: the preview draws past
			// the stale key for as long as it holds it, and a selection that repeats it drops it.
			json staleSelect = base;
			staleSelect["refs"] =
				json::array({json{{"id", formerChildren.front()}, {"group", group2Uuid}}});
			bool heldOk = false;
			const json held = Preview::Instance() ? run("preview.select", staleSelect, heldOk) : json();
			obs_sceneitem_remove(group2Item);
			while (obs_wait_for_destroy_queue()) {
			}
			if (Preview::Instance()) {
				bool droppedOk = false;
				const json dropped = run("preview.select", staleSelect, droppedOk);
				check("a child whose group was deleted is dropped from the selection",
				      heldOk && held.value("selectedRefs", json()).size() == 1 && droppedOk &&
					      dropped.value("selectedRefs", json()) == json::array(),
				      held.dump() + " -> " + dropped.dump());
				run("preview.select", base, droppedOk);
			}
			// The entry's own source, alive again as a top-level item: the one thing a
			// resolver falling back past the deleted group could still write onto.
			obs_sceneitem_t *orphan = moved2Src ? obs_scene_add(scene, moved2Src) : nullptr;
			if (orphan) {
				setPos(orphan, 700.0f, 50.0f);
			}
			settle();
			std::vector<std::pair<obs_sceneitem_t *, vec2>> survivors;
			obs_scene_enum_items(
				scene,
				[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
					vec2 pos;
					obs_sceneitem_get_pos(item, &pos);
					static_cast<std::vector<std::pair<obs_sceneitem_t *, vec2>> *>(param)
						->emplace_back(item, pos);
					return true;
				},
				&survivors);
			ObsBootstrap::Undo().Undo();
			settle();
			bool survivorsUntouched = orphan != nullptr;
			for (const auto &survivor : survivors) {
				survivorsUntouched = survivorsUntouched &&
						     samePos(posOf(survivor.first), survivor.second);
			}
			const UndoManager::State afterDeleteUndo = undoState();
			OBSSourceAutoRelease stale = obs_get_source_by_uuid(group2Uuid.c_str());
			check("undo after group delete consumes its slot and moves nothing",
			      ok && !stale && survivorsUntouched && afterDeleteUndo.redoName == move2Label &&
				      afterDeleteUndo.undoName != move2Label,
			      "redo='" + afterDeleteUndo.redoName + "', group " + (stale ? "still resolves" : "gone") +
				      ", orphan " + (orphan ? posText(posOf(orphan)) : std::string("not added")));
		} else {
			check("regroup for the delete case", false, "");
		}
	}

	// --- cleanup: every remaining item, our sources, temp canvas ----------------------
	Bridge::SetEventObserver(nullptr);
	// preview.select gave the temp canvas a surface, which has to go before its canvas does.
	if (Preview::Instance()) {
		Preview::SelectFromBridge(canvasUuid, "", std::vector<SceneItemKey>{});
		Preview::Instance()->DestroyForCanvas(canvasUuid);
	}
	if (!undoAtStart.canUndo && !undoAtStart.canRedo) {
		ObsBootstrap::Undo().Clear();
	} else {
		HostLog("[selftest] scene-item-group cleanup: undo stack was not empty at start; its entries are left");
	}
	std::vector<obs_sceneitem_t *> remaining;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
			static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
			return true;
		},
		&remaining);
	for (obs_sceneitem_t *item : remaining) {
		obs_sceneitem_remove(item);
	}
	for (obs_source_t *src : {topSrc, childASrc, childBSrc}) {
		if (src) {
			obs_source_remove(src);
			obs_source_release(src);
		}
	}
	obs_source_release(sceneSource);
	const bool gone = teardownCanvas();
	HostLog(std::string("[selftest] scene-item-group cleanup: temp canvas ") +
		(gone ? "removed" : "STILL PRESENT (BUG)"));
	HostLog(std::string("[selftest] scene-item-group overall -> ") + (allPass ? "PASS" : "FAIL (BUG)"));
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
			HostLog("[selftest] " + method + " FAILED: " + Err::Diagnostic(error));
			return json(nullptr);
		}
		return result;
	};

	// The point: an edit driven on this canvas's preview surface must touch ONLY
	// that surface's state, leaving the Default surface + output-0 scene alone.
	const std::string canvasUuid = MakeSelfTestCanvas("selftest-isolation-canvas");

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
	const bool selOk =
		Preview::SelectFromBridge(canvasUuid, "", std::vector<SceneItemKey>{SceneItemKey(canvasItemId)})
			.has_value();
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
	Preview::SelectFromBridge(canvasUuid, "", std::vector<SceneItemKey>{});
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

std::vector<std::string> ObsBootstrap::ExplicitRenderEndpoints()
{
	std::vector<std::string> ids;
	for (const auto &[id, name] : Bridge::EnumAudioDevices(false)) {
		if (!id.empty() && id != "default") {
			ids.push_back(id);
		}
	}
	return ids;
}

ObsBootstrap::SelfTestEndpoint ObsBootstrap::ResolveSelfTestEndpoint(const std::string &logPrefix)
{
	SelfTestEndpoint result;
	const std::vector<std::string> endpoints = ExplicitRenderEndpoints();
	if (endpoints.empty()) {
		result.exitCode = 2;
		result.reason = "no named render endpoint to open";
		return result;
	}
	for (const std::string &id : endpoints) {
		HostLog(logPrefix + " endpoint candidate " + id);
	}
	const std::string wanted = Env::Value("BRAIDCAST_SELFTEST_ENDPOINT");
	if (wanted.empty()) {
		result.exitCode = 3;
		result.reason = "BRAIDCAST_SELFTEST_ENDPOINT is unset; set it to a substring of one of the endpoint "
				"ids logged above, naming one nothing else is playing to";
		return result;
	}
	for (const std::string &id : endpoints) {
		if (id.find(wanted) != std::string::npos) {
			result.id = id;
			return result;
		}
	}
	result.exitCode = 3;
	result.reason = "no render endpoint id contains BRAIDCAST_SELFTEST_ENDPOINT='" + wanted + "'";
	return result;
}

namespace {

// Mirrors SELFTEST_NONCE_ENV, the OPT_SELFTEST_* keys of StartRaceProbe and the default-endpoint
// seam's OPT_SELFTEST_DEFAULT_NONCE and SELFTEST_DEFAULT_PROC in plugins/win-wasapi/win-wasapi.cpp.
constexpr const wchar_t *kSelfTestNonceEnv = L"BRAIDCAST_SELFTEST_WASAPI_NONCE";
constexpr const char *kDefaultEndpointNonce = "selftest_default_endpoint_nonce";
constexpr const char *kDefaultEndpointProc = "selftest_default_changed";
constexpr const char *kStartRaceNonce = "selftest_start_race_nonce";
constexpr const char *kStartRaceInWindow = "selftest_start_race_in_window";
constexpr const char *kStartRaceIdleLate = "selftest_start_race_idle_late";
constexpr const char *kStartRaceHeld = "selftest_start_race_held";
constexpr const char *kStartRaceInitDone = "selftest_start_race_init_done";
constexpr const char *kStartRaceWaitMs = "selftest_start_race_wait_ms";
constexpr const char *kStartRaceStatus = "selftest_start_race_status";

// A signal callback that sets the Win32 event it was connected with.
void SetEventOnSignal(void *event, calldata_t *)
{
	SetEvent(static_cast<HANDLE>(event));
}

// True once every destroy task queued before this call has run. On a timeout the marker
// task is still queued and will signal the event later, so the handle is left open.
bool DestroyQueueDrainsWithin(DWORD ms)
{
	const HANDLE drained = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!drained) {
		return false;
	}
	obs_queue_task(OBS_TASK_DESTROY, [](void *event) { SetEvent(static_cast<HANDLE>(event)); }, drained, false);
	if (WaitForSingleObject(drained, ms) != WAIT_OBJECT_0) {
		return false;
	}
	CloseHandle(drained);
	return true;
}

// Releases a source and waits for its destroy callback to have returned. The "destroy" signal
// fires at the top of the deferred destroy task, so waiting on it reaches only the point where
// the destroy has begun; draining the destroy queue behind it is what proves that task
// returned. The wait comes first because the last reference can be dropped on another thread
// after this release, which then queues the destroy later than a marker queued here would.
// On a timeout the handler may still signal the event later, so the handle is left open.
bool ReleaseAndAwaitDestroy(obs_source_t *source, DWORD ms)
{
	const HANDLE destroying = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!destroying) {
		obs_source_release(source);
		return false;
	}
	signal_handler_connect(obs_source_get_signal_handler(source), "destroy", SetEventOnSignal, destroying);

	const ULONGLONG deadline = GetTickCount64() + ms;
	obs_source_release(source);
	if (WaitForSingleObject(destroying, ms) != WAIT_OBJECT_0) {
		return false;
	}
	CloseHandle(destroying);
	const ULONGLONG now = GetTickCount64();
	return DestroyQueueDrainsWithin(deadline > now ? DWORD(deadline - now) : 0);
}

// Events the start-race probe signals. They belong to the test, so they outlive the source;
// the plugin signals duplicates of them.
struct StartRaceProbeEvents {
	WinHandle inWindow = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	WinHandle idleLate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	WinHandle held = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	WinHandle initDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);

	bool Valid() const { return inWindow.Valid() && idleLate.Valid() && held.Valid() && initDone.Valid(); }
};

// Creates a private wasapi_output_capture from `settings` with one of the plugin's self-test
// seams asked for. A seam arms only when a nonce is in both its settings, under `nonceKey`, and
// the process environment, which saved or imported settings cannot arrange. It is minted here,
// published in the Win32 block the plugin reads, and withdrawn once the source exists. Returns
// nullptr with `error` set if the source was not created.
obs_source_t *CreateSeamedCapture(const char *name, obs_data_t *settings, const char *nonceKey, std::string &error)
{
	const std::string nonce = RandomUtil::HexToken(16);
	if (nonce.empty()) {
		error = "could not mint the self-test nonce";
		return nullptr;
	}
	obs_data_set_string(settings, nonceKey, nonce.c_str());

	SetEnvironmentVariableW(kSelfTestNonceEnv, std::wstring(nonce.begin(), nonce.end()).c_str());
	obs_source_t *capture = obs_source_create_private("wasapi_output_capture", name, settings);
	SetEnvironmentVariableW(kSelfTestNonceEnv, nullptr);
	if (!capture) {
		error = "wasapi_output_capture create failed";
	}
	return capture;
}

// A capture with the plugin's start-race probe asked for; ProbeStatus says whether it armed.
obs_source_t *CreateProbedCapture(const char *name, const std::string &deviceId, const StartRaceProbeEvents &events,
				  DWORD holdMs, std::string &error)
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "device_id", deviceId.c_str());
	obs_data_set_int(settings, kStartRaceInWindow, (long long)(intptr_t)(HANDLE)events.inWindow);
	obs_data_set_int(settings, kStartRaceIdleLate, (long long)(intptr_t)(HANDLE)events.idleLate);
	obs_data_set_int(settings, kStartRaceHeld, (long long)(intptr_t)(HANDLE)events.held);
	obs_data_set_int(settings, kStartRaceInitDone, (long long)(intptr_t)(HANDLE)events.initDone);
	obs_data_set_int(settings, kStartRaceWaitMs, holdMs);
	return CreateSeamedCapture(name, settings, kStartRaceNonce, error);
}

std::string ProbeStatus(obs_source_t *capture)
{
	OBSDataAutoRelease applied = obs_source_get_settings(capture);
	return obs_data_get_string(applied, kStartRaceStatus);
}

// Why a probe that is not "armed" leaves its case unexercised.
std::string ProbeRefusal(const std::string &status)
{
	if (status == "no-rtwq") {
		return "RTWorkQ unavailable: the capture-thread path cannot lose this wake-up, so the case cannot "
		       "fail here";
	}
	if (status.empty()) {
		return "probe gate refused: it needs this run's nonce and FE_SMOKE_QUIT_SECONDS in the process "
		       "environment, not only in .env";
	}
	return "probe not armed: " + status;
}

// Polls `done` every 10 ms until it holds or `ms` has passed; true if it held.
template<typename Done> bool PollUntil(Done &&done, DWORD ms)
{
	const ULONGLONG deadline = GetTickCount64() + ms;
	while (!done()) {
		if (GetTickCount64() >= deadline) {
			return false;
		}
		Sleep(10);
	}
	return true;
}

// Counts log lines containing each of its needles while installed; every line still reaches
// the handler it replaced. blog reads the handler and its param as two separate globals, so a
// line racing the install or the restore can pair either handler with the other's param. So
// the state stays static — the param carries nothing to get out of step — and Handler is
// installed with the replaced handler's own param and ignores it. One counter therefore holds
// a list of needles instead of nesting one counter per needle, which would corrupt both the
// counts and the restore chain. Needles must outlive the counter; string literals do.
//
// Asking for more needles than fit is a coding error rather than a runtime condition, and a
// dropped needle would read as a count that never moves — a silent pass for any case that
// expects none. So truncation is logged and latched, and every caller must fail on Truncated()
// before reading a count. assert is compiled out in RelWithDebInfo and cannot carry this.
class LogLineCounter {
public:
	static constexpr size_t kMaxNeedles = 6;

private:
	static inline log_handler_t prevHandler = nullptr;
	static inline void *prevParam = nullptr;
	static inline const char *needles[kMaxNeedles] = {};
	// Released once the needles are in place and acquired by every reader, so a needle pointer
	// is never read before the store that published it. The install is the only other ordering
	// this rests on, and it happens after this store.
	static inline std::atomic<size_t> needleCount = 0;
	static inline std::atomic<int> counts[kMaxNeedles] = {};
	bool truncated = false;

	static void Handler(int level, const char *format, va_list args, void *)
	{
		va_list copy;
		va_copy(copy, args);
		char line[4096];
		vsnprintf(line, sizeof(line), format, copy);
		va_end(copy);
		const size_t active = needleCount.load(std::memory_order_acquire);
		for (size_t i = 0; i < active; ++i) {
			if (strstr(line, needles[i])) {
				++counts[i];
			}
		}
		prevHandler(level, format, args, prevParam);
	}

public:
	explicit LogLineCounter(std::initializer_list<const char *> wanted)
	{
		needleCount.store(0, std::memory_order_release);
		size_t kept = 0;
		for (const char *needle : wanted) {
			if (kept == kMaxNeedles) {
				truncated = true;
				break;
			}
			needles[kept] = needle;
			counts[kept] = 0;
			++kept;
		}
		needleCount.store(kept, std::memory_order_release);
		// Before the install, so this line is not itself counted.
		if (truncated) {
			HostLog("[selftest] log counter kept " + std::to_string(kept) + " of " +
				std::to_string(wanted.size()) + " needles: raise LogLineCounter::kMaxNeedles");
		}
		base_get_log_handler(&prevHandler, &prevParam);
		base_set_log_handler(Handler, prevParam);
	}
	~LogLineCounter() { base_set_log_handler(prevHandler, prevParam); }
	LogLineCounter(const LogLineCounter &) = delete;
	LogLineCounter &operator=(const LogLineCounter &) = delete;

	bool Truncated() const { return truncated; }

	int Count(size_t needle) const
	{
		return needle < needleCount.load(std::memory_order_acquire) ? counts[needle].load() : 0;
	}

	bool WaitForCount(size_t needle, int n, DWORD ms) const
	{
		return PollUntil([&] { return Count(needle) >= n; }, ms);
	}
};

} // namespace

void ObsBootstrap::RunWasapiStartRaceSelfTest()
{
	constexpr DWORD kWaitMs = 3000;
	// Mirrors StartRaceProbe::kStartBoundMs in win-wasapi.
	constexpr DWORD kStartBoundMs = 60000;
	const auto report = [](const std::string &verdict) {
		HostLog("[selftest] wasapi start-race -> " + verdict);
	};

	const std::vector<std::string> endpoints = ExplicitRenderEndpoints();
	if (endpoints.empty()) {
		report("SKIP (no render endpoint to open)");
		return;
	}
	if (!DestroyQueueDrainsWithin(kWaitMs)) {
		report("SKIP (destroy queue already blocked before this case)");
		return;
	}

	const StartRaceProbeEvents events;
	if (!events.Valid()) {
		report("SKIP (could not create probe events)");
		return;
	}

	std::string error;
	obs_source_t *capture =
		CreateProbedCapture("selftest wasapi start-race", endpoints.front(), events, kWaitMs, error);
	if (!capture) {
		report("SKIP (" + error + ")");
		return;
	}

	const std::string status = ProbeStatus(capture);
	if (status != "armed") {
		ReleaseAndAwaitDestroy(capture, kWaitMs);
		report("SKIP (" + ProbeRefusal(status) + ")");
		return;
	}

	// Release only once the capture has passed its stop check and is held inside Initialize.
	const bool entered = WaitForSingleObject(events.inWindow, kWaitMs) == WAIT_OBJECT_0;

	// Stop() waits up to the probe's start bound for the capture start to return, then at most
	// kWaitMs before the probe delivers the late wake-up itself.
	const bool destroyed = ReleaseAndAwaitDestroy(capture, kStartBoundMs + kWaitMs * 2);
	const bool late = WaitForSingleObject(events.idleLate, 0) == WAIT_OBJECT_0;
	const bool held = WaitForSingleObject(events.held, 0) == WAIT_OBJECT_0;
	const bool initDone = WaitForSingleObject(events.initDone, 0) == WAIT_OBJECT_0;

	if (!entered) {
		report(destroyed ? "SKIP (capture never reached the hold; device lookup failed)"
				 : "FAILED (capture never reached the hold and the destroy did not finish)");
	} else if (!held) {
		report(destroyed ? "SKIP (hold timed out before Stop(): race not exercised)"
				 : "FAILED (hold never saw Stop() and the destroy did not finish; not the start race)");
	} else if (!initDone) {
		report(destroyed ? "SKIP (device init failed after the hold: race not exercised)"
				 : "FAILED (Initialize did not finish after the hold; the destroy is still waiting)");
	} else if (late && !destroyed) {
		report("FAILED (wake-up lost and the probe's rescue did not drain the destroy)");
	} else if (late) {
		report("MISMATCH (Stop() saw no idle signal within " + std::to_string(kWaitMs) +
		       " ms; its wake-up was lost to the capture start)");
	} else if (!destroyed) {
		report("FAILED (source destroy did not finish; not the start race)");
	} else {
		report("OK");
	}
}

void ObsBootstrap::RunWasapiStopDuringStartStressSelfTest()
{
	constexpr int kCycles = 300;
	// Initialize takes about 20 ms on a render endpoint, so releasing 0..24 ms after the
	// create lands the stop before, inside, and after it.
	constexpr DWORD kReleaseSpreadMs = 25;
	constexpr DWORD kDestroyBoundMs = 10000;
	const auto log = [](const std::string &line) {
		HostLog("[selftest] wasapi stop-during-start " + line);
	};

	const std::vector<std::string> endpoints = ExplicitRenderEndpoints();
	if (endpoints.empty()) {
		log("overall -> SKIP (no render endpoint to open)");
		return;
	}
	if (!DestroyQueueDrainsWithin(kDestroyBoundMs)) {
		log("overall -> SKIP (destroy queue already blocked before this case)");
		return;
	}

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "device_id", endpoints.front().c_str());

	// A passing cycle says nothing a summary does not, and 300 of them bury the rest of the
	// smoke log, so only a failing cycle gets a line.
	int destroyed = 0;
	ULONGLONG totalMs = 0;
	ULONGLONG maxMs = 0;
	for (int cycle = 0; cycle < kCycles; ++cycle) {
		obs_source_t *capture = obs_source_create_private("wasapi_output_capture",
								  "selftest wasapi stop-during-start", settings);
		if (!capture) {
			log("cycle " + std::to_string(cycle) + " -> FAILED (wasapi_output_capture create failed)");
			break;
		}
		const DWORD releaseAfterMs = DWORD(cycle) % kReleaseSpreadMs;
		if (releaseAfterMs) {
			Sleep(releaseAfterMs);
		}

		const ULONGLONG releasedAt = GetTickCount64();
		if (!ReleaseAndAwaitDestroy(capture, kDestroyBoundMs)) {
			// Every later destroy would queue behind this one, so stop here.
			log("cycle " + std::to_string(cycle) + " release+" + std::to_string(releaseAfterMs) +
			    "ms -> FAILED (destroy did not finish within " + std::to_string(kDestroyBoundMs) + " ms)");
			break;
		}
		++destroyed;
		const ULONGLONG tookMs = GetTickCount64() - releasedAt;
		totalMs += tookMs;
		maxMs = tookMs > maxMs ? tookMs : maxMs;
	}

	const std::string timing = destroyed ? " destroy max " + std::to_string(maxMs) + " ms, mean " +
						       std::to_string(totalMs / destroyed) + "." +
						       std::to_string((totalMs * 10 / destroyed) % 10) + " ms"
					     : " no cycle completed";
	log("overall -> " + std::string(destroyed == kCycles ? "PASS" : "FAILED") + " (" + std::to_string(destroyed) +
	    "/" + std::to_string(kCycles) + " cycles destroyed;" + timing + ")");
}

void ObsBootstrap::RunWasapiRestartSelfTest()
{
	// Every Initialize parks this long in the probe's hold, which is the window the in-start
	// case lands its second restart in.
	constexpr DWORD kHoldMs = 1000;
	// How late into that hold the in-start case may still issue its second restart. Past the
	// hold the update would be served by the arm Initialize just placed, not by the re-check's
	// restartSignal clause, and the case would pass without exercising it.
	constexpr DWORD kIssueBoundMs = 700;
	constexpr DWORD kInitBoundMs = kHoldMs + 4000;
	constexpr DWORD kQuietMs = kHoldMs + 1500;
	constexpr DWORD kActivateBoundMs = 5000;
	constexpr DWORD kEndpointBoundMs = 3000;
	constexpr DWORD kDestroyBoundMs = 10000;
	const auto log = [](const std::string &line) {
		HostLog("[selftest] wasapi restart " + line);
	};

	// Two explicit endpoints, never "default": parking the source on "default" would let a
	// system default-device change raise a real restart, which fails the quiet case and could
	// supply either of the other two cases' initializations.
	const std::vector<std::string> endpoints = ExplicitRenderEndpoints();
	if (endpoints.size() < 2) {
		log("overall -> SKIP (needs two explicit render endpoints to alternate between; found " +
		    std::to_string(endpoints.size()) + ")");
		return;
	}
	const std::string first = endpoints[0];
	const std::string second = endpoints[1];
	log("endpoints -> first=" + first + " second=" + second);

	if (!DestroyQueueDrainsWithin(kDestroyBoundMs)) {
		log("overall -> SKIP (destroy queue already blocked before this case)");
		return;
	}
	const StartRaceProbeEvents events;
	if (!events.Valid()) {
		log("overall -> SKIP (could not create probe events)");
		return;
	}

	// The probe also hands the audio engine an event of its own in place of receiveSignal, so a
	// capture event lands on a handle nothing waits on and only the restart request itself can
	// wake the sample handler.
	//
	// A reconnect driven by a ProcessCaptureData failure logs the same `initialized` line as a
	// restart, so an increment alone does not prove the restart under test produced it. Each
	// restart served on the RTWQ path logs exactly one `invalidated.  Retrying`, so every case
	// pins that count to the restarts it asked for; a spontaneous reconnect, or a start that
	// failed and retried, adds a line the case did not account for.
	constexpr size_t kInitialized = 0;
	constexpr size_t kInvalidated = 1;
	constexpr size_t kFailedStart = 2;
	constexpr size_t kProbeInit = 3;
	const LogLineCounter lines({"initialized (source: selftest wasapi restart)",
				    "invalidated.  Retrying (source: selftest wasapi restart)",
				    "failed to start (source: selftest wasapi restart)",
				    "initialized (source: selftest wasapi endpoint probe)"});
	if (lines.Truncated()) {
		log("overall -> FAILED (the log counter dropped a needle; every count it reports would be blind)");
		return;
	}

	// An endpoint that will not open is an environment fact, so it is found here and skips the
	// whole test. Pre-flighting rather than reclassifying keeps a `failed to start` during the
	// cases meaning the one thing it should: a defect.
	for (const std::string &endpoint : {first, second}) {
		OBSDataAutoRelease probeSettings = obs_data_create();
		obs_data_set_string(probeSettings, "device_id", endpoint.c_str());
		const int beforeProbe = lines.Count(kProbeInit);
		obs_source_t *probe = obs_source_create_private("wasapi_output_capture",
								"selftest wasapi endpoint probe", probeSettings);
		const bool opened = probe && lines.WaitForCount(kProbeInit, beforeProbe + 1, kEndpointBoundMs);
		// A probe that will not destroy leaves the queue blocked, so the real case's own
		// destroy would time out behind it and read as a defect.
		if (probe && !ReleaseAndAwaitDestroy(probe, kDestroyBoundMs)) {
			log("overall -> SKIP (endpoint " + endpoint + " probe did not destroy within " +
			    std::to_string(kDestroyBoundMs) + " ms)");
			return;
		}
		if (!opened) {
			log("overall -> SKIP (endpoint " + endpoint + " did not open within " +
			    std::to_string(kEndpointBoundMs) + " ms)");
			return;
		}
	}

	std::string error;
	obs_source_t *capture = CreateProbedCapture("selftest wasapi restart", first, events, kHoldMs, error);
	if (!capture) {
		log("overall -> SKIP (" + error + ")");
		return;
	}
	const std::string status = ProbeStatus(capture);
	if (status != "armed") {
		ReleaseAndAwaitDestroy(capture, kDestroyBoundMs);
		log("overall -> SKIP (" + ProbeRefusal(status) + ")");
		return;
	}

	// A restart reconnects through the reconnect thread, which exists only while active. Left
	// open if the destroy times out, since the handler could still signal it.
	const HANDLE activated = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (activated) {
		signal_handler_connect(obs_source_get_signal_handler(capture), "activate", SetEventOnSignal, activated);
	}
	obs_source_inc_active(capture);
	const bool isActive = activated && WaitForSingleObject(activated, kActivateBoundMs) == WAIT_OBJECT_0;
	const bool started = lines.WaitForCount(kInitialized, 1, kInitBoundMs);

	const auto update = [capture](const std::string &deviceId) {
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "device_id", deviceId.c_str());
		obs_source_update(capture, settings);
	};

	int invalidatedMark = 0;
	int failedMark = 0;
	const auto markNoise = [&]() {
		invalidatedMark = lines.Count(kInvalidated);
		failedMark = lines.Count(kFailedStart);
	};
	const auto noiseFree = [&](int restarts) {
		return lines.Count(kInvalidated) - invalidatedMark == restarts &&
		       lines.Count(kFailedStart) == failedMark;
	};
	const auto noiseText = [&](int restarts) {
		return ", invalidated +" + std::to_string(lines.Count(kInvalidated) - invalidatedMark) + " of " +
		       std::to_string(restarts) + " expected, failed-to-start +" +
		       std::to_string(lines.Count(kFailedStart) - failedMark);
	};

	bool pass = isActive && started;
	bool inStartSkipped = false;
	if (!pass) {
		log("setup -> FAILED (active=" + std::to_string(isActive) + " initialized=" + std::to_string(started) +
		    ")");
	} else {
		// A device change while capturing restarts it.
		int before = lines.Count(kInitialized);
		markNoise();
		update(second);
		const bool changed = lines.WaitForCount(kInitialized, before + 1, kInitBoundMs);
		const bool changedClean = noiseFree(1);
		log(std::string("device change -> ") +
		    (!changed        ? "FAILED (no re-initialize)"
		     : !changedClean ? "FAILED (a reconnect this case did not ask for supplied the start)"
				     : "PASS (re-initialized)") +
		    " initialized " + std::to_string(before) + " -> " + std::to_string(lines.Count(kInitialized)) +
		    noiseText(1));

		// An update that keeps the device does not.
		before = lines.Count(kInitialized);
		ResetEvent(events.inWindow);
		markNoise();
		update(second);
		const bool quiet = !lines.WaitForCount(kInitialized, before + 1, kQuietMs) &&
				   WaitForSingleObject(events.inWindow, 0) == WAIT_TIMEOUT;
		const bool quietClean = noiseFree(0);
		log(std::string("same device -> ") +
		    (!quiet        ? "FAILED (restarted)"
		     : !quietClean ? "FAILED (a reconnect landed in the quiet window)"
				   : "PASS (no restart)") +
		    " initialized " + std::to_string(before) + " -> " + std::to_string(lines.Count(kInitialized)) +
		    noiseText(0));

		// A restart requested while Initialize is parked before it resets receiveSignal, so the
		// reset eats that wake-up. It must still be served: two initializations, not one. The
		// second update has to reach the plugin while Initialize is still parked, which a
		// scheduling stall can miss without anything being wrong, so a miss retries once and
		// then reports the case unexercised instead of failing the run.
		constexpr int kInStartAttempts = 2;
		bool inStart = false;
		bool inHold = false;
		bool served = false;
		bool servedClean = false;
		bool lateLost = false;
		ULONGLONG issuedAfterMs = 0;
		int attempt = 0;
		for (; attempt < kInStartAttempts; ++attempt) {
			before = lines.Count(kInitialized);
			ResetEvent(events.inWindow);
			markNoise();
			update(first);
			inStart = WaitForSingleObject(events.inWindow, kInitBoundMs) == WAIT_OBJECT_0;
			const ULONGLONG inWindowAt = GetTickCount64();
			issuedAfterMs = 0;
			if (!inStart) {
				break;
			}
			update(second);
			issuedAfterMs = GetTickCount64() - inWindowAt;
			inHold = issuedAfterMs <= kIssueBoundMs;
			if (inHold) {
				served = lines.WaitForCount(kInitialized, before + 2, kInitBoundMs * 2);
				servedClean = noiseFree(2);
				break;
			}
			// The late update still restarts the capture, so let both restarts finish before
			// the retry: it needs a settled source and its own baseline. A settle that never
			// arrives means the late restart itself was lost, which is the defect this case
			// hunts, so report it rather than retrying on an unsettled source.
			if (!lines.WaitForCount(kInitialized, before + 2, kInitBoundMs * 2)) {
				lateLost = true;
				break;
			}
		}
		inStartSkipped = inStart && !inHold && !lateLost;
		log(std::string("restart during Initialize -> ") +
		    (!inStart         ? "FAILED (the first restart never reached Initialize)"
		     : lateLost       ? "FAILED (a restart issued after the hold was never served)"
		     : inStartSkipped ? "SKIP (inconclusive: every attempt issued the second restart after the "
					"hold, so the case was never exercised)"
		     : !served        ? "FAILED (the restart raised inside Initialize was lost)"
		     : !servedClean   ? "FAILED (a reconnect this case did not ask for supplied a start)"
				      : "PASS (both restarts served)") +
		    " initialized " + std::to_string(before) + " -> " + std::to_string(lines.Count(kInitialized)) +
		    noiseText(2) + ", second update +" + std::to_string(issuedAfterMs) + " ms into a " +
		    std::to_string(kHoldMs) + " ms hold, attempt " +
		    std::to_string(std::min(attempt + 1, kInStartAttempts)) + " of " +
		    std::to_string(kInStartAttempts));

		pass = changed && changedClean && quiet && quietClean && (inStartSkipped || (served && servedClean));
	}

	obs_source_dec_active(capture);
	const bool destroyed = ReleaseAndAwaitDestroy(capture, kDestroyBoundMs);
	if (destroyed && activated) {
		CloseHandle(activated);
	}
	log(std::string("destroy -> ") + (destroyed ? "PASS" : "FAILED (did not finish)"));
	log("coverage -> the default-device path is not driven: it needs a system default-device change");
	if (inStartSkipped) {
		log("coverage -> the restart-during-Initialize case did not run: its second restart never landed "
		    "inside the hold");
	}
	log(std::string("overall -> ") + (pass && destroyed ? "PASS" : "FAILED"));
}

void ObsBootstrap::RunWasapiDedupFollowSelfTest()
{
	constexpr DWORD kActivateBoundMs = 5000;
	// Past win-wasapi's RECONNECT_INTERVAL (3 s): a capture recovering from a failed open reopens
	// only on its next reconnect attempt.
	constexpr DWORD kInitBoundMs = 8000;
	constexpr DWORD kDecideBoundMs = 2000;
	constexpr DWORD kDestroyBoundMs = 10000;
	constexpr const char *kName = "selftest wasapi dedup";
	// A well-formed endpoint id no device has, so the capture's open fails.
	constexpr const char *kMissingEndpoint = "{0.0.0.00000000}.{00000000-0000-0000-0000-000000000000}";
	const auto log = [](const std::string &line) {
		HostLog("[selftest] wasapi dedup " + line);
	};

	// The monitor is pinned to one endpoint, and the capture's "default" moves between the two
	// through the plugin's seam; the system default is never touched.
	const std::vector<std::string> endpoints = ExplicitRenderEndpoints();
	if (endpoints.size() < 2) {
		log("overall -> SKIP (needs two explicit render endpoints; found " + std::to_string(endpoints.size()) +
		    ")");
		return;
	}

	if (!DestroyQueueDrainsWithin(kDestroyBoundMs)) {
		log("overall -> SKIP (destroy queue already blocked before this case)");
		return;
	}

	constexpr size_t kInitialized = 0;
	constexpr size_t kOn = 1;
	constexpr size_t kOff = 2;
	constexpr size_t kFailedStart = 3;
	constexpr size_t kLiveInitialized = 4;
	constexpr size_t kTakeoverFailedStart = 5;
	constexpr const char *kIdleName = "selftest wasapi takeover idle";
	constexpr const char *kLiveName = "selftest wasapi takeover live";
	const LogLineCounter lines({"initialized (source: selftest wasapi dedup)",
				    "source selftest wasapi dedup is also used for audio monitoring",
				    "source selftest wasapi dedup is no longer used for audio monitoring",
				    "failed to start (source: selftest wasapi dedup)",
				    "initialized (source: selftest wasapi takeover live)",
				    "failed to start (source: selftest wasapi takeover"});
	if (lines.Truncated()) {
		log("overall -> FAILED (the log counter dropped a needle; every count it reports would be blind)");
		return;
	}

	const auto ownerIs = [](obs_source_t *who) {
		obs_source_t *owner = obs_get_audio_monitoring_dedup_source();
		const bool is = owner == who;
		obs_source_release(owner);
		return is;
	};
	const char *priorName = nullptr;
	const char *priorId = nullptr;
	obs_get_audio_monitoring_device(&priorName, &priorId);
	const std::string savedName = priorName ? priorName : "";
	const std::string savedId = priorId ? priorId : "";
	const auto restoreMonitor = [&]() {
		if (!savedId.empty() && !obs_set_audio_monitoring_device(savedName.c_str(), savedId.c_str())) {
			log("restore -> FAILED (monitoring device " + savedId + " could not be set back)");
		}
	};
	// A capture that matches never takes deduplication from an active one that already has it, so
	// the monitor is pinned to an endpoint no other capture -- the user's Desktop Audio above all --
	// claims.
	std::string monitored;
	std::string other;
	for (size_t i = 0; i < 2 && monitored.empty(); ++i) {
		if (!obs_set_audio_monitoring_device(kName, endpoints[i].c_str())) {
			restoreMonitor();
			log("overall -> SKIP (could not pin the monitoring device to " + endpoints[i] + ")");
			return;
		}
		if (ownerIs(nullptr)) {
			monitored = endpoints[i];
			other = endpoints[1 - i];
		}
	}
	if (monitored.empty()) {
		restoreMonitor();
		log("overall -> SKIP (another capture already deduplicates each of the first two endpoints)");
		return;
	}
	log("endpoints -> monitored=" + monitored + " other=" + other);

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "device_id", "default");
	std::string error;
	obs_source_t *capture = CreateSeamedCapture(kName, settings, kDefaultEndpointNonce, error);
	if (!capture) {
		restoreMonitor();
		log("overall -> SKIP (" + error + ")");
		return;
	}

	// A restart reconnects through the reconnect thread, which exists only while active. Left
	// open if the destroy times out, since the handler could still signal it.
	const HANDLE activated = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (activated) {
		signal_handler_connect(obs_source_get_signal_handler(capture), "activate", SetEventOnSignal, activated);
	}
	obs_source_inc_active(capture);
	const bool isActive = activated && WaitForSingleObject(activated, kActivateBoundMs) == WAIT_OBJECT_0;
	const bool started = lines.WaitForCount(kInitialized, 1, kInitBoundMs);

	const auto moveDefault = [capture](const std::string &id) {
		calldata_t cd = {};
		calldata_set_string(&cd, "id", id.c_str());
		const bool delivered =
			proc_handler_call(obs_source_get_proc_handler(capture), kDefaultEndpointProc, &cd);
		calldata_free(&cd);
		return delivered;
	};
	const auto isOwner = [&]() {
		return ownerIs(capture);
	};
	const auto waitForOwnership = [&](bool expectOn) {
		return PollUntil([&] { return isOwner() == expectOn; }, kDecideBoundMs);
	};
	const auto counts = [&]() {
		return " initialized " + std::to_string(lines.Count(kInitialized)) + ", on " +
		       std::to_string(lines.Count(kOn)) + ", off " + std::to_string(lines.Count(kOff));
	};

	bool pass = false;
	bool skipped = false;
	std::string skipReason;
	// One default move: the restart must re-open the capture on `id`, and the decision it re-makes
	// must leave `expectOn` true exactly when it logged `expectLine` once more. An endpoint that
	// will not open is an environment fact, not a defect.
	const auto step = [&](const char *label, const std::string &id, bool expectOn, int expectLines) {
		const int initBefore = lines.Count(kInitialized);
		const int onBefore = lines.Count(kOn);
		const int offBefore = lines.Count(kOff);
		const int failedBefore = lines.Count(kFailedStart);
		if (!moveDefault(id)) {
			skipped = true;
			skipReason = "the default-endpoint seam refused: it needs this run's nonce and "
				     "FE_SMOKE_QUIT_SECONDS in the process environment, not only in .env";
			return false;
		}
		if (!lines.WaitForCount(kInitialized, initBefore + 1, kInitBoundMs)) {
			if (lines.Count(kFailedStart) > failedBefore) {
				skipped = true;
				skipReason = "endpoint " + id + " did not open";
			} else {
				log(std::string(label) +
				    " -> FAILED (the default move never re-initialized the capture)" + counts());
			}
			return false;
		}
		const bool decided = waitForOwnership(expectOn);
		const size_t line = expectOn ? kOn : kOff;
		const int before = expectOn ? onBefore : offBefore;
		const bool logged = expectLines < 0 || lines.WaitForCount(line, before + expectLines, kDecideBoundMs);
		const bool exact = expectLines < 0 || lines.Count(line) == before + expectLines;
		const bool noOpposite = lines.Count(expectOn ? kOff : kOn) == (expectOn ? offBefore : onBefore);
		const bool ok = decided && logged && exact && noOpposite;
		log(std::string(label) + " -> " +
		    (!decided ? std::string("FAILED (deduplication ") + (expectOn ? "never" : "still") +
					" named this capture)"
		     : !logged || !exact ? "FAILED (the transition was not logged exactly once)"
		     : !noOpposite       ? "FAILED (the opposite transition was logged)"
					 : "PASS") +
		    counts());
		return ok;
	};

	if (!isActive || !started) {
		log("setup -> FAILED (active=" + std::to_string(isActive) + " initialized=" + std::to_string(started) +
		    ")");
	} else {
		// Settles the capture off the monitored endpoint first. Where the real default already
		// is the monitored one, the capture starts out deduplicating and this is its first stop,
		// so the line it logs is not counted.
		const bool baseline = step("baseline", other, false, -1);
		const bool on = baseline && step("default moves onto the monitor", monitored, true, 1);

		// The same decision made again is not a transition: nothing is logged and nothing is sent.
		bool repeatQuiet = false;
		if (on) {
			const int onBefore = lines.Count(kOn);
			obs_source_audio_output_capture_device_changed(capture, monitored.c_str());
			repeatQuiet = lines.Count(kOn) == onBefore && isOwner();
			log(std::string("unchanged decision -> ") +
			    (repeatQuiet ? "PASS" : "FAILED (re-deciding the same match logged a transition)") +
			    counts());
		}

		// A restart onto an endpoint that will not open leaves a capture that hears nothing, so it
		// must give deduplication up rather than keep the monitored sources out of the mix.
		bool failedOff = false;
		if (repeatQuiet) {
			const int initBefore = lines.Count(kInitialized);
			const int offBefore = lines.Count(kOff);
			moveDefault(kMissingEndpoint);
			failedOff = waitForOwnership(false) &&
				    lines.WaitForCount(kOff, offBefore + 1, kDecideBoundMs) &&
				    lines.Count(kOff) == offBefore + 1 && lines.Count(kInitialized) == initBefore;
			log(std::string("default moves onto an endpoint that will not open -> ") +
			    (failedOff ? "PASS"
				       : "FAILED (the failed capture kept deduplication, or it was not logged once)") +
			    counts());
		}
		const bool recovered = failedOff && step("capture reopens on the monitor", monitored, true, 1);
		const bool off = recovered && step("default moves off the monitor", other, false, 1);
		pass = baseline && on && repeatQuiet && failedOff && recovered && off;
	}

	obs_source_dec_active(capture);
	const bool destroyed = ReleaseAndAwaitDestroy(capture, kDestroyBoundMs);
	if (destroyed && activated) {
		CloseHandle(activated);
	}
	log(std::string("destroy -> ") + (destroyed ? "PASS" : "FAILED (did not finish)"));

	// An owner that is not active silences nothing, so it must not keep an active capture on the
	// same endpoint from taking over; two active captures must still not swap.
	bool takeoverPass = false;
	bool takeoverSkipped = false;
	bool takeoverDestroyed = true;
	if (!destroyed || !ownerIs(nullptr)) {
		takeoverSkipped = true;
		log("takeover -> SKIP (deduplication is not free after the first case)");
	} else {
		OBSDataAutoRelease pinned = obs_data_create();
		obs_data_set_string(pinned, "device_id", monitored.c_str());
		obs_source_t *idle = obs_source_create_private("wasapi_output_capture", kIdleName, pinned);
		obs_source_t *live = obs_source_create_private("wasapi_output_capture", kLiveName, pinned);
		const HANDLE idleActivated = CreateEventW(nullptr, TRUE, FALSE, nullptr);

		// Holds for kDecideBoundMs unless the owner changes first.
		const auto ownerStays = [&](obs_source_t *who) {
			return !PollUntil([&] { return !ownerIs(who); }, kDecideBoundMs);
		};
		const auto waitForOwner = [&](obs_source_t *who) {
			return PollUntil([&] { return ownerIs(who); }, kInitBoundMs);
		};

		std::string verdict;
		if (!idle || !live || !idleActivated) {
			verdict = "FAILED (could not create the two captures)";
		} else if (!waitForOwner(idle)) {
			takeoverSkipped = lines.Count(kTakeoverFailedStart) > 0;
			verdict = takeoverSkipped ? "SKIP (endpoint " + monitored + " did not open)"
						  : "FAILED (the idle capture never took deduplication)";
		} else if (!lines.WaitForCount(kLiveInitialized, 1, kInitBoundMs)) {
			takeoverSkipped = lines.Count(kTakeoverFailedStart) > 0;
			verdict = takeoverSkipped ? "SKIP (endpoint " + monitored + " did not open twice)"
						  : "FAILED (the live capture never initialized)";
		} else if (!ownerStays(idle)) {
			verdict = "FAILED (an inactive capture took over from an inactive owner)";
		} else {
			obs_source_inc_active(live);
			if (!waitForOwner(live)) {
				verdict = "FAILED (the idle owner kept the active capture out)";
			} else {
				signal_handler_connect(obs_source_get_signal_handler(idle), "activate",
						       SetEventOnSignal, idleActivated);
				obs_source_inc_active(idle);
				if (WaitForSingleObject(idleActivated, kActivateBoundMs) != WAIT_OBJECT_0) {
					verdict = "FAILED (the idle capture never activated)";
				} else if (!ownerStays(live)) {
					verdict = "FAILED (two active captures swapped deduplication)";
				} else {
					takeoverPass = true;
					verdict = "PASS";
				}
				obs_source_dec_active(idle);
			}
			obs_source_dec_active(live);
		}
		log("takeover -> " + verdict);

		for (obs_source_t *takeover : {live, idle}) {
			if (!takeover) {
				continue;
			}
			takeoverDestroyed = ReleaseAndAwaitDestroy(takeover, kDestroyBoundMs) && takeoverDestroyed;
		}
		if (takeoverDestroyed && idleActivated) {
			CloseHandle(idleActivated);
		}
		log(std::string("takeover destroy -> ") + (takeoverDestroyed ? "PASS" : "FAILED (did not finish)"));
	}

	// Re-decides every Audio Output Capture source against the monitor it is set back to.
	restoreMonitor();

	// Any failure outranks a skip, which outranks a pass.
	const bool failed = (!skipped && !pass) || !destroyed || (!takeoverPass && !takeoverSkipped) ||
			    !takeoverDestroyed;
	if (failed) {
		log("overall -> FAILED");
	} else if (skipped || takeoverSkipped) {
		log("overall -> SKIP (" + (skipped ? skipReason : std::string("the takeover case did not run")) + ")");
	} else {
		log("overall -> PASS");
	}
}

void ObsBootstrap::RunFilterPreviewSelfTest()
{
	FilterPreview *fp = FilterPreviewHost::Instance();
	if (!fp) {
		HostLog("[selftest] filter-preview: no instance (skipped)");
		return;
	}

	// A scene is previewable (OBS_SOURCE_TYPE_SCENE + video), so the current program
	// scene is a subject that needs no setup and leaves nothing behind.
	obs_source_t *scene = Transitions::GetProgramScene();
	if (!scene) {
		HostLog("[selftest] filter-preview: no program scene (skipped)");
		return;
	}
	std::string error;
	const bool opened = fp->Open(scene, error);
	obs_source_release(scene);
	if (!opened) {
		HostLog("[selftest] filter-preview open -> FAILED: " + error);
		return;
	}

	// The overlay + display are lazy: they exist only once the UI reports a rect.
	const bool beforeRect = fp->HasDisplayForTest();
	fp->SetRect(0, 0, 320, 180);
	const bool afterRect = fp->HasDisplayForTest();
	fp->Close();
	const bool afterClose = fp->HasDisplayForTest();
	if (beforeRect || !afterRect || afterClose) {
		HostLog("[selftest] filter-preview lifecycle -> FAILED (beforeRect=" + std::to_string(beforeRect) +
			" afterRect=" + std::to_string(afterRect) + " afterClose=" + std::to_string(afterClose) + ")");
		return;
	}
	HostLog("[selftest] filter-preview lifecycle -> display created on first rect, gone after close");

	// An audio-only source must be refused, which is what makes the dialog omit its
	// preview pane instead of showing a black box.
	OBSSourceAutoRelease audioOnly =
		obs_source_create_private("wasapi_output_capture", "selftest filter-preview audio", nullptr);
	if (!audioOnly) {
		HostLog("[selftest] filter-preview: no audio source to probe (audio-only case unverified)");
		return;
	}
	std::string audioError;
	const bool audioOpened = fp->Open(audioOnly, audioError);
	fp->Close();
	HostLog(std::string("[selftest] filter-preview audio-only -> ") +
		(audioOpened ? "OPENED (BUG: should be refused)" : "refused: " + audioError));
}

void ObsBootstrap::RunAudioMixerSelfTest()
{
	using Bridge::json;

	auto run = [](const std::string &method, const json &params, bool &ok) -> json {
		json result;
		std::string error;
		ok = Bridge::Dispatch(method, params, result, error);
		if (!ok) {
			HostLog("[selftest] " + method + " FAILED: " + Err::Diagnostic(error));
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
	OBSSourceAutoRelease prior = obs_get_output_source(kSelfTestOutputChannel); // save to restore
	obs_source_t *audioSrc =
		obs_source_create("wasapi_output_capture", GlobalAudioChannels::kSelfTestSourceName, nullptr, nullptr);
	if (!audioSrc) {
		HostLog("[selftest] audio-mixer: wasapi_output_capture create FAILED (skipping)");
		return;
	}
	obs_set_output_source(kSelfTestOutputChannel, audioSrc);

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
	obs_set_output_source(kSelfTestOutputChannel, prior); // null or the prior source
	obs_source_remove(audioSrc);
	obs_source_release(audioSrc);
	g_audioMonitor->Rebuild();
	json list2 = run("audio.list", json(nullptr), ok);
	const size_t finalCount = (ok && list2.is_object() && list2["sources"].is_array()) ? list2["sources"].size()
											   : 0;
	HostLog("[selftest] audio-mixer cleanup -> audio.list back to " + std::to_string(finalCount) +
		" source(s) (was " + std::to_string(baseCount) + ")");
}

void ObsBootstrap::RunOverlayAudioSelfTest()
{
	using Overlay::kRerouteAudioKey;
	using Overlay::kRerouteMigratedKey;
	using Overlay::kRerouteOwnerKey;

	// How a row's settings reach the source. Raw hands them to create as seeded -- the
	// plugin's own default and migration, nothing of the frontend's. Create runs them
	// through FollowTemplateReroute first -- the id-keyed form of the ApplyTemplateReroute
	// call overlays.addToScene builds its create settings with. Load wraps them in a
	// saved-source record and runs SyncSavedReroute over it, exactly as SceneCollection::Load
	// does before obs_load_sources.
	enum class Path { Raw, Create, Load };
	// What the row's overlay_id names: nothing, a stock widget of `type`, a forked one, or
	// an id no widget has.
	enum class Bind { None, Stock, Forked, Unknown };
	// A step after the fence that moves the route on a live source: fork the widget and
	// sweep it (the tail every overlays.* mutation ends in), rebind the source to a stock
	// widget of `thenType` through the properties patch, or flip reroute_audio through the
	// same patch the way the properties form does.
	enum class Then { Nothing, Fork, Rebind, Toggle };

	// pageWidth is each type's own default width in obs-browser, and the fence below:
	// it is the first field the deferred update writes back through the source -- the same
	// cross-repo coupling as the keys.
	struct Case {
		const char *name;
		const char *sourceId;
		uint32_t pageWidth;
		Path path;
		Bind bind;
		const char *type;
		bool seedReroute;
		bool seedRerouteValue;
		bool seedMigrated;
		const char *seedOwner;
		Then then;
		const char *thenType;
		bool wantReroute;
		bool wantMarker;
		const char *wantOwner; // kNoOwner = the key absent
	};
	constexpr const char *kNoOwner = "";
	constexpr const char *kTemplate = Overlay::kRerouteOwnerTemplate;
	constexpr const char *kUser = Overlay::kRerouteOwnerUser;
	constexpr const char *kOverlay = Overlay::kOverlaySourceId;
	// The control row is load-bearing, not decorative. libobs initialises
	// source->audio_active to true (libobs/obs-source.c:226), so read as an absolute the
	// rerouted rows would assert it vacuously -- a subject whose update never touched
	// audio state at all still reads active. A plain browser_source through the same
	// harness keeps the shared false default and must come back inactive, which is what
	// makes "true" mean something on the rows above it, and which doubles as the guard
	// that the overlay's flipped default did not leak into the other CEF source type.
	//
	// The first three overlay rows are bound to nothing, so they prove the plugin's own
	// default and migration with the frontend's template rule out of the way.
	const Case kCases[] = {
		{"fresh overlay", kOverlay, 1920, Path::Raw, Bind::None, nullptr, false, false, false, nullptr,
		 Then::Nothing, nullptr, true, true, kNoOwner},
		{"overlay, persisted false, unmigrated", kOverlay, 1920, Path::Raw, Bind::None, nullptr, true, false,
		 false, nullptr, Then::Nothing, nullptr, true, true, kNoOwner},
		{"overlay, persisted false, already migrated", kOverlay, 1920, Path::Raw, Bind::None, nullptr, true,
		 false, true, nullptr, Then::Nothing, nullptr, false, true, kNoOwner},
		{"browser_source control", "browser_source", 800, Path::Raw, Bind::None, nullptr, false, false, false,
		 nullptr, Then::Nothing, nullptr, false, false, kNoOwner},
		{"silent built-in, fresh", kOverlay, 1920, Path::Create, Bind::Stock, "countdown", false, false, false,
		 nullptr, Then::Nothing, nullptr, false, true, kTemplate},
		{"alertbox, fresh", kOverlay, 1920, Path::Create, Bind::Stock, "alertbox", false, false, false, nullptr,
		 Then::Nothing, nullptr, true, true, kTemplate},
		{"forked silent type, fresh", kOverlay, 1920, Path::Create, Bind::Forked, "chatbox", false, false,
		 false, nullptr, Then::Nothing, nullptr, true, true, kTemplate},
		{"unknown overlay, fresh", kOverlay, 1920, Path::Create, Bind::Unknown, nullptr, false, false, false,
		 nullptr, Then::Nothing, nullptr, true, true, kNoOwner},
		{"silent built-in, persisted true, no owner", kOverlay, 1920, Path::Load, Bind::Stock, "labels", true,
		 true, true, nullptr, Then::Nothing, nullptr, false, true, kTemplate},
		{"silent built-in, persisted default, no owner", kOverlay, 1920, Path::Load, Bind::Stock, "viewercount",
		 false, false, true, nullptr, Then::Nothing, nullptr, false, true, kTemplate},
		{"silent built-in, user-owned true", kOverlay, 1920, Path::Load, Bind::Stock, "chatbox", true, true,
		 true, kUser, Then::Nothing, nullptr, true, true, kUser},
		{"alertbox, persisted deliberate false", kOverlay, 1920, Path::Load, Bind::Stock, "alertbox", true,
		 false, true, nullptr, Then::Nothing, nullptr, false, true, kUser},
		{"unknown overlay, persisted true", kOverlay, 1920, Path::Load, Bind::Unknown, nullptr, true, true,
		 true, nullptr, Then::Nothing, nullptr, true, true, kNoOwner},
		// Widgets are global and sources per collection, so a template can change while the
		// collection holding its sources is not loaded. The next load has to catch up, in both
		// directions, and must still never move a user-owned value.
		{"forked while unloaded, template-owned false", kOverlay, 1920, Path::Load, Bind::Forked, "countdown",
		 true, false, true, kTemplate, Then::Nothing, nullptr, true, true, kTemplate},
		{"back to stock while unloaded, template-owned true", kOverlay, 1920, Path::Load, Bind::Stock, "labels",
		 true, true, true, kTemplate, Then::Nothing, nullptr, false, true, kTemplate},
		{"alertbox, user-owned false", kOverlay, 1920, Path::Load, Bind::Stock, "alertbox", true, false, true,
		 kUser, Then::Nothing, nullptr, false, true, kUser},
		{"silent built-in, then forked", kOverlay, 1920, Path::Create, Bind::Stock, "countdown", false, false,
		 false, nullptr, Then::Fork, nullptr, true, true, kTemplate},
		{"silent built-in, rebound to alertbox", kOverlay, 1920, Path::Create, Bind::Stock, "uptime", false,
		 false, false, nullptr, Then::Rebind, "alertbox", true, true, kTemplate},
		{"alertbox, rebound to silent", kOverlay, 1920, Path::Create, Bind::Stock, "alertbox", false, false,
		 false, nullptr, Then::Rebind, "goalbar", false, true, kTemplate},
		{"silent built-in, user turns on", kOverlay, 1920, Path::Create, Bind::Stock, "ticker", false, false,
		 false, nullptr, Then::Toggle, nullptr, true, true, kUser},
	};

	// A test widget per bound row, injected rather than created so nothing reaches
	// overlays.json, and removed at the end of the row whatever the verdict.
	std::vector<std::string> injected;
	auto inject = [&injected](const char *type, bool forked) {
		Overlay::Widget w;
		w.id = "selftest-overlay-audio-" + std::to_string(injected.size());
		w.name = w.id;
		w.type = type;
		if (forked) {
			w.custom = Overlay::CustomCode{};
		}
		Overlay::Store().InjectForTest(w);
		injected.push_back(w.id);
		return w.id;
	};
	auto removeInjected = [&injected] {
		for (const std::string &id : injected) {
			Overlay::Store().RemoveForTest(id);
		}
		injected.clear();
	};

	bool allPass = true;
	for (const Case &c : kCases) {
		OBSDataAutoRelease settings = obs_data_create();
		// Never shown and shutdown-when-invisible, so obs-browser leaves create_browser
		// false and no CEF browser is spun up for a settings-only subject. Load-bearing
		// for the fence below rather than merely economical -- see there.
		obs_data_set_bool(settings, "shutdown", true);
		std::string widgetId;
		if (c.bind == Bind::Stock || c.bind == Bind::Forked) {
			widgetId = inject(c.type, c.bind == Bind::Forked);
		} else if (c.bind == Bind::Unknown) {
			widgetId = "selftest-overlay-audio-no-such-widget";
		}
		if (!widgetId.empty()) {
			obs_data_set_string(settings, Overlay::kOverlayIdKey, widgetId.c_str());
		}
		if (c.seedReroute) {
			obs_data_set_bool(settings, kRerouteAudioKey, c.seedRerouteValue);
		}
		if (c.seedMigrated) {
			obs_data_set_bool(settings, kRerouteMigratedKey, true);
		}
		if (c.seedOwner) {
			obs_data_set_string(settings, kRerouteOwnerKey, c.seedOwner);
		}

		if (c.path == Path::Create) {
			Overlay::FollowTemplateReroute(settings, widgetId.c_str(), settings);
		} else if (c.path == Path::Load) {
			OBSDataAutoRelease saved = obs_data_create();
			obs_data_set_string(saved, "id", c.sourceId);
			obs_data_set_string(saved, "name", c.name);
			obs_data_set_obj(saved, "settings", settings);
			OBSDataArrayAutoRelease sources = obs_data_array_create();
			obs_data_array_push_back(sources, saved);
			Overlay::SyncSavedReroute(sources);
		}

		// Private: obs_enum_sources skips private sources (libobs/obs.c:2871), which is
		// what keeps a concurrent AudioMonitor::Rebuild from attaching a volmeter to a
		// rerouted subject and surfacing it as a row in the mixer dock -- and what keeps
		// a concurrent RefreshSources sweep off it. Defaults still apply --
		// obs_source_create_internal calls get_defaults either way
		// (libobs/obs-source.c:493-500) -- and private sources are still ticked, since
		// they go into obs->data.sources unconditionally (:305), which is the list
		// tick_sources walks.
		OBSSourceAutoRelease src = obs_source_create_private(c.sourceId, "selftest-overlay-audio", settings);
		if (!src) {
			removeInjected();
			// Summary line too: a scraper keying on "overlay-audio ->" should see a
			// verdict rather than nothing at all.
			HostLog(std::string("[selftest] overlay-audio ") + c.name + " SKIPPED: " + c.sourceId +
				" unavailable (obs-browser not loaded)");
			HostLog("[selftest] overlay-audio -> SKIPPED (obs-browser not loaded)");
			return;
		}

		// The fence. libobs defers every OBS_SOURCE_VIDEO update to the video thread, so
		// the settings under test are not applied when create returns; the page width is
		// what the update writes back, so reaching it means the update ran.
		//
		// Sleeping here stops CEF task dispatch: main.cpp runs CEF with
		// multi_threaded_message_loop=false and this self-test runs inside HostWndProc's
		// WM_TIMER, so this thread IS the CEF UI thread. It is safe only because the
		// shutdown seed above leaves create_browser false on a never-shown subject, so
		// DestroyBrowser has nothing to queue. A row that drops that seed, or adds its
		// subject to a scene, would post a CEF task to a thread that will not pump until
		// this function returns.
		//
		// One latent way the fence itself breaks: gpu_diag's kill switch
		// (diag/gpu_diag.cpp:38-53) rewrites width to 16 on every browser source from the
		// global source_create signal, which would strand every row at NO UPDATE. Gated
		// on BRAIDCAST_DISABLE_BROWSER_SOURCES, which the smoke path does not set.
		uint32_t observed = obs_source_get_width(src);
		uint64_t deadline = os_gettime_ns() + 1000000000ULL;
		while (observed != c.pageWidth && os_gettime_ns() < deadline) {
			os_sleep_ms(4);
			observed = obs_source_get_width(src);
		}
		const bool applied = observed == c.pageWidth;

		// The live step. Its settings land synchronously, audio_active only once the
		// deferred update has run, and the width fence cannot tell the two apart because
		// the step does not move the width. So audio_active is polled toward the wanted
		// value and read once more after the deadline: a MISMATCH costs the run a second,
		// never a false OK.
		if (applied && c.then != Then::Nothing) {
			if (c.then == Then::Fork) {
				std::optional<Overlay::Widget> w = Overlay::Store().Get(widgetId);
				if (w) {
					w->custom = Overlay::CustomCode{};
					Overlay::Store().RemoveForTest(widgetId);
					Overlay::Store().InjectForTest(*w);
				}
				Overlay::RefreshSource(src);
			} else {
				OBSDataAutoRelease patch = obs_data_create();
				if (c.then == Then::Rebind) {
					obs_data_set_string(patch, Overlay::kOverlayIdKey,
							    inject(c.thenType, false).c_str());
				} else {
					OBSDataAutoRelease now = obs_source_get_settings(src);
					obs_data_set_bool(patch, kRerouteAudioKey,
							  !obs_data_get_bool(now, kRerouteAudioKey));
				}
				// The "source" property kind's update, which properties.set calls.
				Overlay::ApplySettingsPatch(src, patch);
			}
			deadline = os_gettime_ns() + 1000000000ULL;
			while (obs_source_audio_active(src) != c.wantReroute && os_gettime_ns() < deadline) {
				os_sleep_ms(4);
			}
		}

		OBSDataAutoRelease after = obs_source_get_settings(src);
		const bool reroute = obs_data_get_bool(after, kRerouteAudioKey);
		const bool marker = obs_data_get_bool(after, kRerouteMigratedKey);
		const std::string owner = obs_data_get_string(after, kRerouteOwnerKey);
		// What actually decides whether the mixer lists the source and whether its
		// audio reaches an output at all.
		const bool audioActive = obs_source_audio_active(src);
		const bool pass = applied && reroute == c.wantReroute && marker == c.wantMarker &&
				  owner == c.wantOwner && audioActive == c.wantReroute;
		allPass = allPass && pass;
		HostLog(std::string("[selftest] overlay-audio ") + c.name +
			" -> reroute=" + (reroute ? "true" : "false") + " marker=" + (marker ? "true" : "false") +
			" owner=" + (owner.empty() ? "(none)" : owner) +
			" audioActive=" + (audioActive ? "true" : "false") +
			" mixers=" + std::to_string(obs_source_get_audio_mixers(src)) + " (want reroute=" +
			(c.wantReroute ? "true" : "false") + " marker=" + (c.wantMarker ? "true" : "false") +
			" owner=" + (*c.wantOwner ? c.wantOwner : "(none)") + ") " +
			(pass ? "OK"
			      : (applied ? "MISMATCH"
					 : "NO UPDATE (width " + std::to_string(observed) + ", wanted " +
						   std::to_string(c.pageWidth) + ")")));
		removeInjected();
	}

	HostLog(std::string("[selftest] overlay-audio -> ") + (allPass ? "OK" : "FAILED (see step lines above)"));
}

void ObsBootstrap::RunHotkeysSelfTest()
{
	using Bridge::json;

	auto run = [](const std::string &method, const json &params, bool &ok) -> json {
		json result;
		std::string error;
		ok = Bridge::Dispatch(method, params, result, error);
		if (!ok) {
			HostLog("[selftest] " + method + " FAILED: " + Err::Diagnostic(error));
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

namespace {

// Composite Main into a small buffer and report whether anything landed. Mirrors
// what a Default screenshot does (History::GrabPng and screenshot.takeProgram both
// render this same texture), so a blank here is a blank there.
bool MainCompositeIsBlank()
{
	obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		return true;
	}
	std::vector<uint8_t> bgra;
	std::string err;
	const auto render = []() {
		obs_render_main_texture();
	};
	if (!Bridge::RenderToBgraPixels(ovi.base_width, ovi.base_height, 64, 36, render, true, bgra, err)) {
		return true;
	}
	return History::IsBlank(bgra);
}

// Give the graphics thread two ticks to act on a gate change. total_frames advances
// at the top of each tick, before that tick composites, so two means one full pass
// both began and ended under the new state.
void WaitTwoComposites()
{
	const uint32_t start = obs_get_total_frames();
	const uint64_t deadline = os_gettime_ns() + 500000000ULL;
	while (obs_get_total_frames() - start < 2 && os_gettime_ns() < deadline) {
		os_sleep_ms(4);
	}
}

} // namespace

void ObsBootstrap::RunScreenshotGateSelfTest()
{
	// Channel 0 holds the transition, not the scene; Transitions::GetProgramScene
	// unwraps it (and returns channel 0 directly when no transition is set).
	OBSSourceAutoRelease scene = Transitions::GetProgramScene();
	obs_scene_t *asScene = scene ? obs_scene_from_source(scene) : nullptr;
	if (!asScene) {
		HostLog("[selftest] screenshot-gate SKIPPED: no program scene");
		return;
	}

	// An opaque white fill, so "did the composite run" is decidable by luma alone and
	// does not depend on whatever the program scene happens to hold.
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, "color", 0xFFFFFFFF);
	obs_data_set_int(settings, "width", 1920);
	obs_data_set_int(settings, "height", 1080);
	OBSSourceAutoRelease fill = obs_source_create_private("color_source", "selftest-screenshot-fill", settings);
	if (!fill) {
		HostLog("[selftest] screenshot-gate SKIPPED: color_source unavailable");
		return;
	}
	obs_sceneitem_t *item = obs_scene_add(asScene, fill);
	if (!item) {
		HostLog("[selftest] screenshot-gate SKIPPED: could not add fill to program scene");
		return;
	}

	// Sanity: the fill must be visible at all, or nothing below means anything.
	WaitTwoComposites();
	if (MainCompositeIsBlank()) {
		obs_sceneitem_remove(item);
		HostLog("[selftest] screenshot-gate SKIPPED: fill did not render, so the gate "
			"cannot be told apart from an empty scene");
		return;
	}

	// Try the real thing first: with nothing consuming Main, the frontend gates it and a
	// capture comes back blank. A headless smoke usually has a consumer anyway (an enabled
	// Default binding is enough), so this is attempted, not assumed.
	VideoGate::Reconcile();
	WaitTwoComposites();
	const bool idleBlanked = MainCompositeIsBlank();

	bool refRestored = false;
	if (idleBlanked) {
		g_canvasRuntime->AddPreview(g_canvases.Default().uuid);
		WaitTwoComposites();
		refRestored = !MainCompositeIsBlank();
		g_canvasRuntime->RemovePreview(g_canvases.Default().uuid);
	}

	// Main had a consumer, so the gated state never occurred on its own. Synthesize it at
	// the libobs level instead: this still covers the mechanism that makes a screenshot
	// blank -- an elided composite leaves texture_rendered false and
	// obs_render_main_texture draws nothing -- but not the frontend wiring that decides
	// when to elide, which needs a genuinely idle Main to exercise.
	bool synthBlanked = false;
	bool synthRestored = false;
	if (!idleBlanked) {
		OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
		if (mainCanvas) {
			obs_canvas_set_render_gated(mainCanvas, true);
			WaitTwoComposites();
			synthBlanked = MainCompositeIsBlank();
			obs_canvas_set_render_gated(mainCanvas, false);
			WaitTwoComposites();
			synthRestored = !MainCompositeIsBlank();
		}
	}

	obs_sceneitem_remove(item);
	VideoGate::Reconcile(); // hand the gate back to the authoritative predicate

	if (idleBlanked) {
		HostLog(std::string("[selftest] screenshot-gate -> idle capture blank, Default ref ") +
			(refRestored ? "restored the composite (OK)"
				     : "did NOT restore the composite (BUG: screenshots stay blank)"));
		return;
	}
	HostLog(std::string("[selftest] screenshot-gate -> Main had a consumer, so the frontend's "
			    "idle path was not exercised; synthesized elision ") +
		(synthBlanked ? "blanked the capture" : "did NOT blank the capture (BUG: elision is inert)") +
		" and ungating " + (synthRestored ? "restored it (OK)" : "did NOT restore it (BUG)"));
}

void ObsBootstrap::RunCalendarSelfTest()
{
	using Bridge::json;

	json result;
	std::string error;
	if (!Bridge::Dispatch("sessions.list", json(nullptr), result, error)) {
		HostLog("[selftest] calendar FAILED: " + Err::Diagnostic(error));
		return;
	}
	if (!result.is_array()) {
		HostLog("[selftest] calendar MISMATCH: sessions.list did not return an array");
		return;
	}
	HostLog("[selftest] calendar OK: " + std::to_string(result.size()) + " session(s), db " +
		(g_sessions.IsAttached() ? "attached" : "unavailable"));
}

void ObsBootstrap::RunScheduleSelfTest()
{
	using Bridge::json;

	json before;
	std::string error;
	if (!Bridge::Dispatch("schedule.list", json(nullptr), before, error)) {
		HostLog("[selftest] schedule FAILED: " + Err::Diagnostic(error));
		return;
	}
	if (!before.is_array()) {
		HostLog("[selftest] schedule MISMATCH: schedule.list did not return an array");
		return;
	}
	const size_t baseline = before.size();

	// A year out, so the runner never arms it. Arming rewrites the user's real output
	// bindings and per-destination metadata, which a smoke run must not do; the arm,
	// cancel and revert state machine is covered by test_history against an injected
	// clock, where there is no real configuration to damage.
	json params = json::object();
	params["startsAt"] = TimeUtil::NowMs() + 365LL * 24 * 60 * 60 * 1000;
	params["title"] = "braidcast self-test";

	json created;
	if (!Bridge::Dispatch("schedule.create", params, created, error)) {
		HostLog("[selftest] schedule.create FAILED: " + Err::Diagnostic(error));
		return;
	}
	const std::string id = created.is_object() ? created.value("id", std::string()) : std::string();
	if (id.empty()) {
		HostLog("[selftest] schedule MISMATCH: schedule.create returned no id; a row may have leaked");
		return;
	}

	json listed;
	const bool grew = Bridge::Dispatch("schedule.list", json(nullptr), listed, error) && listed.is_array() &&
			  listed.size() == baseline + 1;

	// The row is in the user's real history.db, reached through the rundir config
	// junction, so the delete runs whatever the assertions above found.
	json deleteParams = json::object();
	deleteParams["id"] = id;
	json deleted;
	std::string deleteError;
	const bool removed = Bridge::Dispatch("schedule.delete", deleteParams, deleted, deleteError);

	json after;
	const bool restored = Bridge::Dispatch("schedule.list", json(nullptr), after, error) && after.is_array() &&
			      after.size() == baseline;

	HostLog(std::string("[selftest] schedule create/list/delete -> ") +
		((grew && removed && restored) ? "OK" : "MISMATCH") + " (baseline=" + std::to_string(baseline) +
		", grew=" + (grew ? "yes" : "no") + ", removed=" + (removed ? "yes" : "no") +
		", restored=" + (restored ? "yes" : "no") + ")");
	if (!removed) {
		HostLog("[selftest] schedule LEAKED entry " + id + ": " + Err::Diagnostic(deleteError));
	}
}

void ObsBootstrap::RunStatsSelfTest()
{
	using Bridge::json;

	json result;
	std::string error;
	if (!Bridge::Dispatch("stats.get", json(nullptr), result, error)) {
		HostLog("[selftest] stats.get FAILED: " + Err::Diagnostic(error));
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

	json resetResult;
	std::string resetError;
	const bool resetOk = Bridge::Dispatch("stats.reset", json(nullptr), resetResult, resetError) &&
			     resetResult.is_object() && resetResult.value("ok", false);
	HostLog(std::string("[selftest] stats.reset -> ") +
		(resetOk ? "OK" : ("FAILED: " + Err::Diagnostic(resetError))));
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

void ObsBootstrap::RunDevToolsPortSelfTest()
{
	using Bridge::json;

	const uint16_t resolved = DevToolsPort::Resolve();
	const uint16_t active = DevToolsPort::Active();

	// Resolve() re-reads the environment, so comparing it against itself would prove
	// nothing. The independent fact is that the opt-in lives in one variable: with
	// BRAIDCAST_DEBUG_COMPONENTS absent there is no token to name 'devtools', so a
	// non-zero resolution means something outside that variable opened the port.
	const bool optInNamed = Env::IsSet("BRAIDCAST_DEBUG_COMPONENTS");
	const bool closedWithoutOptIn = optInNamed || resolved == 0;
	// main.cpp resolved the port before CefInitialize and stashed it; a mismatch here
	// means the boot path and the accessor read different state.
	const bool stashAgrees = active == resolved;

	json diag;
	std::string error;
	const bool got = Bridge::Dispatch("diagnostics.get", json(nullptr), diag, error);
	const bool fieldPresent = got && diag.is_object() && diag.contains("devToolsPort") &&
				  diag["devToolsPort"].is_number_unsigned();
	const bool fieldAgrees = fieldPresent && diag["devToolsPort"].get<uint16_t>() == active;
	// The one direction that must never fail, stated on its own so a regression names
	// itself: a gate that authorized a port must never surface as "no port".
	const bool exposureNotUnderReported = active == 0 ||
					      (fieldPresent && diag["devToolsPort"].get<uint16_t>() != 0);

	// The opt-in-named check above is only as strong as this machine's environment: with
	// BRAIDCAST_DEBUG_COMPONENTS set (as .env sets it) it passes whatever Resolve()
	// returned. These assert the grammar itself, over literals, so they hold on any
	// machine and would catch a token quietly gaining the devtools capability.
	const bool devToolsTokenExclusive =
		!Log::ParseComponents("render").devTools && !Log::ParseComponents("basic").devTools &&
		!Log::ParseComponents("all").devTools && !Log::ParseComponents("gpudiag").devTools &&
		Log::ParseComponents("devtools").devTools;
	const bool masterGrammarStrict = !StringUtil::ParseBool("") && !StringUtil::ParseBool("0") &&
					 !StringUtil::ParseBool("false") && !StringUtil::ParseBool("maybe") &&
					 StringUtil::ParseBool("TRUE") && StringUtil::ParseBool(" on ");

	HostLog(std::string("[selftest] devtools grammar -> 'devtools' token exclusive=") +
		(devToolsTokenExclusive ? "OK" : "false (BUG)") + "; master flag strict=" +
		(masterGrammarStrict ? "OK" : "false (BUG)") + ". Environment-independent: neither reads a variable.");

	HostLog("[selftest] devtools resolve=" + std::to_string(resolved) + " active=" + std::to_string(active) +
		" components-var=" + (optInNamed ? "set" : "absent") +
		" -> closed-without-opt-in=" + (closedWithoutOptIn ? "OK" : "false (BUG)") +
		"; accessor-agrees=" + (stashAgrees ? "OK" : "false (BUG)") + "; diagnostics.get devToolsPort=" +
		(fieldAgrees ? "OK" : (fieldPresent ? "mismatch (BUG)" : "MISSING (BUG)")) +
		"; exposure-not-under-reported=" + (exposureNotUnderReported ? "OK" : "false (BUG)") +
		". Proves the resolution and the seams that read it ONLY -- whether a socket is "
		"listening is not asserted by anything, here or at boot. Nothing checks it: this "
		"runs inside the process that owns the port, so a probe from here could not "
		"answer the question independently anyway.");
}

void ObsBootstrap::RunEventSelfTest()
{
	// Snapshot the user's real events.json (+ its .bak) so the synthetic ingests below
	// can exercise the persist / dedupe / round-trip path without clobbering real
	// history; restored verbatim at the end (mirrors the model self-test discipline).
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
	const std::optional<std::string> origMain = FileUtil::ReadUtf8File(path);
	const std::optional<std::string> origBak = FileUtil::ReadUtf8File(bakPath);

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

	// Persistence round-trip: a fresh store loads the saved events.json. The Flush is
	// not test scaffolding -- Add coalesces writes on a 3s debounce and Clear() above
	// stamps that timer, so all three Ingests land inside one window and leave the feed
	// dirty in memory with the empty file Clear wrote still on disk. Flush is exactly
	// how the app persists that trailing state (bridge shutdown does the same call), so
	// reading back without it asserts against a write the product never promised.
	Events::Store().Flush();
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
		// Same again for the per-destination release: the stores it reads are cleared just
		// below, and nothing needs its quota saved while the process is exiting.
		g_multistream->onOutputEnded = nullptr;
		g_multistream->StopAll();
		g_multistream.reset();
		// The publish seam is gone; nothing is live once the engine is down.
		g_anyOutputLive.store(false, std::memory_order_release);
	}

	// History last among the seams above, once nothing can fire into it: the
	// callbacks are disconnected and the sampler's observer is dropped here.
	Bridge::SetStatsTickObserver(nullptr);
	if (g_recorder.IsRecording()) {
		// A clean shutdown mid-broadcast is still a clean end. Leaving the row
		// open would have the next launch report a crash that never happened,
		// which is worse than useless -- it is a false alarm in exactly the
		// signal the feature exists to provide.
		EndSessionWithThumbnail("ended");
	}
	g_recorder.Detach();
	g_scheduleRunner.Detach();
	g_sessions.Detach();
	g_schedule.Detach();
	g_historyDb.Close();

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
	MainChannel::Set(nullptr);
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

	// Dump the profiler tree while libobs is still up: a snapshot borrows the node
	// name pointers rather than copying them, and the name store most of them live
	// in is freed by obs_shutdown. Stop first so the tree stops growing under the
	// snapshot.
	if (g_profilerStarted) {
		profiler_stop();
		profiler_snapshot_t *snap = profile_snapshot_create();
		profiler_print(snap);
		profile_snapshot_free(snap);
	}

	obs_shutdown();

	// After obs_shutdown: profiler_free destroys the profiler's root mutex, so it
	// must not run while the graphics and audio threads (joined above) can still
	// profile. Before the leak count, so the profiler's own allocations are not
	// reported as outstanding.
	if (g_profilerStarted) {
		profiler_free();
		g_profilerStarted = false;
	}

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
