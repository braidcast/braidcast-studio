#include "harness.hpp"
#include "testseam.hpp"

#include "scene_collections.hpp"
#include "scene_persistence.hpp"

#include "multistream/CanvasRuntime.hpp"
#include "multistream/CanvasStore.hpp"

#include <CanvasDefinition.hpp>

#include <obs.h>
#include <obs.hpp>
#include <util/platform.h>
#include <util/bmem.h>

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

// The single production instances the TestSeam shim hands back through the
// ObsBootstrap accessors. Constructed here (not in obs_bootstrap.cpp) so the test
// links the persistence layer without the CEF app. CanvasStore seeds its Default
// in its ctor, so it is valid before obs comes up; the runtime is built after
// obs_reset_video and destroyed before obs_shutdown.
CanvasStore g_canvases;
std::unique_ptr<CanvasRuntime> g_runtime;
SceneCollections g_collections;
std::string g_scenePath;

// The harness owns every source a test creates so tests never juggle refs across a
// Save->teardown->reload cycle. Cleared (with channel 0 unbound) in TeardownWorld,
// which guarantees the old world is fully destroyed before Load rebuilds it -- no
// removed-but-alive scene lingers to be enumerated twice or collide by uuid.
std::vector<OBSSourceAutoRelease> g_created;

bool g_booted = false;
bool g_firstSceneBound = false;

std::string ExeDir()
{
	char buf[MAX_PATH] = {};
	const DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
	if (n == 0 || n >= sizeof(buf)) {
		return std::string();
	}
	return std::filesystem::path(buf).parent_path().u8string();
}

// Remove every additional (non-Default) canvas from both the runtime and the
// model so the next test starts with just the Default.
void ClearAdditionalCanvases()
{
	std::vector<std::string> uuids;
	for (const CanvasDefinition &def : g_canvases.Definitions()) {
		if (!def.isDefault) {
			uuids.push_back(def.uuid);
		}
	}
	for (const std::string &uuid : uuids) {
		if (g_runtime) {
			g_runtime->RemoveCanvas(uuid);
		}
		g_canvases.Remove(uuid);
	}
}

} // namespace

// --- TestSeam bindings (consumed by bootstrap_shim.cpp) ---------------------

CanvasStore &TestSeam::Canvases()
{
	return g_canvases;
}

CanvasRuntime &TestSeam::Runtime()
{
	return *g_runtime;
}

SceneCollections &TestSeam::Collections()
{
	return g_collections;
}

const std::string &TestSeam::ActiveScenePath()
{
	return g_scenePath;
}

void TestSeam::SetActiveScenePath(const std::string &path)
{
	g_scenePath = path;
}

// --- Harness ----------------------------------------------------------------

std::string Harness::DataPath()
{
	// The test exe lands in the OBS rundir bin dir (…/bin/64bit); the libobs
	// data (default effects, needed by obs_reset_video) sits at …/data/libobs.
	std::filesystem::path rundir = std::filesystem::u8path(ExeDir()).parent_path().parent_path();
	return (rundir / "data" / "libobs").u8string() + "/";
}

bool Harness::BootObs()
{
	if (g_booted) {
		return true;
	}

	if (!obs_startup("en-US", nullptr, nullptr)) {
		blog(LOG_ERROR, "[harness] obs_startup failed");
		return false;
	}

#pragma warning(push)
#pragma warning(disable : 4996)
	obs_add_data_path(DataPath().c_str());
#pragma warning(pop)

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
		blog(LOG_ERROR, "[harness] obs_reset_video failed (code=%d); need a D3D11 device (WARP is fine)", rv);
		obs_shutdown();
		return false;
	}

	obs_audio_info oai = {};
	oai.samples_per_sec = 48000;
	oai.speakers = SPEAKERS_STEREO;
	if (!obs_reset_audio(&oai)) {
		blog(LOG_ERROR, "[harness] obs_reset_audio failed");
		obs_shutdown();
		return false;
	}

	g_runtime = std::make_unique<CanvasRuntime>(g_canvases);
	g_booted = true;
	return true;
}

void Harness::ShutdownObs()
{
	if (!g_booted) {
		return;
	}
	// Drop the runtime (destroys obs_canvas mixes) while libobs is still up.
	g_runtime.reset();
	obs_shutdown();
	// Dump whatever survived shutdown (no-op unless OBS_TRACK_ALLOCS=1).
	bmem_dump_outstanding();
	g_booted = false;
}

void Harness::BeginTest()
{
	g_firstSceneBound = false;

	// A fresh, inspectable temp dir per test so persisted files are isolated and
	// nothing touches the user's real profile. Uniqueness: pid + a monotonic
	// counter so parallel/repeated runs never collide.
	static std::atomic<uint32_t> counter{0};
	char tmp[MAX_PATH] = {};
	GetTempPathA(sizeof(tmp), tmp);
	std::filesystem::path dir = std::filesystem::u8path(tmp) /
				    ("braidcast_it_" + std::to_string(GetCurrentProcessId()) + "_" +
				     std::to_string(counter.fetch_add(1)));
	os_mkdirs(dir.u8string().c_str());

	g_scenePath = (dir / "collection.json").u8string();
	TestSeam::SetActiveScenePath(g_scenePath);

	// Start from a clean world regardless of what a prior test left.
	TeardownWorld();
	ClearAdditionalCanvases();
}

void Harness::EndTest()
{
	TeardownWorld();
	ClearAdditionalCanvases();

	if (!g_scenePath.empty()) {
		std::error_code ec;
		std::filesystem::remove_all(std::filesystem::u8path(g_scenePath).parent_path(), ec);
	}
	g_scenePath.clear();
}

const std::string &Harness::ScenePath()
{
	return g_scenePath;
}

obs_source_t *Harness::CreateMainScene(const char *name)
{
	obs_scene_t *scene = obs_scene_create(name);
	if (!scene) {
		return nullptr;
	}
	obs_source_t *src = obs_scene_get_source(scene); // borrowed
	g_created.emplace_back(obs_source_get_ref(src)); // harness takes the owning ref

	// Bind the first scene of a test to channel 0 so it is the "current" scene
	// Save records (and Load re-binds), mirroring the boot default scene.
	if (!g_firstSceneBound) {
		obs_set_output_source(0, src);
		g_firstSceneBound = true;
	}

	obs_scene_release(scene); // drop the create-ref; g_created holds it now
	return src;               // borrowed; the harness owns the lifetime
}

std::string Harness::AddCanvas(uint32_t width, uint32_t height)
{
	CanvasDefinition def;
	def.name = "Test Canvas";
	def.isDefault = false;
	def.width = width;
	def.height = height;
	def.fpsNum = 60;
	def.fpsDen = 1;

	const CanvasDefinition &added = g_canvases.Add(std::move(def)); // assigns uuid
	const std::string uuid = added.uuid;

	g_runtime->EnsureCanvas(added);
	// Force a live mix the way an open preview would, so the canvas behaves like a
	// production canvas with a surface (VideoFor non-null); dropped by RemoveCanvas.
	g_runtime->AddPreview(uuid);
	return uuid;
}

obs_source_t *Harness::CreateCanvasScene(const std::string &canvasUuid, const char *name)
{
	obs_source_t *scene = g_runtime->CreateScene(canvasUuid, name); // addref'd or null
	if (!scene) {
		return nullptr;
	}
	g_created.emplace_back(scene);                // harness takes the owning ref
	g_runtime->SetCurrentScene(canvasUuid, name); // bind as the canvas's current scene
	return scene;                                 // borrowed; the harness owns the lifetime
}

void Harness::Save()
{
	SceneCollection::Save(g_scenePath);
}

bool Harness::Load()
{
	return SceneCollection::Load(g_scenePath);
}

void Harness::TeardownWorld()
{
	// ClearCurrent's contract requires the caller to have unbound channel 0 first
	// (production does this via Transitions::Shutdown; the fast slice has no
	// transition). Then drop every harness-owned ref so the removed sources actually
	// destroy in ClearCurrent's drain, leaving a clean world before any reload.
	obs_set_output_source(0, nullptr);
	g_created.clear();
	SceneCollection::ClearCurrent();
	g_firstSceneBound = false;
}

std::vector<std::string> Harness::CanvasSceneNames(const std::string &canvasUuid)
{
	std::vector<std::string> out;
	if (!g_runtime) {
		return out;
	}
	for (const CanvasRuntime::SceneInfo &s : g_runtime->Scenes(canvasUuid)) {
		out.push_back(s.name);
	}
	return out;
}

obs_source_t *Harness::CanvasSceneSource(const std::string &canvasUuid, const char *name)
{
	obs_canvas_t *canvas = g_runtime ? g_runtime->Find(canvasUuid) : nullptr;
	if (!canvas) {
		return nullptr;
	}
	return obs_canvas_get_source_by_name(canvas, name); // addref'd; caller releases
}
