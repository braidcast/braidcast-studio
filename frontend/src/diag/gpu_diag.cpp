#include "gpu_diag.hpp"

#include <obs.h>
#include <util/platform.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "../log.hpp"
#include "../obs_bootstrap.hpp"

namespace {

// The obs-browser OSR source id (plugins/obs-browser registers "browser_source").
// Match the unversioned id so a versioned "browser_source_v2" would still count.
constexpr char kBrowserSourceId[] = "browser_source";

constexpr uint64_t kBytesPerMiB = 1024ull * 1024ull;
constexpr int kDefaultIntervalMs = 5000;
constexpr int kMinIntervalMs = 250;

// Env is "true" when present, non-empty, and not the literal "0". Keeps the same
// permissive =1 / =on convention the rest of the shell's env flags use.
bool EnvEnabled(const char *name)
{
	const char *v = getenv(name);
	return v != nullptr && *v != '\0' && strcmp(v, "0") != 0;
}

// --- kill switch ---------------------------------------------------------------

// Overlay a blank, tiny page onto a browser source so its CEF OSR browser renders
// essentially no tiles (removing the GPU raster/compositing load) without
// destroying the obs_source or its scene item. Video-source obs_source_update only
// merges settings + flags a deferred update (see obs-source.c), so this is safe
// from the source_create signal callback -- no synchronous rebuild, no reentrancy.
void NeutralizeBrowserSource(obs_source_t *source)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", false);
	obs_data_set_string(settings, "local_file", "");
	obs_data_set_string(settings, "url", "about:blank");
	obs_data_set_int(settings, "width", 16);
	obs_data_set_int(settings, "height", 16);
	obs_data_set_bool(settings, "fps_custom", false);
	obs_data_set_int(settings, "fps", 1);
	obs_source_update(source, settings);
	obs_data_release(settings);
}

const char *SafeName(obs_source_t *source)
{
	const char *name = obs_source_get_name(source);
	return name != nullptr ? name : "?";
}

bool IsBrowserSource(obs_source_t *source)
{
	const char *id = obs_source_get_unversioned_id(source);
	return id != nullptr && strcmp(id, kBrowserSourceId) == 0;
}

// obs_enum_sources sweep: neutralize browser sources that already exist when the
// kill switch arms (loaded by the initial scene collection inside Start()).
bool SweepNeutralize(void * /*param*/, obs_source_t *source)
{
	if (IsBrowserSource(source)) {
		NeutralizeBrowserSource(source);
		HostLog(std::string("[gpudiag] kill-switch: neutralized existing browser source \"") +
			SafeName(source) + "\"");
	}
	return true;
}

// Global "source_create" signal: neutralize browser sources created after the
// sweep (scene switches, bridge-driven adds). calldata carries the new source.
void OnSourceCreate(void * /*data*/, calldata_t *cd)
{
	auto *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (source == nullptr || !IsBrowserSource(source)) {
		return;
	}
	NeutralizeBrowserSource(source);
	HostLog(std::string("[gpudiag] kill-switch: neutralized new browser source \"") + SafeName(source) + "\"");
}

bool g_hookConnected = false;

// --- sampler -------------------------------------------------------------------

struct BrowserTally {
	int count = 0;
	std::string detail; // "1920x1080 \"Chat\", 800x600 \"Alerts\""
};

bool TallyBrowsers(void *param, obs_source_t *source)
{
	if (!IsBrowserSource(source)) {
		return true;
	}
	auto *tally = static_cast<BrowserTally *>(param);
	if (tally->count > 0) {
		tally->detail += ", ";
	}
	tally->detail += std::to_string(obs_source_get_width(source));
	tally->detail += "x";
	tally->detail += std::to_string(obs_source_get_height(source));
	tally->detail += " \"";
	tally->detail += SafeName(source);
	tally->detail += "\"";
	tally->count++;
	return true;
}

struct OutputTally {
	int active = 0;
	std::string names;
};

bool TallyActiveOutputs(void *param, obs_output_t *output)
{
	if (!obs_output_active(output)) {
		return true;
	}
	auto *tally = static_cast<OutputTally *>(param);
	if (tally->active > 0) {
		tally->names += ", ";
	}
	const char *name = obs_output_get_name(output);
	tally->names += name != nullptr ? name : "?";
	tally->active++;
	return true;
}

// Last sampled live-output count; drives the go-live / stop edge markers. Only
// touched from the sampler thread.
int g_prevActiveOutputs = 0;

void SampleOnce()
{
	BrowserTally browsers;
	obs_enum_sources(&TallyBrowsers, &browsers);

	OutputTally outputs;
	obs_enum_outputs(&TallyActiveOutputs, &outputs);

	const uint64_t rssMiB = os_get_proc_resident_size() / kBytesPerMiB;

	// Edge-detect go-live / stop off the live-output count so the marker needs no
	// MultistreamEngine hook (multistream go-live never fires frontend streaming
	// events; output start/stop signals are per-output only). Within one interval
	// of the real transition -- enough to bracket the CEF debug.log crash window.
	if (outputs.active > 0 && g_prevActiveOutputs == 0) {
		HostLog("[gpudiag] --- stream start detected: " + std::to_string(outputs.active) +
			" output(s) live ---");
	} else if (outputs.active == 0 && g_prevActiveOutputs > 0) {
		HostLog("[gpudiag] --- stream stopped ---");
	}
	g_prevActiveOutputs = outputs.active;

	std::string line = "[gpudiag] osr_browsers=" + std::to_string(browsers.count);
	if (browsers.count > 0) {
		line += " [" + browsers.detail + "]";
	}
	line += " outputs_live=" + std::to_string(outputs.active);
	if (outputs.active > 0) {
		line += " [" + outputs.names + "]";
	}
	line += " rss=" + std::to_string(rssMiB) + "MiB";
	HostLog(line);
}

std::thread g_sampler;
std::atomic<bool> g_running{false};
std::mutex g_wakeMtx;
std::condition_variable g_wake;

void SamplerLoop(int intervalMs)
{
	HostLog("[gpudiag] sampler started (interval=" + std::to_string(intervalMs) + "ms)");
	std::unique_lock<std::mutex> lock(g_wakeMtx);
	while (g_running.load()) {
		lock.unlock();
		SampleOnce();
		lock.lock();
		g_wake.wait_for(lock, std::chrono::milliseconds(intervalMs), [] { return !g_running.load(); });
	}
	HostLog("[gpudiag] sampler stopped");
}

} // namespace

bool GpuDiag::BrowserSourcesDisabled()
{
	static const bool disabled = EnvEnabled("BRAIDCAST_DISABLE_BROWSER_SOURCES");
	return disabled;
}

void GpuDiag::InstallBrowserSourceKillSwitch()
{
	if (!BrowserSourcesDisabled() || g_hookConnected) {
		return;
	}
	g_hookConnected = true;
	obs_enum_sources(&SweepNeutralize, nullptr);
	signal_handler_connect(obs_get_signal_handler(), "source_create", &OnSourceCreate, nullptr);
	HostLog("[gpudiag] kill-switch armed: obs-browser OSR sources neutralized "
		"(BRAIDCAST_DISABLE_BROWSER_SOURCES); scene collection will NOT be saved this run");
}

void GpuDiag::Start()
{
	if (!ObsBootstrap::GpuDiagRequested() || g_running.load()) {
		return;
	}
	int intervalMs = kDefaultIntervalMs;
	if (const char *spec = getenv("BRAIDCAST_GPUDIAG_INTERVAL_MS")) {
		const int parsed = atoi(spec);
		if (parsed >= kMinIntervalMs) {
			intervalMs = parsed;
		}
	}
	g_running.store(true);
	g_sampler = std::thread(SamplerLoop, intervalMs);
}

void GpuDiag::Stop()
{
	if (g_running.exchange(false)) {
		// Serialize with the sampler between its predicate check and wait_for so the
		// stop is never lost: taking the wake mutex guarantees the notify below is
		// observed even if the sampler is about to sleep.
		{
			std::lock_guard<std::mutex> lock(g_wakeMtx);
		}
		g_wake.notify_all();
		if (g_sampler.joinable()) {
			g_sampler.join();
		}
	}
	if (g_hookConnected) {
		signal_handler_disconnect(obs_get_signal_handler(), "source_create", &OnSourceCreate, nullptr);
		g_hookConnected = false;
	}
}
