#include "capture_rate_selftest.hpp"

#include <windows.h>
#include <dwmapi.h>

#include <obs.hpp>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "bridge.hpp"
#include "diag/capture_rate_sampler.hpp"
#include "log.hpp"
#include "multistream/CanvasRuntime.hpp"
#include "multistream/CanvasStore.hpp"
#include "multistream/VideoGate.hpp"
#include "obs_bootstrap.hpp"
#include "util/selftest_paths.hpp"

namespace {

using json = Bridge::json;
using Clock = std::chrono::steady_clock;

constexpr const char *kTag = "[selftest-stream] capture-rate";
constexpr const char *kAsyncId = "braidcast_selftest_async";
constexpr const char *kWgcName = "caprate-wgc";
constexpr const char *kDxgiName = "caprate-dxgi";
constexpr const char *kDxgi2Name = "caprate-dxgi2";
constexpr const char *kAsyncName = "caprate-async";
// On a non-Default canvas, which nothing else reaches: only the canvas walk can
// find them, as it has to for a Shorts destination.
constexpr const char *kCanvasDxgiName = "caprate-canvas-dxgi";
constexpr const char *kGameName = "caprate-game";

// win-capture's display_capture_method values.
constexpr int kMethodDxgi = 1;
constexpr int kMethodWgc = 2;

// Phase lengths. Half cadence has to outlast the tracker's 5 s grace plus its
// 10 s lock streak, with room for the sampler's 1 Hz phase against ours.
constexpr std::chrono::seconds kWaitKinds{10};
constexpr std::chrono::seconds kFullRate{8};
constexpr std::chrono::seconds kHalfRate{20};
constexpr std::chrono::seconds kJoinAfter{2};
// The static phases read the real desktop, and the taskbar stays above the
// flicker window, so they run long enough for a median to ride out a stray update.
constexpr std::chrono::seconds kStatic{8};
constexpr std::chrono::seconds kHide{8};
constexpr std::chrono::seconds kReshow{4};
constexpr std::chrono::seconds kSwitch{5};
constexpr std::chrono::seconds kReload{6};

// Under half cadence the true rate is about half the canvas. A missing baseline
// shows up as a sample reading near the full rate, so anything past this share
// of the main rate is a spike.
constexpr double kSpikeShare = 0.7;
constexpr double kStaticMaxRate = 5.0;

// ---------------------------------------------------------------------------
// A full-screen window over the primary monitor that repaints right before a
// chosen share of compositor frames, so the desktop changes at a known rate.

class Flicker {
public:
	bool Start(const RECT &rc)
	{
		thread_ = std::thread([this, rc] { Run(rc); });
		for (int i = 0; i < 200 && !ready_.load() && !failed_.load(); i++) {
			Sleep(10);
		}
		return ready_.load();
	}

	// Repaint every `n`th compositor frame; 0 holds the screen still.
	void SetEvery(int n) { every_.store(n); }

	void Stop()
	{
		stop_.store(true);
		if (thread_.joinable()) {
			thread_.join();
		}
	}

	~Flicker() { Stop(); }

private:
	void Run(RECT rc)
	{
		static const wchar_t *kClass = L"BraidcastCaptureRateFlicker";
		WNDCLASSW wc = {};
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = kClass;
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		RegisterClassW(&wc); // a second run's re-registration fails harmlessly

		HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, L"", WS_POPUP,
					    rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr,
					    wc.hInstance, nullptr);
		if (!hwnd) {
			failed_.store(true);
			return;
		}
		ShowWindow(hwnd, SW_SHOWNOACTIVATE);
		HDC dc = GetDC(hwnd);
		RECT client;
		GetClientRect(hwnd, &client);
		HBRUSH brushes[2] = {CreateSolidBrush(RGB(40, 40, 40)), CreateSolidBrush(RGB(200, 200, 200))};
		FillRect(dc, &client, brushes[0]);
		ready_.store(true);

		uint64_t frame = 0;
		int shade = 0;
		while (!stop_.load()) {
			MSG msg;
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
			const int every = every_.load();
			if (every > 0 && frame % every == 0) {
				shade ^= 1;
				FillRect(dc, &client, brushes[shade]);
				GdiFlush();
			}
			frame++;
			if (FAILED(DwmFlush())) {
				Sleep(8);
			}
		}

		ReleaseDC(hwnd, dc);
		DeleteObject(brushes[0]);
		DeleteObject(brushes[1]);
		DestroyWindow(hwnd);
	}

	std::thread thread_;
	std::atomic<bool> stop_{false};
	std::atomic<bool> ready_{false};
	std::atomic<bool> failed_{false};
	std::atomic<int> every_{0};
};

// Circles the real cursor over a still screen; puts it back when stopped.
class CursorMover {
public:
	void Start(POINT center)
	{
		GetCursorPos(&saved_);
		thread_ = std::thread([this, center] {
			double angle = 0.0;
			while (!stop_.load()) {
				SetCursorPos(center.x + int(120 * std::cos(angle)),
					     center.y + int(120 * std::sin(angle)));
				angle += 0.2;
				Sleep(16);
			}
		});
	}

	void Stop()
	{
		if (!thread_.joinable()) {
			return;
		}
		stop_.store(true);
		thread_.join();
		SetCursorPos(saved_.x, saved_.y);
	}

	~CursorMover() { Stop(); }

private:
	std::thread thread_;
	std::atomic<bool> stop_{false};
	POINT saved_ = {};
};

// ---------------------------------------------------------------------------
// A private async video source and the producer thread that feeds it.

obs_source_info AsyncTestSourceInfo()
{
	obs_source_info info = {};
	info.id = kAsyncId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_CAP_DISABLED;
	info.get_name = [](void *) -> const char * {
		return "Capture-rate self-test async";
	};
	info.create = [](obs_data_t *, obs_source_t *source) -> void * {
		return source;
	};
	info.destroy = [](void *) {
	};
	return info;
}

void RegisterAsyncTestSource()
{
	static bool registered = false;
	if (!registered) {
		const obs_source_info info = AsyncTestSourceInfo();
		obs_register_source(&info);
		registered = true;
	}
}

enum class FeedMode { Steady, Bursty };

class Producer {
public:
	void Start(obs_source_t *source)
	{
		source_ = source;
		thread_ = std::thread([this] { Run(); });
	}

	void SetMode(FeedMode mode) { mode_.store(mode); }

	void Stop()
	{
		stop_.store(true);
		if (thread_.joinable()) {
			thread_.join();
		}
		source_ = nullptr;
	}

	~Producer() { Stop(); }

private:
	void Output(std::vector<uint8_t> &pixels, uint8_t shade)
	{
		std::fill(pixels.begin(), pixels.end(), shade);
		obs_source_frame frame = {};
		frame.data[0] = pixels.data();
		frame.linesize[0] = kSide * 4;
		frame.width = kSide;
		frame.height = kSide;
		frame.format = VIDEO_FORMAT_BGRA;
		frame.full_range = true;
		frame.timestamp = os_gettime_ns();
		obs_source_output_video(source_, &frame);
	}

	void Run()
	{
		std::vector<uint8_t> pixels(kSide * kSide * 4);
		uint8_t shade = 0;
		uint64_t next = os_gettime_ns();
		while (!stop_.load()) {
			if (mode_.load() == FeedMode::Steady) {
				Output(pixels, shade += 8);
				next += 1000000000ull / 30;
			} else {
				// A second's worth at once: the render can pick only one of them.
				for (int i = 0; i < 30; i++) {
					Output(pixels, shade += 8);
				}
				next += 1000000000ull;
			}
			if (next < os_gettime_ns()) {
				next = os_gettime_ns();
			}
			os_sleepto_ns(next);
		}
	}

	static constexpr uint32_t kSide = 16;
	OBSSource source_;
	std::thread thread_;
	std::atomic<bool> stop_{false};
	std::atomic<FeedMode> mode_{FeedMode::Steady};
};

// ---------------------------------------------------------------------------

enum class Phase {
	Idle,
	Settle,
	Setup,
	WaitKinds,
	FullRate,
	HalfRate,
	Static,
	Hide,
	Reshow,
	Switch,
	Reload,
	Finish,
	Done,
};

// One stats sample: capture rows by source name.
using Sample = std::map<std::string, json>;

struct Placed {
	OBSSource source;
	obs_sceneitem_t *item = nullptr;
	bool holdShowing = true; // false: shown only by the canvas rendering it
	bool showing = false;    // holds a VideoGate::IncShowing
};

struct State {
	Phase phase = Phase::Idle;
	Clock::time_point phaseStart;
	int exitCode = -1;
	std::vector<std::string> failures;
	std::vector<std::string> passes;
	std::vector<std::string> inconclusive; // the machine, not the code, decided these

	double mainFps = 0.0;
	int refreshHz = 60;
	RECT monitorRect = {};
	std::string monitorId;
	std::string defaultUuid;
	std::string canvasUuid; // the non-Default test canvas
	double canvasFps = 0.0;

	OBSScene scene;
	OBSScene canvasScene;
	std::map<std::string, Placed> placed; // by name
	std::unique_ptr<Flicker> flicker;
	std::unique_ptr<CursorMover> cursor;
	std::unique_ptr<Producer> producer;
	// Every check reads the real desktop, which a display gone to sleep stops
	// composing; held for the run so the idle timer cannot turn it off midway.
	os_inhibit_t *displayAwake = nullptr;
	json lastHalfRateCaptures; // the summary's example of a locked stats.get payload
	bool joined = false;
	bool reloaded = false;
	std::string reloadUuid;
	bool sessionOpen = false;
	int sessionLinesBefore = 0;
	Clock::time_point sessionBegan;
	std::string sessionLine;

	int64_t lastSampledAtMs = -1;
	std::map<Phase, std::vector<Sample>> samples;
	std::string lastLine;
};

State g_state;

void Say(const std::string &line)
{
	HostLog(std::string(kTag) + " " + line);
}

void Check(State &st, bool ok, const std::string &what)
{
	Say(std::string(ok ? "PASS " : "FAIL ") + what);
	(ok ? st.passes : st.failures).push_back(what);
}

void Inconclusive(State &st, const std::string &what)
{
	Say("INCONCLUSIVE " + what);
	st.inconclusive.push_back(what);
}

void Enter(State &st, Phase phase)
{
	st.phase = phase;
	st.phaseStart = Clock::now();
}

Clock::duration InPhase(const State &st)
{
	return Clock::now() - st.phaseStart;
}

// The primary monitor's device interface id, the value monitor_capture keys on.
bool PrimaryMonitor(State &st)
{
	const POINT origin = {0, 0};
	HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFOEXA mi = {};
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoA(mon, reinterpret_cast<LPMONITORINFO>(&mi))) {
		return false;
	}
	st.monitorRect = mi.rcMonitor;
	DISPLAY_DEVICEA dev = {};
	dev.cb = sizeof(dev);
	if (!EnumDisplayDevicesA(mi.szDevice, 0, &dev, EDD_GET_DEVICE_INTERFACE_NAME)) {
		return false;
	}
	st.monitorId = dev.DeviceID;
	DEVMODEA mode = {};
	mode.dmSize = sizeof(mode);
	if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1) {
		st.refreshHz = int(mode.dmDisplayFrequency);
	}
	return true;
}

// The scene on channel 0, looking through a transition to its active scene.
OBSScene MainScene()
{
	OBSSourceAutoRelease out = obs_get_output_source(0);
	if (!out) {
		return nullptr;
	}
	if (obs_source_get_type(out) == OBS_SOURCE_TYPE_TRANSITION) {
		OBSSourceAutoRelease active = obs_transition_get_active_source(out);
		return OBSScene(obs_scene_from_source(active));
	}
	return OBSScene(obs_scene_from_source(out));
}

void Show(Placed &p)
{
	if (p.holdShowing && !p.showing) {
		VideoGate::IncShowing(p.source);
		p.showing = true;
	}
	if (p.item) {
		obs_sceneitem_set_visible(p.item, true);
	}
}

void Hide(Placed &p)
{
	if (p.item) {
		obs_sceneitem_set_visible(p.item, false);
	}
	if (p.showing) {
		VideoGate::DecShowing(p.source);
		p.showing = false;
	}
}

void PlaceIn(State &st, const std::string &name, obs_source_t *source, obs_scene_t *scene, bool holdShowing)
{
	Placed p;
	p.source = source;
	p.holdShowing = holdShowing;
	p.item = obs_scene_add(scene, source);
	Show(p);
	st.placed[name] = std::move(p);
}

// Adds `source` to the main scene and holds it showing, so the Main root reaches it
// (which is what gives it a reference rate) and it renders even with Main idle.
void Place(State &st, const std::string &name, obs_source_t *source)
{
	PlaceIn(st, name, source, st.scene, true);
}

// Adds `source` to the test canvas's scene with no showing hold, so no showing root
// reaches it: it shows only because that canvas renders it.
void PlaceOnCanvas(State &st, const std::string &name, obs_source_t *source)
{
	PlaceIn(st, name, source, st.canvasScene, false);
}

// Declares which canvases are live to the sampler alone; this run must not broadcast.
void SetLiveCanvases(const State &st, bool canvasLive)
{
	const std::string defaultUuid = st.defaultUuid;
	const std::string canvasUuid = canvasLive ? st.canvasUuid : std::string();
	ObsBootstrap::CaptureRates().SetCanvasLiveOverrideForTest([defaultUuid, canvasUuid](const std::string &uuid) {
		return uuid == defaultUuid || (!canvasUuid.empty() && uuid == canvasUuid);
	});
}

void Unplace(State &st, const std::string &name)
{
	auto it = st.placed.find(name);
	if (it == st.placed.end()) {
		return;
	}
	Hide(it->second);
	if (it->second.item) {
		obs_sceneitem_remove(it->second.item);
	}
	st.placed.erase(it);
}

// A game capture pointed at a window that does not exist, so it never hooks
// anything (least of all the flicker window): showing, with no size, and counting
// nothing.
OBSSourceAutoRelease CreateGameCapture(const char *name)
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "capture_mode", "window");
	obs_data_set_string(settings, "window", "caprate-none:CaprateNone:caprate-none.exe");
	return obs_source_create_private(CaptureRate::kGameCaptureId, name, settings);
}

// The test canvas's frame rate and the scene on its channel 0. The rate is its video
// output's, as the sampler reads it: libobs runs every mix's composite at Main's
// rate, so obs_canvas_get_video_info reports Main's.
bool CanvasSetup(State &st)
{
	obs_canvas_t *canvas = ObsBootstrap::CanvasRuntime().Find(st.canvasUuid);
	video_t *video = ObsBootstrap::CanvasRuntime().VideoFor(st.canvasUuid);
	const struct video_output_info *info = video ? video_output_get_info(video) : nullptr;
	if (!canvas || !info || !info->fps_den) {
		return false;
	}
	st.canvasFps = double(info->fps_num) / info->fps_den;
	OBSSourceAutoRelease channel = obs_canvas_get_channel(canvas, 0);
	st.canvasScene = OBSScene(obs_scene_from_source(channel));
	return st.canvasScene != nullptr;
}

OBSSourceAutoRelease CreateMonitorCapture(const State &st, const char *name, int method)
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "monitor_id", st.monitorId.c_str());
	obs_data_set_int(settings, "method", method);
	obs_data_set_bool(settings, "capture_cursor", false);
	return obs_source_create_private(CaptureRate::kMonitorCaptureId, name, settings);
}

enum obs_frame_count_kind KindOf(const State &st, const std::string &name)
{
	auto it = st.placed.find(name);
	struct obs_source_frame_counts counts = {};
	if (it != st.placed.end()) {
		obs_source_get_frame_counts(it->second.source, &counts);
	}
	return counts.kind;
}

// Why a capture never started, per source: not showing (nothing renders it), gated,
// or showing with a capture that never opened.
void SayCaptureState(const State &st)
{
	for (const auto &[name, p] : st.placed) {
		struct obs_source_frame_counts counts = {};
		obs_source_get_frame_counts(p.source, &counts);
		Say(name + ": showing=" + std::to_string(obs_source_showing(p.source)) +
		    " gated=" + std::to_string(obs_source_video_gated(p.source)) +
		    " kind=" + std::to_string(counts.kind) + " liveTicks=" + std::to_string(counts.live_ticks));
	}
}

// Keep the lease alive and record each new sampler tick's rows.
void Poll(State &st)
{
	json result;
	std::string err;
	Bridge::Dispatch("stats.watchCaptures", json(nullptr), result, err);
	if (!Bridge::Dispatch("stats.get", json(nullptr), result, err) || !result.is_object()) {
		return;
	}
	const int64_t at = result.value("sampledAtMs", int64_t(-1));
	if (at == st.lastSampledAtMs || !result.contains("captures")) {
		return;
	}
	st.lastSampledAtMs = at;
	if (st.phase == Phase::HalfRate) {
		st.lastHalfRateCaptures = result["captures"];
	}
	Sample sample;
	for (const json &row : result["captures"]) {
		sample[row.value("name", "")] = row;
	}
	st.samples[st.phase].push_back(std::move(sample));
}

std::vector<json> RowsOf(const State &st, Phase phase, const std::string &name, size_t skip = 0)
{
	std::vector<json> out;
	auto it = st.samples.find(phase);
	if (it == st.samples.end()) {
		return out;
	}
	for (size_t i = skip; i < it->second.size(); i++) {
		auto row = it->second[i].find(name);
		if (row != it->second[i].end()) {
			out.push_back(row->second);
		}
	}
	return out;
}

std::vector<double> Numbers(const std::vector<json> &rows, const char *field)
{
	std::vector<double> out;
	for (const json &r : rows) {
		if (r.contains(field) && r[field].is_number()) {
			out.push_back(r[field].get<double>());
		}
	}
	return out;
}

double Median(std::vector<double> v)
{
	if (v.empty()) {
		return -1.0;
	}
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

double Max(const std::vector<double> &v)
{
	return v.empty() ? -1.0 : *std::max_element(v.begin(), v.end());
}

std::string Fmt(double v)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%.1f", v);
	return buf;
}

bool AnyFlag(const std::vector<json> &rows, const char *field)
{
	return std::any_of(rows.begin(), rows.end(), [field](const json &r) { return r.value(field, false); });
}

// ---------------------------------------------------------------------------

// A weak ref outlives its source, and the allocator may hand the freed address to
// the next source created, as a reload can. The pointer value such a source would
// carry is only compared here, never dereferenced.
void CheckIdentityAfterFree(State &st)
{
	obs_source_t *probe = obs_source_create_private(kAsyncId, "caprate-identity", nullptr);
	if (!probe) {
		Check(st, false, "identity probe source created");
		return;
	}
	OBSWeakSourceAutoRelease weak = obs_source_get_weak_source(probe);
	Check(st, CaptureRate::HoldsSource(weak, probe), "a weak ref holds its live source");
	obs_source_release(probe);
	Check(st, !CaptureRate::HoldsSource(weak, probe), "a freed source's address is not its identity");
}

bool Setup(State &st)
{
	json result;
	std::string err;
	if (!Bridge::Dispatch("stats.get", json(nullptr), result, err) || !result.is_object() ||
	    !result.contains("captures")) {
		st.exitCode = 1;
		Say("FAIL stats.get has no captures");
		st.failures.push_back("stats.get has no captures");
		return false;
	}

	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi) || !ovi.fps_den) {
		st.exitCode = 3;
		st.failures.push_back("no video info");
		return false;
	}
	st.mainFps = double(ovi.fps_num) / ovi.fps_den;
	if (!PrimaryMonitor(st)) {
		st.exitCode = 3;
		st.failures.push_back("primary monitor not found");
		return false;
	}
	st.scene = MainScene();
	if (!st.scene) {
		st.exitCode = 3;
		st.failures.push_back("no scene on channel 0");
		return false;
	}

	// The reference rate comes only from live canvases, and this run must not
	// broadcast, so the Default canvas is declared live to the sampler alone. The
	// test canvas goes live after the full-rate phase.
	st.defaultUuid = ObsBootstrap::Canvases().Default().uuid;
	st.canvasUuid = ObsBootstrap::MakeSelfTestCanvas("caprate-canvas");
	if (!CanvasSetup(st)) {
		st.exitCode = 3;
		st.failures.push_back("test canvas has no mix or no scene");
		return false;
	}
	SetLiveCanvases(st, false);

	st.displayAwake = os_inhibit_sleep_create("Braidcast capture-rate self-test");
	os_inhibit_sleep_set_active(st.displayAwake, true);

	st.flicker = std::make_unique<Flicker>();
	if (!st.flicker->Start(st.monitorRect)) {
		st.exitCode = 3;
		st.failures.push_back("flicker window could not be created");
		return false;
	}
	st.flicker->SetEvery(1);

	OBSSourceAutoRelease wgc = CreateMonitorCapture(st, kWgcName, kMethodWgc);
	OBSSourceAutoRelease dxgi = CreateMonitorCapture(st, kDxgiName, kMethodDxgi);
	RegisterAsyncTestSource();
	CheckIdentityAfterFree(st);
	OBSSourceAutoRelease async = obs_source_create_private(kAsyncId, kAsyncName, nullptr);
	OBSSourceAutoRelease canvasDxgi = CreateMonitorCapture(st, kCanvasDxgiName, kMethodDxgi);
	OBSSourceAutoRelease game = CreateGameCapture(kGameName);
	if (!wgc || !dxgi || !async || !canvasDxgi || !game) {
		st.exitCode = 3;
		st.failures.push_back("could not create the test sources");
		return false;
	}
	Place(st, kWgcName, wgc);
	Place(st, kDxgiName, dxgi);
	Place(st, kAsyncName, async);
	PlaceOnCanvas(st, kCanvasDxgiName, canvasDxgi);
	PlaceOnCanvas(st, kGameName, game);
	st.producer = std::make_unique<Producer>();
	st.producer->Start(async);

	Say("up: mainFps=" + Fmt(st.mainFps) + " canvasFps=" + Fmt(st.canvasFps) +
	    " refreshHz=" + std::to_string(st.refreshHz) + " monitor=" + st.monitorId);
	return true;
}

// Change the screen about 30 times a second: half a 60 fps canvas.
void HalfCadence(State &st)
{
	st.flicker->SetEvery(std::max(1, int(std::lround(st.refreshHz / (st.mainFps / 2.0)))));
}

void CheckFullRate(State &st)
{
	const double want = 0.9 * std::min(st.mainFps, double(st.refreshHz));
	for (const char *name : {kWgcName, kDxgiName}) {
		const double median = Median(Numbers(RowsOf(st, Phase::FullRate, name, 2), "rate"));
		Check(st, median >= want,
		      std::string("A full rate: ") + name + " median " + Fmt(median) + "/s >= " + Fmt(want));
	}
	const std::vector<json> async = RowsOf(st, Phase::FullRate, kAsyncName, 2);
	Check(st, !async.empty() && !AnyFlag(async, "below"),
	      "async steady 30/s: input " + Fmt(Median(Numbers(async, "inputFps"))) + " rendered " +
		      Fmt(Median(Numbers(async, "renderedFps"))) + ", never below");
}

bool Near(double a, double b)
{
	return std::fabs(a - b) < 0.01;
}

bool AllStatus(const std::vector<json> &rows, const char *status)
{
	return !rows.empty() && std::all_of(rows.begin(), rows.end(),
					    [status](const json &r) { return r.value("status", "") == status; });
}

// Whether the test canvas's root is the only one that reaches `name`: not Main, not
// a showing hold. Its row then exists because the walk visited that canvas.
bool OnlyTestCanvasReaches(const State &st, const std::string &name)
{
	auto it = st.placed.find(name);
	if (it == st.placed.end()) {
		return false;
	}
	const std::string uuid = obs_source_get_uuid(it->second.source);
	bool canvas = false;
	bool other = false;
	for (const VideoGate::Root &root : VideoGate::WalkRoots()) {
		if (root.sources.count(uuid)) {
			const bool mine = root.kind == VideoGate::RootKind::Canvas && root.canvasUuid == st.canvasUuid;
			(mine ? canvas : other) = true;
		}
	}
	return canvas && !other;
}

// The test canvas is not live yet: the walk still reaches its sources, but no rule
// applies to them, whatever Main is doing.
void CheckCanvasOffAir(State &st)
{
	for (const char *name : {kCanvasDxgiName, kGameName}) {
		Check(st, OnlyTestCanvasReaches(st, name),
		      std::string("D canvas walk: only the test canvas's root reaches ") + name);
	}
	const std::vector<json> disp = RowsOf(st, Phase::FullRate, kCanvasDxgiName, 2);
	Check(st, !disp.empty() && disp.back().value("status", "") == "ok",
	      std::string("D canvas walk: ") + kCanvasDxgiName + " measured on the non-Default canvas");
	Check(st,
	      !disp.empty() &&
		      std::all_of(disp.begin(), disp.end(), [](const json &r) { return r["refFps"].is_null(); }),
	      std::string("D canvas off air: ") + kCanvasDxgiName + " has no reference rate");
	Check(st, !disp.empty() && !disp.back().value("inGrace", true),
	      std::string("D canvas off air: ") + kCanvasDxgiName + " past its grace");

	const std::vector<json> game = RowsOf(st, Phase::FullRate, kGameName, 2);
	// Unhooked, it has no size and nothing to count; hooked, it would read unmeasurable.
	Check(st, AllStatus(game, "idle") && game.back().value("kind", "") == "none",
	      std::string("D game capture: unhooked ") + kGameName + " listed as idle");
}

// The test canvas went live at the start of half cadence: the reference comes from
// that canvas's rate, not Main's, and going live restarts the grace period.
void CheckCanvasLive(State &st)
{
	const std::vector<json> disp = RowsOf(st, Phase::HalfRate, kCanvasDxgiName);
	const double ref = Median(Numbers(disp, "refFps"));
	const double want = std::min(st.canvasFps, st.mainFps);
	Check(st, Near(ref, want),
	      std::string("D canvas live: ") + kCanvasDxgiName + " refFps " + Fmt(ref) + " == canvas " + Fmt(want));
	Check(st, AnyFlag(disp, "inGrace"), std::string("D canvas live: ") + kCanvasDxgiName + " grace ran again");
	const double mainRef = Median(Numbers(RowsOf(st, Phase::HalfRate, kDxgiName), "refFps"));
	if (st.canvasFps < st.mainFps) {
		Check(st, Near(mainRef, st.mainFps),
		      std::string("D per-canvas ref: ") + kDxgiName + " on Main keeps refFps " + Fmt(mainRef));
	} else {
		Inconclusive(st, "D per-canvas ref: the test canvas runs at " + Fmt(st.canvasFps) +
					 " fps, not below Main's " + Fmt(st.mainFps));
	}

	const std::vector<json> game = RowsOf(st, Phase::HalfRate, kGameName);
	Check(st, !game.empty() && Near(Median(Numbers(game, "refFps")), want),
	      std::string("D game capture: ") + kGameName + " takes the canvas's reference");
	Check(st, AllStatus(game, "idle") && !AnyFlag(game, "below"),
	      std::string("D game capture: unhooked ") + kGameName + " stays idle, never below");
}

void CheckHalfRate(State &st)
{
	const double spike = kSpikeShare * st.mainFps;
	for (const char *name : {kWgcName, kDxgiName}) {
		const std::vector<json> rows = RowsOf(st, Phase::HalfRate, name, 3);
		const double fraction = Median(Numbers(rows, "fraction"));
		Check(st, fraction >= 0.42 && fraction <= 0.58,
		      std::string("B half cadence: ") + name + " median fraction " + Fmt(fraction));
		std::string locked;
		if (!rows.empty() && rows.back()["lockedFraction"].is_string()) {
			locked = rows.back()["lockedFraction"].get<std::string>();
		}
		Check(st, locked == "1/2", std::string("B locked: ") + name + " lockedFraction '" + locked + "'");
		Check(st, !AnyFlag(RowsOf(st, Phase::HalfRate, name), "below"),
		      std::string("B no warning: ") + name + " never below");
	}
	// The join lands mid-phase; every sample it appears in must be about half.
	for (const char *name : {kDxgiName, kDxgi2Name}) {
		const double max = Max(Numbers(RowsOf(st, Phase::HalfRate, name, 3), "rate"));
		Check(st, max >= 0.0 && max <= spike,
		      std::string("B shared duplicator: ") + name + " max " + Fmt(max) + "/s <= " + Fmt(spike));
	}
	Check(st, AnyFlag(RowsOf(st, Phase::HalfRate, kAsyncName, 2), "below"), "B bursty async input reads below");
}

void CheckStaticDxgi(State &st, Phase phase, const std::string &what)
{
	const double median = Median(Numbers(RowsOf(st, phase, kDxgiName, 2), "rate"));
	Check(st, median >= 0.0 && median <= kStaticMaxRate,
	      "C " + what + ": DXGI median " + Fmt(median) + "/s <= " + Fmt(kStaticMaxRate));
}

void CheckStatic(State &st)
{
	CheckStaticDxgi(st, Phase::Static, "static screen, still cursor");
	const std::vector<json> async = RowsOf(st, Phase::Static, kAsyncName, 2);
	Check(st, !async.empty() && async.back().value("status", "") == "unmeasurable",
	      "C deinterlaced async reads unmeasurable");
}

// The session line's length in seconds, or -1.
double SessionSeconds(const std::string &line)
{
	const std::string marker = "[capture-rate] session ";
	const size_t at = line.find(marker);
	return at == std::string::npos ? -1.0 : std::atof(line.c_str() + at + marker.size());
}

void CheckNoSpike(State &st, Phase phase, const std::string &name, const std::string &what)
{
	const std::vector<json> rows = RowsOf(st, phase, name);
	const double max = Max(Numbers(rows, "rate"));
	const double spike = kSpikeShare * st.mainFps;
	Check(st, max <= spike, what + ": " + name + " max " + Fmt(max) + "/s <= " + Fmt(spike));
	Check(st, AnyFlag(rows, "inGrace"), what + ": " + name + " grace ran again");
}

std::optional<double> RateIn(const Sample &sample, const std::string &name)
{
	auto row = sample.find(name);
	if (row == sample.end() || !row->second.contains("rate") || !row->second["rate"].is_number()) {
		return std::nullopt;
	}
	return row->second["rate"].get<double>();
}

// Both DXGI sources share one duplicator, so screen activity raises them together,
// while a reloaded source that missed its baseline spikes alone.
void CheckReload(State &st)
{
	const std::string what = "save/remove/load";
	const double spike = kSpikeShare * st.mainFps;
	double soloMax = -1.0;
	double sharedMax = -1.0;
	for (const Sample &sample : st.samples[Phase::Reload]) {
		const std::optional<double> rate = RateIn(sample, kDxgiName);
		if (!rate || *rate <= spike) {
			continue;
		}
		const std::optional<double> other = RateIn(sample, kDxgi2Name);
		double &slot = other && *other > spike ? sharedMax : soloMax;
		slot = std::max(slot, *rate);
	}
	if (soloMax >= 0.0) {
		Check(st, false,
		      what + ": " + kDxgiName + " max " + Fmt(soloMax) + "/s <= " + Fmt(spike) + " with " + kDxgi2Name +
			      " steady (missed baseline)");
	} else if (sharedMax >= 0.0) {
		Inconclusive(st, what + ": " + kDxgiName + " and " + kDxgi2Name + " both above " + Fmt(spike) +
					 "/s in one sample (" + kDxgiName + " " + Fmt(sharedMax) +
					 "/s): screen activity, not a missed baseline");
	} else {
		const double max = Max(Numbers(RowsOf(st, Phase::Reload, kDxgiName), "rate"));
		Check(st, true, what + ": " + kDxgiName + " max " + Fmt(max) + "/s <= " + Fmt(spike));
	}
	Check(st, AnyFlag(RowsOf(st, Phase::Reload, kDxgiName), "inGrace"),
	      what + ": " + kDxgiName + " grace ran again");
}

void Teardown(State &st)
{
	if (st.cursor) {
		st.cursor->Stop();
	}
	if (st.producer) {
		st.producer->Stop();
	}
	std::vector<std::string> names;
	for (const auto &[name, p] : st.placed) {
		names.push_back(name);
	}
	for (const std::string &name : names) {
		Unplace(st, name);
	}
	if (st.reloaded && !st.reloadUuid.empty()) {
		OBSSourceAutoRelease loaded = obs_get_source_by_uuid(st.reloadUuid.c_str());
		if (loaded) {
			obs_source_remove(loaded);
		}
	}
	if (st.flicker) {
		st.flicker->Stop();
	}
	if (st.sessionOpen) {
		st.sessionLine = ObsBootstrap::CaptureRates().SessionEnd();
		st.sessionOpen = false;
	}
	ObsBootstrap::CaptureRates().SetCanvasLiveOverrideForTest(nullptr);
	st.scene = nullptr;
	st.canvasScene = nullptr;
	if (!st.canvasUuid.empty()) {
		ObsBootstrap::RemoveSelfTestCanvas(st.canvasUuid);
		st.canvasUuid.clear();
	}
	if (st.displayAwake) {
		os_inhibit_sleep_destroy(st.displayAwake);
		st.displayAwake = nullptr;
	}
}

void WriteSummary(State &st)
{
	if (st.exitCode < 0) {
		st.exitCode = !st.failures.empty() ? 1 : !st.inconclusive.empty() ? 2 : 0;
	}
	const json summary{
		{"result", SelfTest::ResultName(st.exitCode)},
		{"exitCode", st.exitCode},
		{"passes", st.passes},
		{"failures", st.failures},
		{"inconclusive", st.inconclusive},
		{"mainFps", st.mainFps},
		{"refreshHz", st.refreshHz},
		{"halfRateCaptures", st.lastHalfRateCaptures},
	};
	const std::string path = SelfTest::WriteSummaryFile("capture-rate", summary.dump(2));
	Say(std::string(SelfTest::ResultName(st.exitCode)) + " passes=" + std::to_string(st.passes.size()) +
	    " failures=" + std::to_string(st.failures.size()) + " inconclusive=" +
	    std::to_string(st.inconclusive.size()) + " summary=" + (path.empty() ? "(unwritten)" : path));
}

} // namespace

void ObsBootstrap::ArmCaptureRateSelfTest(HWND)
{
	g_state = State{};
	Enter(g_state, Phase::Settle);
	Say("armed");
}

bool ObsBootstrap::RunCaptureRateSelfTest()
{
	State &st = g_state;
	if (st.phase != Phase::Idle && st.phase != Phase::Settle && st.phase != Phase::Done) {
		Poll(st);
	}

	switch (st.phase) {
	case Phase::Idle:
	case Phase::Done:
		return true;

	case Phase::Settle:
		if (InPhase(st) >= SelfTest::kBootSettle) {
			Enter(st, Phase::Setup);
		}
		return false;

	case Phase::Setup:
		if (!Setup(st)) {
			Enter(st, Phase::Finish);
			return false;
		}
		Enter(st, Phase::WaitKinds);
		return false;

	case Phase::WaitKinds: {
		const bool wgc = KindOf(st, kWgcName) == OBS_FRAME_COUNT_WGC;
		const bool dxgi = KindOf(st, kDxgiName) == OBS_FRAME_COUNT_DXGI;
		// Counting at all means showing, which only the test canvas's render gives it.
		const bool canvasDxgi = KindOf(st, kCanvasDxgiName) == OBS_FRAME_COUNT_DXGI;
		if (wgc && dxgi && canvasDxgi) {
			ObsBootstrap::CaptureRates().SessionBegin();
			st.sessionOpen = true;
			st.sessionBegan = Clock::now();
			st.sessionLinesBefore = SelfTest::CountSessionLogLines("[capture-rate] session ");
			Enter(st, Phase::FullRate);
		} else if (InPhase(st) >= kWaitKinds) {
			SayCaptureState(st);
			if (wgc && dxgi) {
				// The same duplicator works on Main, so this is the canvas, not the machine.
				Check(st, false,
				      std::string("D canvas walk: ") + kCanvasDxgiName +
					      " never showed on the non-Default canvas");
			} else {
				st.exitCode = 2;
				st.failures.push_back(std::string("capture never started: ") +
						      (dxgi ? "" : "no DXGI duplicator ") +
						      (wgc ? "" : "no WGC capture"));
				Say("SKIP " + st.failures.back());
			}
			Enter(st, Phase::Finish);
		}
		return false;
	}

	case Phase::FullRate:
		if (InPhase(st) >= kFullRate) {
			CheckFullRate(st);
			CheckCanvasOffAir(st);
			SetLiveCanvases(st, true);
			// A second live edge while the session is open, as coalesced
			// transitions deliver: the session must carry on untouched.
			ObsBootstrap::CaptureRates().SessionBegin();
			HalfCadence(st);
			st.producer->SetMode(FeedMode::Bursty);
			Enter(st, Phase::HalfRate);
		}
		return false;

	case Phase::HalfRate:
		if (!st.joined && InPhase(st) >= kJoinAfter) {
			OBSSourceAutoRelease dxgi2 = CreateMonitorCapture(st, kDxgi2Name, kMethodDxgi);
			if (dxgi2) {
				Place(st, kDxgi2Name, dxgi2);
			}
			st.joined = true;
		}
		if (InPhase(st) >= kHalfRate) {
			CheckHalfRate(st);
			CheckCanvasLive(st);
			st.flicker->SetEvery(0);
			st.producer->SetMode(FeedMode::Steady);
			obs_source_set_deinterlace_mode(st.placed[kAsyncName].source, OBS_DEINTERLACE_MODE_BLEND);
			Enter(st, Phase::Static);
		}
		return false;

	case Phase::Static:
		if (InPhase(st) >= kStatic) {
			CheckStatic(st);
			// While a WGC session is capturing, pointer motion reaches DXGI as real
			// desktop presents (about 60/s here, against 0/s with WGC torn down).
			// Hiding the WGC source frees its session, so the moving-cursor check
			// sees DXGI's own filtering.
			Hide(st.placed[kWgcName]);
			const RECT &rc = st.monitorRect;
			st.cursor = std::make_unique<CursorMover>();
			st.cursor->Start(POINT{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2});
			Enter(st, Phase::Hide);
		}
		return false;

	case Phase::Hide:
		if (InPhase(st) >= kHide) {
			CheckStaticDxgi(st, Phase::Hide, "static screen, moving cursor");
			const std::vector<json> rows = RowsOf(st, Phase::Hide, kWgcName, 1);
			Check(st, rows.empty() || rows.back().value("status", "") != "ok",
			      "hidden WGC source stops reading a rate");
			st.cursor->Stop();
			obs_source_set_deinterlace_mode(st.placed[kAsyncName].source, OBS_DEINTERLACE_MODE_DISABLE);
			HalfCadence(st);
			Show(st.placed[kWgcName]);
			Enter(st, Phase::Reshow);
		}
		return false;

	case Phase::Reshow:
		if (InPhase(st) >= kReshow) {
			CheckNoSpike(st, Phase::Reshow, kWgcName, "re-show");
			OBSDataAutoRelease settings = obs_data_create();
			obs_data_set_int(settings, "method", kMethodDxgi);
			obs_source_update(st.placed[kWgcName].source, settings);
			Enter(st, Phase::Switch);
		}
		return false;

	case Phase::Switch:
		if (InPhase(st) >= kSwitch) {
			CheckNoSpike(st, Phase::Switch, kWgcName, "WGC->DXGI switch");
			const std::vector<json> rows = RowsOf(st, Phase::Switch, kWgcName);
			Check(st, !rows.empty() && rows.back().value("kind", "") == "dxgi",
			      "method switch reads as DXGI");

			// Save, remove and load: the loaded source takes the saved uuid.
			Placed &old = st.placed[kDxgiName];
			OBSDataAutoRelease saved = obs_save_source(old.source);
			const std::string uuid = obs_source_get_uuid(old.source);
			Unplace(st, kDxgiName);
			OBSSourceAutoRelease loaded = obs_load_source(saved);
			st.reloaded = true;
			st.reloadUuid = uuid;
			Check(st, loaded && uuid == obs_source_get_uuid(loaded), "reload keeps the uuid " + uuid);
			if (loaded) {
				Place(st, kDxgiName, loaded);
			}
			Enter(st, Phase::Reload);
		}
		return false;

	case Phase::Reload:
		if (InPhase(st) >= kReload) {
			CheckReload(st);
			Enter(st, Phase::Finish);
		}
		return false;

	case Phase::Finish: {
		const bool hadSession = st.sessionOpen;
		const double sessionWanted =
			std::chrono::duration<double>(Clock::now() - st.sessionBegan).count() - 2.0;
		Teardown(st);
		if (hadSession) {
			const double seconds = SessionSeconds(st.sessionLine);
			Check(st, seconds >= sessionWanted,
			      "a repeated begin keeps the session: " + Fmt(seconds) + " s >= " + Fmt(sessionWanted));
			const int lines = SelfTest::CountSessionLogLines("[capture-rate] session ");
			Check(st, lines == st.sessionLinesBefore + 1, "session log line written");
			Check(st, SelfTest::CountSessionLogLines("'caprate-async' async in") >= 1,
			      "session line reports the async source");
			Check(st, SelfTest::CountSessionLogLines("'caprate-dxgi2' DXGI median") >= 1,
			      "session line reports a DXGI source");
			Check(st, SelfTest::CountSessionLogLines("'caprate-canvas-dxgi' DXGI median") >= 1,
			      "session line reports the non-Default canvas's source");
			Check(st, SelfTest::CountSessionLogLines("'caprate-game' idle (never counted)") >= 1,
			      "session line names the showing, unhooked game capture");
		}
		WriteSummary(st);
		Enter(st, Phase::Done);
		return true;
	}
	}
	return true;
}

int ObsBootstrap::CaptureRateSelfTestExitCode()
{
	return g_state.exitCode < 0 ? 0 : g_state.exitCode;
}
