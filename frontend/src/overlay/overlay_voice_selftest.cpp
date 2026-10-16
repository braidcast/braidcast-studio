#include "../obs_bootstrap.hpp"

#include "overlay_sources.hpp"
#include "overlay_voice_selftest.hpp"

#include "../audio/AudioMonitor.hpp"
#include "../bridge.hpp"
#include "../log.hpp"
#include "../scene/scene_collections.hpp"
#include "../scene/scene_persistence.hpp"
#include "../util/selftest_paths.hpp"

#include <obs.h>
#include <obs.hpp>

#include <windows.h>

#include <cmath>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"

// The overlay voice smoke self-test (OverlayVoice, libobs's audio groups). Spec items (b)-(f):
// (a) is the cmocka table in test/cmocka/test_audio_voice.c, (g) the overlay-voice stream mode.
//
// Adoption and fan-out are queued onto the UI thread, which the smoke battery holds for its
// whole synchronous run, so this test runs as a chain of UI tasks after it: each step acts
// once and is then polled every kPollMs until its condition holds or its budget runs out.
// Every task queued by an act is ahead of the step's first poll, so a condition that needs
// the queued pass sees it.
namespace {

using Bridge::json;

constexpr const char *kTag = "[selftest] overlay-voice";
constexpr int kPollMs = 20;
constexpr int kHideBudgetMs = 3000;
// Subjects name no widget, so their page stays about:blank; each group gets its own id.
constexpr const char *kLiveId = "selftest-overlay-voice-live";
constexpr const char *kLegacyIds[] = {"selftest-overlay-voice-legacy-original-first",
				      "selftest-overlay-voice-legacy-copy-first"};

struct Step {
	const char *name;
	std::function<void()> act;
	std::function<bool()> holds; // polled until true or the budget runs out
	int budgetMs;
};

struct Ctx {
	OBSSource a;
	OBSSource b;
	obs_scene_t *sceneA = nullptr;
	obs_scene_t *sceneB = nullptr;
	obs_sceneitem_t *itemA = nullptr;
	obs_canvas_t *canvas = nullptr;
	OBSSource priorChannel;
	std::string savedA;
	std::string savedB;
	std::vector<OBSSource> legacy; // (c)'s loaded pairs
	bool savedCollection = false;

	std::vector<Step> steps;
	size_t index = 0;
	bool acted = false;
	ULONGLONG startedAt = 0;
	ULONGLONG holdingSince = 0; // Hold(): when the condition was first seen, 0 = not yet
	bool broken = false;        // Hold(): the condition broke after it was first seen
	std::string detail;         // the failing step's own account
	int failures = 0;
};

std::unique_ptr<Ctx> g_ctx;

bool VoiceIs(const char *expected)
{
	if (!g_ctx->a || !g_ctx->b) {
		g_ctx->detail = "braidcast_overlay create failed (obs-browser not loaded?)";
		return false;
	}
	g_ctx->detail = std::string("voice=") + OverlayVoiceSelfTest::HeardOf(g_ctx->a, g_ctx->b);
	return g_ctx->detail == std::string("voice=") + expected;
}

// True once `condition` has held for `ms` without a break since it was first seen.
bool Hold(const std::function<bool()> &condition, ULONGLONG ms)
{
	Ctx &c = *g_ctx;
	if (c.broken) {
		return false;
	}
	const ULONGLONG now = GetTickCount64();
	if (!condition()) {
		c.broken = c.holdingSince != 0;
		if (c.broken) {
			c.detail += " (broke after holding)";
		}
		return false;
	}
	if (c.holdingSince == 0) {
		c.holdingSince = now;
	}
	return now - c.holdingSince >= ms;
}

std::string Saved(obs_source_t *source)
{
	OBSDataAutoRelease data = obs_save_source(source);
	const char *text = data ? obs_data_get_json(data) : nullptr;
	return text ? text : "";
}

json Call(const std::string &method, const json &params)
{
	json result;
	std::string error;
	if (!Bridge::Dispatch(method, params, result, error)) {
		g_ctx->detail = method + " failed: " + error;
		return json();
	}
	return result;
}

std::string Uuid(obs_source_t *source)
{
	return obs_source_get_uuid(source);
}

bool BothMuted(bool muted)
{
	const bool a = obs_source_muted(g_ctx->a);
	const bool b = obs_source_muted(g_ctx->b);
	g_ctx->detail = std::string("A muted=") + (a ? "true" : "false") + " B muted=" + (b ? "true" : "false");
	return a == muted && b == muted;
}

obs_source_t *CreateCopy(const char *overlayId, const char *name)
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, Overlay::kOverlayIdKey, overlayId);
	obs_data_set_bool(settings, Overlay::kRerouteAudioKey, true);
	return obs_source_create(Overlay::kOverlaySourceId, name, settings, nullptr);
}

void SetUpLive()
{
	Ctx &c = *g_ctx;
	c.a = OBSSource(CreateCopy(kLiveId, "selftest-overlay-voice-A"));
	c.b = OBSSource(CreateCopy(kLiveId, "selftest-overlay-voice-B"));
	obs_source_release(c.a); // OBSSource took its own reference
	obs_source_release(c.b);
	if (!c.a || !c.b) {
		return;
	}

	// A on the main mix, as on the Main canvas's program.
	c.sceneA = obs_scene_create_private("selftest-overlay-voice-scene-A");
	c.itemA = obs_scene_add(c.sceneA, c.a);
	c.priorChannel = OBSSource(obs_get_output_source(ObsBootstrap::kSelfTestOutputChannel));
	obs_source_release(c.priorChannel);
	obs_set_output_source(ObsBootstrap::kSelfTestOutputChannel, obs_scene_get_source(c.sceneA));

	// B on a canvas of its own whose mix feeds the audio.
	struct obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	c.canvas = obs_canvas_create_private("selftest-overlay-voice-canvas", &ovi, PROGRAM);
	c.sceneB = obs_scene_create_private("selftest-overlay-voice-scene-B");
	obs_scene_add(c.sceneB, c.b);
	if (c.canvas) {
		obs_canvas_set_channel(c.canvas, 0, obs_scene_get_source(c.sceneB));
	}
}

void PutAOnAir(bool onAir)
{
	obs_set_output_source(ObsBootstrap::kSelfTestOutputChannel,
			      onAir ? obs_scene_get_source(g_ctx->sceneA) : g_ctx->priorChannel.Get());
}

void PutBOnAir(bool onAir)
{
	if (g_ctx->canvas) {
		obs_canvas_set_channel(g_ctx->canvas, 0, onAir ? obs_scene_get_source(g_ctx->sceneB) : nullptr);
	}
}

// A saved record as a collection file holds it.
OBSDataAutoRelease SavedRecord(const char *overlayId, const char *name, bool muted)
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, Overlay::kOverlayIdKey, overlayId);
	obs_data_set_bool(settings, Overlay::kRerouteAudioKey, true);
	obs_data_t *record = obs_data_create();
	obs_data_set_string(record, "id", Overlay::kOverlaySourceId);
	obs_data_set_string(record, "name", name);
	obs_data_set_obj(record, "settings", settings);
	obs_data_set_bool(record, "muted", muted);
	return OBSDataAutoRelease(record);
}

// The legacy copy a scene duplicate used to leave muted on another canvas, loaded beside
// its original in the given order.
void LoadLegacyPair(size_t which, bool copyFirst)
{
	const char *id = kLegacyIds[which];
	const std::string original = std::string(id) + "-original";
	const std::string copy = std::string(id) + "-copy";
	OBSDataAutoRelease originalRecord = SavedRecord(id, original.c_str(), false);
	OBSDataAutoRelease copyRecord = SavedRecord(id, copy.c_str(), true);
	obs_data_t *order[] = {copyFirst ? copyRecord.Get() : originalRecord.Get(),
			       copyFirst ? originalRecord.Get() : copyRecord.Get()};
	for (obs_data_t *record : order) {
		obs_source_t *source = obs_load_source(record);
		g_ctx->legacy.emplace_back(source);
		obs_source_release(source);
	}
}

// Both sources of pair `which` unmuted, live and in the saved collection.
bool LegacyPairUnmuted(size_t which)
{
	Ctx &c = *g_ctx;
	const OBSSource &first = c.legacy[which * 2];
	const OBSSource &second = c.legacy[which * 2 + 1];
	if (!first || !second) {
		c.detail = "obs_load_source failed";
		return false;
	}
	if (obs_source_muted(first) || obs_source_muted(second)) {
		c.detail = "a copy is still muted live";
		return false;
	}
	std::ifstream in(ObsBootstrap::SceneCollections().ActiveScenePath());
	const json file =
		json::parse(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>(), nullptr, false);
	if (!file.is_object() || !file.contains("sources") || !file["sources"].is_array()) {
		c.detail = "the collection file has no sources array";
		return false;
	}
	int savedUnmuted = 0;
	for (const json &source : file["sources"]) {
		const std::string name = source.value("name", "");
		if ((name == obs_source_get_name(first) || name == obs_source_get_name(second)) &&
		    !source.value("muted", true)) {
			savedUnmuted++;
		}
	}
	c.savedCollection = true;
	c.detail = "saved unmuted=" + std::to_string(savedUnmuted) + "/2";
	return savedUnmuted == 2;
}

void TearDown()
{
	Ctx &c = *g_ctx;
	if (c.sceneA) {
		obs_set_output_source(ObsBootstrap::kSelfTestOutputChannel, c.priorChannel);
		obs_scene_release(c.sceneA);
		c.sceneA = nullptr;
	}
	if (c.canvas) {
		obs_canvas_set_channel(c.canvas, 0, nullptr);
	}
	if (c.sceneB) {
		obs_scene_release(c.sceneB);
		c.sceneB = nullptr;
	}
	if (c.canvas) {
		obs_canvas_remove(c.canvas);
		obs_canvas_release(c.canvas);
		c.canvas = nullptr;
	}
	for (OBSSource *source : {&c.a, &c.b}) {
		if (*source) {
			obs_source_remove(*source);
		}
		*source = nullptr;
	}
	for (OBSSource &source : c.legacy) {
		if (source) {
			obs_source_remove(source);
		}
	}
	c.legacy.clear();
	c.priorChannel = nullptr;
	// Adoption saved the subjects into the collection; this saves them back out.
	SceneCollection::SaveUnlessBrowserSourcesDisabled();
}

std::vector<Step> BuildSteps()
{
	const auto noop = [] {
	};
	return {
		{"(b) two copies on air, on two mixes: exactly one open, A by rank", SetUpLive,
		 [] { return VoiceIs("A"); }, 500},
		{"(b) A off air: B opens",
		 [] {
			 g_ctx->savedA = Saved(g_ctx->a);
			 g_ctx->savedB = Saved(g_ctx->b);
			 PutAOnAir(false);
		 },
		 [] { return VoiceIs("B"); }, 500},
		{"(e) the gate is never saved", noop,
		 [] {
			 const bool same = Saved(g_ctx->a) == g_ctx->savedA && Saved(g_ctx->b) == g_ctx->savedB;
			 g_ctx->detail = same ? "obs_save_source identical across the handoff"
					      : "obs_save_source changed across the handoff (MISMATCH)";
			 return same;
		 },
		 0},
		{"(b) A back on air: B keeps the voice", [] { PutAOnAir(true); },
		 [] { return Hold([] { return VoiceIs("B"); }, 300); }, 500},
		{"(b) B off air: A opens", [] { PutBOnAir(false); }, [] { return VoiceIs("A"); }, 500},
		{"(b) B back on air: A keeps the voice", [] { PutBOnAir(true); },
		 [] { return Hold([] { return VoiceIs("A"); }, 300); }, 500},
		// An audio source's item hides on the audio timeline (scene audio actions), so the
		// handoff waits out the mix's buffering as well as the pick.
		{"(b) A hidden while B shows: the voice hands off to B",
		 [] { obs_sceneitem_set_visible(g_ctx->itemA, false); }, [] { return VoiceIs("B"); }, kHideBudgetMs},
		{"(b) A shown again: B keeps the voice", [] { obs_sceneitem_set_visible(g_ctx->itemA, true); },
		 [] { return Hold([] { return VoiceIs("B"); }, 100); }, 300},
		{"(f) audio.list folds the copies into one row", [] { ObsBootstrap::AudioMonitor().Rebuild(); },
		 [] {
			 const json list = Call("audio.list", json::object());
			 int rows = 0;
			 size_t members = 0;
			 if (list.is_object() && list["sources"].is_array()) {
				 for (const json &row : list["sources"]) {
					 const bool ours = row.value("overlayId", "") == kLiveId ||
							   row.value("uuid", "") == Uuid(g_ctx->a) ||
							   row.value("uuid", "") == Uuid(g_ctx->b);
					 if (ours) {
						 rows++;
						 members = row.contains("members") ? row["members"].size() : 0;
					 }
				 }
			 }
			 g_ctx->detail = "rows=" + std::to_string(rows) + " members=" + std::to_string(members);
			 return rows == 1 && members == 2;
		 },
		 0},
		{"(d) audio.setMuted on the representative mutes every copy",
		 [] { Call("audio.setMuted", json{{"uuid", Uuid(g_ctx->a)}, {"muted", true}}); },
		 [] { return BothMuted(true); }, 0},
		{"(d) audio.setMuted off again unmutes every copy",
		 [] { Call("audio.setMuted", json{{"uuid", Uuid(g_ctx->a)}, {"muted", false}}); },
		 [] { return BothMuted(false); }, 0},
		{"(d) the voice muted directly, as a hotkey does, with a UI change on the other copy",
		 [] {
			 obs_source_set_muted(g_ctx->b, true);
			 Call("audio.setAdvanced", json{{"uuid", Uuid(g_ctx->a)}, {"volumeDb", -6.0}});
		 },
		 [] {
			 return Hold(
				 [] {
					 const float want = obs_db_to_mul(-6.0f);
					 const float a = obs_source_get_volume(g_ctx->a);
					 const float b = obs_source_get_volume(g_ctx->b);
					 const bool muted = BothMuted(true);
					 g_ctx->detail += " volume A=" + std::to_string(a) + " B=" + std::to_string(b);
					 return muted && std::fabs(a - want) < 1e-4f && std::fabs(b - want) < 1e-4f;
				 },
				 200);
		 },
		 600},
		{"(d) restore",
		 [] {
			 Call("audio.setMuted", json{{"uuid", Uuid(g_ctx->a)}, {"muted", false}});
			 Call("audio.setAdvanced", json{{"uuid", Uuid(g_ctx->a)}, {"volumeDb", 0.0}});
		 },
		 [] { return BothMuted(false) && obs_source_get_volume(g_ctx->b) == 1.0f; }, 0},
		{"(c) a muted legacy copy loaded after its original: both unmuted and saved",
		 [] { LoadLegacyPair(0, false); }, [] { return LegacyPairUnmuted(0); }, 1000},
		{"(c) a muted legacy copy loaded before its original: both unmuted and saved",
		 [] { LoadLegacyPair(1, true); }, [] { return LegacyPairUnmuted(1); }, 1000},
		// The mixer page reloads on every audio.changed above, listing the subjects and then
		// reading each one; removing them mid-reload would fail its reads. Let it finish.
		{"let the mixer page settle before teardown", noop, [] { return Hold([] { return true; }, 1000); },
		 1500},
	};
}

void Tick()
{
	if (!g_ctx) {
		return;
	}
	Ctx &c = *g_ctx;
	if (c.index == c.steps.size()) {
		TearDown();
		HostLog(std::string(kTag) + ": " + std::to_string(c.steps.size() - size_t(c.failures)) + "/" +
			std::to_string(c.steps.size()) + " steps passed" + (c.failures ? ": FAIL" : " (PASS)"));
		g_ctx.reset();
		return;
	}
	Step &step = c.steps[c.index];
	if (!c.acted) {
		c.acted = true;
		c.startedAt = GetTickCount64();
		c.holdingSince = 0;
		c.broken = false;
		c.detail.clear();
		step.act();
		// The first poll waits behind whatever the act queued.
		CefPostTask(TID_UI, base::BindOnce(&Tick));
		return;
	}
	const bool passed = step.holds();
	const bool expired = GetTickCount64() - c.startedAt >= ULONGLONG(step.budgetMs);
	if (!passed && !expired) {
		CefPostDelayedTask(TID_UI, base::BindOnce(&Tick), kPollMs);
		return;
	}
	HostLog(std::string(kTag) + " " + step.name + ": " + (passed ? "PASS" : "FAIL") + " after " +
		std::to_string(GetTickCount64() - c.startedAt) + " ms" +
		(c.detail.empty() ? "" : " (" + c.detail + ")"));
	c.failures += passed ? 0 : 1;
	c.index++;
	c.acted = false;
	if (!passed && c.index == 1) {
		c.index = c.steps.size(); // nothing to test without the two copies
	}
	CefPostTask(TID_UI, base::BindOnce(&Tick));
}

} // namespace

void ObsBootstrap::RunOverlayVoiceSelfTest()
{
	if (g_ctx) {
		return;
	}
	g_ctx = std::make_unique<Ctx>();
	g_ctx->steps = BuildSteps();
	// Started once the UI thread is free: the rest of the smoke battery runs synchronously
	// after this call, and adoption and fan-out wait on the UI thread.
	CefPostTask(TID_UI, base::BindOnce(&Tick));
}

void ObsBootstrap::CancelOverlayVoiceSelfTest()
{
	if (!g_ctx) {
		return;
	}
	TearDown();
	// Reports through the log alone: this battery's verdict is its closing line, not an exit code.
	int exitCode = SelfTest::kUndecided;
	HostLog(std::string(kTag) + ": " +
		SelfTest::SettleCancelled(exitCode, nullptr, g_ctx->index, g_ctx->steps.size(), "steps",
					  g_ctx->failures));
	g_ctx.reset();
}
