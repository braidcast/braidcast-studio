#include "obs_bootstrap.hpp"

#include <obs.h>
#include <obs-frontend-internal.hpp>
#include <util/base.h>

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "bridge.hpp"
#include "frontend_callbacks.hpp"
#include "log.hpp"
#include "paths.hpp"

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
void CreateDefaultScene()
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
		return;
	}
	HostLog("[obs] default color source created (Placeholder Background)");

	g_scene = obs_scene_create("Default Scene");
	if (!g_scene) {
		HostLog("[obs] obs_scene_create failed");
		obs_source_release(source);
		return;
	}

	obs_scene_add(g_scene, source);
	obs_source_release(source); // scene owns the create-ref now

	obs_set_output_source(0, obs_scene_get_source(g_scene));
	HostLog("[obs] default scene bound to output channel 0");
}

} // namespace

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

	LoadCuratedModules();

	obs_post_load_modules();
	HostLog("[obs] core up (curated full-set load)");

	// Lifecycle signal plugins' registered handlers expect post-load.
	if (g_frontend) {
		g_frontend->on_event(OBS_FRONTEND_EVENT_FINISHED_LOADING);
	}

	RunProbes();

	CreateDefaultScene();

	return true;
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

void ObsBootstrap::Stop()
{
	// Drop the bridge's obs frontend event callback while libobs is still up.
	Bridge::Shutdown();

	// Deferred source destruction can cascade across the destruction-task
	// thread; drain in a loop until no more work is spawned before
	// obs_shutdown, mirroring the Qt frontend's ClearSceneData.
	while (obs_wait_for_destroy_queue()) {
	}

	obs_shutdown();

	// Same counter the legacy frontend prints. Nonzero == fixed libobs static
	// residuals (no per-run growth), not host-introduced leaks.
	HostLog("[obs] leaks: " + std::to_string(bnum_allocs()));
	HostLog("[obs] shutdown complete");
}
