#include "diag/capture_rate.hpp"
#include "hook-frame-relay.h"
#include "frame-gen-pacer.h"
#include "frame-gen-stats.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

// cmocka requires these in this order before cmocka.h, and on MSVC cmocka.h
// macroizes `inline`, which the C++ standard library rejects -- so every other
// header this file needs has to come above it. Wrap cmocka.h in extern "C" so its
// symbols get C linkage in this C++ TU (its own guard does not apply here).
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
extern "C" {
#include <cmocka.h>
}

using namespace CaptureRate;

namespace {

constexpr double kMainFps = 60.0;

// One capture source driven one second at a time. Each Step() advances the
// counters the way the libobs fold would for that second and feeds one sample.
struct Feed {
	SourceInput src;

	Feed(const char *uuid, Kind kind, double canvasFps = kMainFps, bool live = true)
	{
		src.uuid = uuid;
		src.name = uuid;
		src.identity = 1;
		src.showing = true;
		src.counts.kind = kind;
		src.reach.push_back(Reach{canvasFps, live});
	}

	// ticks: live ticks this second; newTicks: ticks that brought a frame;
	// delivered: producer frames this second.
	void Advance(uint32_t ticks, uint32_t newTicks, uint32_t delivered)
	{
		src.counts.liveTicks += ticks;
		src.counts.newFrameTicks += newTicks;
		src.counts.framesDelivered += delivered;
	}

	// A hooked game's second: the plugin relays presents as offered frames and
	// copies as delivered ones, and newTicks are the ticks that brought a frame
	// the source had not shown (hook_frame_relay_step decides which).
	void AdvanceGame(uint32_t ticks, uint32_t presents, uint32_t copies, uint32_t newTicks)
	{
		Advance(ticks, newTicks, copies);
		src.counts.framesOffered += presents;
	}
};

// One sample carrying every feed as it stands.
void SampleAll(Tracker &t, std::initializer_list<const Feed *> feeds, double dt = 1.0)
{
	SampleInput in;
	in.dtSec = dt;
	in.mainFps = kMainFps;
	for (const Feed *f : feeds) {
		in.sources.push_back(f->src);
	}
	t.Sample(in);
}

void Step(Tracker &t, Feed &f, uint32_t ticks, uint32_t newTicks, uint32_t delivered, double dt = 1.0)
{
	f.Advance(ticks, newTicks, delivered);
	SampleAll(t, {&f}, dt);
}

const Row *RowFor(const Tracker &t, const std::string &uuid)
{
	for (const Row &r : t.Rows()) {
		if (r.uuid == uuid) {
			return &r;
		}
	}
	return nullptr;
}

// Baseline sample: the first sight of a source reports no delta.
void Prime(Tracker &t, Feed &f)
{
	Step(t, f, 0, 0, 0);
}

} // namespace

// C1: the render loop lost 20% of its slots to lag, so only 48 ticks ran. An async
// source that rendered a frame on every one of them is keeping up, not "below".
static void test_lag_is_not_async_below(void **)
{
	Tracker t;
	Feed cam("cam", Kind::Async);
	Prime(t, cam);
	for (int i = 0; i < 5; i++) {
		Step(t, cam, 48, 48, 60);
		const Row *r = RowFor(t, "cam");
		assert_non_null(r);
		assert_false(r->below);
		assert_true(r->renderedFps.has_value());
		assert_true(std::fabs(*r->renderedFps - 48.0) < 0.01);
		assert_true(std::fabs(*r->inputFps - 60.0) < 0.01);
	}
}

// C2: display-rate sources only ever report a rate. A static desktop or page
// delivering nothing, or a 30 fps one, is never "below".
static void test_display_capture_is_never_below(void **)
{
	for (Kind kind : {Kind::Wgc, Kind::Dxgi, Kind::BrowserPaint}) {
		for (uint32_t rate : {0u, 5u, 15u, 30u}) {
			Tracker t;
			Feed f("disp", kind);
			Prime(t, f);
			for (int i = 0; i < 20; i++) {
				Step(t, f, 60, rate, rate);
				const Row *r = RowFor(t, "disp");
				assert_non_null(r);
				assert_false(r->below);
				assert_true(std::fabs(*r->rate - rate) < 0.01);
			}
		}
	}
}

// WGC can land two frames in one pump; the displayed rate is capped at what the
// canvas could show, and the fraction never passes 1.
static void test_display_rate_is_capped_at_expected(void **)
{
	Tracker t;
	Feed f("disp", Kind::Wgc);
	Prime(t, f);
	Step(t, f, 60, 60, 75);
	const Row *r = RowFor(t, "disp");
	assert_true(std::fabs(*r->rate - 60.0) < 0.01);
	assert_true(std::fabs(*r->fraction - 1.0) < 0.001);
}

// A steady half cadence locks once grace (5 s) and ten eligible seconds have passed,
// and not a second earlier. It never warns.
static void test_half_cadence_locks_after_grace(void **)
{
	Tracker t;
	Feed f("disp", Kind::Dxgi);
	Prime(t, f);
	for (int s = 1; s <= 14; s++) {
		Step(t, f, 60, 30, 30);
		assert_null(RowFor(t, "disp")->lockedFraction);
	}
	Step(t, f, 60, 30, 30);
	const Row *r = RowFor(t, "disp");
	assert_non_null(r->lockedFraction);
	assert_string_equal(r->lockedFraction, "1/2");
	assert_false(r->below);
}

static void test_zero_rate_never_locks(void **)
{
	Tracker t;
	Feed f("disp", Kind::Wgc);
	Prime(t, f);
	for (int s = 0; s < 40; s++) {
		Step(t, f, 60, 0, 0);
		assert_null(RowFor(t, "disp")->lockedFraction);
	}
}

// A second the source spent mostly hidden (alt-tab) is skipped: it neither counts
// toward the lock nor breaks the streak.
static void test_mostly_hidden_second_is_skipped(void **)
{
	Tracker t;
	Feed f("disp", Kind::Wgc);
	Prime(t, f);
	for (int s = 1; s <= 12; s++) {
		Step(t, f, 60, 30, 30);
	}
	Step(t, f, 10, 1, 1); // mostly hidden, and far off the band
	for (int s = 0; s < 2; s++) {
		Step(t, f, 60, 30, 30);
	}
	assert_null(RowFor(t, "disp")->lockedFraction);
	Step(t, f, 60, 30, 30);
	assert_non_null(RowFor(t, "disp")->lockedFraction);
}

static void test_lock_exits_after_five_seconds_outside_band(void **)
{
	Tracker t;
	Feed f("disp", Kind::Wgc);
	Prime(t, f);
	for (int s = 0; s < 15; s++) {
		Step(t, f, 60, 30, 30);
	}
	assert_non_null(RowFor(t, "disp")->lockedFraction);
	for (int s = 0; s < 4; s++) {
		Step(t, f, 60, 57, 57);
		assert_non_null(RowFor(t, "disp")->lockedFraction);
	}
	Step(t, f, 60, 57, 57);
	assert_null(RowFor(t, "disp")->lockedFraction);
}

static void test_async_below(void **)
{
	Tracker t;
	Feed cam("cam", Kind::Async);
	Prime(t, cam);
	Step(t, cam, 60, 30, 60);
	assert_true(RowFor(t, "cam")->below);

	Tracker t2;
	Feed ok("cam", Kind::Async);
	Prime(t2, ok);
	Step(t2, ok, 60, 30, 30);
	assert_false(RowFor(t2, "cam")->below);
}

// Reference fps comes from LIVE canvases only; a preview-only canvas sets no rule.
static void test_ref_from_live_canvases_only(void **)
{
	const std::vector<Reach> reach{Reach{30.0, true}, Reach{60.0, false}};
	const std::optional<double> ref = RefFps(reach, kMainFps);
	assert_true(ref.has_value());
	assert_true(std::fabs(*ref - 30.0) < 0.001);

	assert_false(RefFps({Reach{60.0, false}}, kMainFps).has_value());
	assert_false(RefFps({}, kMainFps).has_value());
	// A canvas faster than main is still bounded by main's tick rate.
	assert_true(std::fabs(*RefFps({Reach{120.0, true}}, kMainFps) - 60.0) < 0.001);

	// And it drives the rule: at ref 30 a 30 fps async source is not below.
	Tracker t;
	Feed cam("cam", Kind::Async, 30.0, true);
	cam.src.reach.push_back(Reach{60.0, false});
	Prime(t, cam);
	Step(t, cam, 60, 30, 30);
	const Row *r = RowFor(t, "cam");
	assert_non_null(r);
	assert_true(r->refFps.has_value());
	assert_true(std::fabs(*r->refFps - 30.0) < 0.001);
	assert_false(r->below);
}

// A recreated source (same uuid, new object) starts with low counters. That is a
// new baseline with no delta, not a huge modular jump.
static void test_new_identity_rebaselines(void **)
{
	Tracker t;
	Feed f("disp", Kind::Dxgi);
	Prime(t, f);
	for (int s = 0; s < 3; s++) {
		Step(t, f, 60, 60, 60);
	}
	f.src.identity = 2;
	f.src.counts = Counts{Kind::Dxgi, 0, 0, 0};
	Step(t, f, 5, 5, 5);
	const Row *r = RowFor(t, "disp");
	assert_false(r->rate.has_value());
	assert_true(r->inGrace);
	Step(t, f, 60, 60, 60);
	assert_true(std::fabs(*RowFor(t, "disp")->rate - 60.0) < 0.01);
}

// Counters wrap as uint32; the delta across the wrap is still the true count.
static void test_uint32_wrap_delta(void **)
{
	Tracker t;
	Feed f("disp", Kind::Wgc);
	f.src.counts = Counts{Kind::Wgc, 0xFFFFFFF0u, 0xFFFFFFF0u, 0xFFFFFFF0u};
	Prime(t, f);
	Step(t, f, 60, 30, 30);
	const Row *r = RowFor(t, "disp");
	assert_true(std::fabs(*r->rate - 30.0) < 0.01);
}

// A kind change (method switch) starts a new window: no delta, grace again.
static void test_kind_change_rebaselines(void **)
{
	Tracker t;
	Feed f("disp", Kind::Wgc);
	Prime(t, f);
	for (int s = 0; s < 8; s++) {
		Step(t, f, 60, 60, 60);
	}
	assert_false(RowFor(t, "disp")->inGrace);
	f.src.counts.kind = Kind::Dxgi;
	Step(t, f, 60, 60, 500);
	assert_false(RowFor(t, "disp")->rate.has_value());
	assert_true(RowFor(t, "disp")->inGrace);
}

static void test_unmeasurable_and_idle(void **)
{
	Tracker t;
	Feed bitblt("win", Kind::None);
	bitblt.src.frameSignal = false;
	Prime(t, bitblt);
	Step(t, bitblt, 0, 0, 0);
	assert_int_equal((int)RowFor(t, "win")->status, (int)Status::Unmeasurable);

	bitblt.src.showing = false;
	Step(t, bitblt, 0, 0, 0);
	assert_int_equal((int)RowFor(t, "win")->status, (int)Status::Idle);

	// Showing but counting nothing with a method that does count (a stopped
	// camera, a monitor capture whose duplicator failed): not capturing, which
	// is not the same claim as "cannot be measured".
	Tracker t2;
	Feed cam("cam", Kind::None);
	Prime(t2, cam);
	Step(t2, cam, 60, 0, 0);
	assert_int_equal((int)RowFor(t2, "cam")->status, (int)Status::Idle);
	t2.SessionBegin(0);
	Step(t2, cam, 60, 0, 0);
	const std::string line = t2.SessionEnd(1000000000ull);
	assert_true(line.find("unmeasurable") == std::string::npos);
	assert_true(line.find("'cam' idle (never counted)") != std::string::npos);
}

// Which sources that report no frame-count kind still get a row, and which of
// those can be measured at all.
static void test_listing_and_frame_signal(void **)
{
	auto traits = [](const char *id) {
		SourceTraits t;
		t.id = id;
		return t;
	};

	assert_true(IsListed(traits(kMonitorCaptureId)));
	assert_true(HasFrameSignal(traits(kMonitorCaptureId)));

	// A counting hook reports GAME_HOOK, so a game capture that reports no kind
	// is either unhooked (no size, nothing to count) or hooked by a hook from
	// before the counters (a size, no counter), and only the latter says why.
	SourceTraits game = traits(kGameCaptureId);
	assert_true(IsListed(game));
	assert_true(HasFrameSignal(game));
	game.hasSize = true;
	assert_false(HasFrameSignal(game));
	assert_non_null(UnmeasurableNote(game));
	assert_non_null(strstr(UnmeasurableNote(game), "restart the game"));
	assert_null(UnmeasurableNote(traits(kMonitorCaptureId)));
	assert_null(UnmeasurableNote(traits(kWindowCaptureId)));

	SourceTraits window = traits(kWindowCaptureId);
	assert_true(IsListed(window));
	assert_false(HasFrameSignal(window));
	window.windowWgc = true;
	assert_true(HasFrameSignal(window));

	SourceTraits async = traits("ffmpeg_source");
	async.async = true;
	assert_true(IsListed(async));
	assert_true(HasFrameSignal(async));
	async.deinterlaced = true;
	assert_false(HasFrameSignal(async));

	assert_false(IsListed(traits("image_source")));
	assert_false(IsListed(traits("browser_source")));
	assert_false(IsListed(traits("")));
}

// A Game Capture feed as the sampler builds it for a source reporting no kind:
// unhooked, or hooked by a hook without the counters marker.
static Feed GameCaptureFeed(bool hooked)
{
	SourceTraits traits;
	traits.id = kGameCaptureId;
	traits.hasSize = hooked;
	Feed game("Game Capture", Kind::None, 30.0, true);
	game.src.frameSignal = HasFrameSignal(traits);
	game.src.unmeasurableNote = UnmeasurableNote(traits);
	return game;
}

// Marker absent: a game still holding an older hook, on a 30 fps canvas (a
// vertical Shorts destination). The source is named as unmeasurable with the
// reason, never warns, and the session line no longer claims there were no
// capture sources.
static void test_game_capture_reads_unmeasurable(void **)
{
	Tracker t;
	t.SessionBegin(0);
	Feed game = GameCaptureFeed(true);
	Prime(t, game);
	for (int s = 0; s < 20; s++) {
		Step(t, game, 0, 0, 0);
		const Row *r = RowFor(t, "Game Capture");
		assert_non_null(r);
		assert_int_equal((int)r->status, (int)Status::Unmeasurable);
		assert_non_null(r->unmeasurableNote);
		assert_false(r->below);
		assert_false(r->rate.has_value());
		assert_false(r->inputFps.has_value());
		assert_null(r->lockedFraction);
		assert_true(r->refFps.has_value());
		assert_true(std::fabs(*r->refFps - 30.0) < 0.001);
	}
	const std::string line = t.SessionEnd(20ull * 1000000000ull);
	assert_non_null(strstr(line.c_str(), "'Game Capture' unmeasurable"));
	assert_null(strstr(line.c_str(), "no capture sources"));

	// Hidden for the whole session: not capturing, so nothing to name.
	Tracker hidden;
	hidden.SessionBegin(0);
	game.src.showing = false;
	Prime(hidden, game);
	Step(hidden, game, 0, 0, 0);
	const Row *r = RowFor(hidden, "Game Capture");
	assert_non_null(r);
	assert_int_equal((int)r->status, (int)Status::Idle);
	assert_non_null(strstr(hidden.SessionEnd(2ull * 1000000000ull).c_str(), "no capture sources"));
}

// Showing but not hooked into anything: it reports no size and has nothing to
// count, which is idle rather than unmeasurable. It is still named.
static void test_unhooked_game_capture_reads_idle(void **)
{
	Tracker t;
	t.SessionBegin(0);
	Feed game = GameCaptureFeed(false);
	Prime(t, game);
	Step(t, game, 0, 0, 0);
	const Row *r = RowFor(t, "Game Capture");
	assert_non_null(r);
	assert_int_equal((int)r->status, (int)Status::Idle);
	assert_null(r->unmeasurableNote);
	const std::string line = t.SessionEnd(2ull * 1000000000ull);
	assert_non_null(strstr(line.c_str(), "'Game Capture' idle (never counted)"));
	assert_null(strstr(line.c_str(), "no capture sources"));
}

static void StepGame(Tracker &t, Feed &f, uint32_t ticks, uint32_t presents, uint32_t copies, uint32_t newTicks)
{
	f.AdvanceGame(ticks, presents, copies, newTicks);
	SampleAll(t, {&f});
}

// A counting hook: presents/s, copies/s and new frames/s, never a display rate,
// and a game outrunning the canvas is not "below" (new frames are judged against
// the canvas). The session line names all three medians and the reference.
static void test_game_hook_reports_presents_and_copies(void **)
{
	Tracker t;
	t.SessionBegin(0);
	Feed game("Game Capture", Kind::GameHook);
	Prime(t, game);
	assert_false(RowFor(t, "Game Capture")->inputFps.has_value());
	for (int s = 0; s < 10; s++) {
		StepGame(t, game, 60, 120, 61, 60);
		const Row *r = RowFor(t, "Game Capture");
		assert_non_null(r);
		assert_int_equal((int)r->status, (int)Status::Ok);
		assert_true(std::fabs(*r->inputFps - 120.0) < 0.01);
		assert_true(std::fabs(*r->copiesFps - 61.0) < 0.01);
		assert_true(std::fabs(*r->renderedFps - 60.0) < 0.01);
		assert_false(r->rate.has_value());
		assert_false(r->fraction.has_value());
		assert_null(r->lockedFraction);
		assert_false(r->below);
		assert_null(r->unmeasurableNote);
	}
	const std::string line = t.SessionEnd(10ull * 1000000000ull);
	assert_non_null(
		strstr(line.c_str(), "'Game Capture' game presents 120.0 copies 61.0 new 60.0 (ref 60), below 0.0%"));
	assert_null(strstr(line.c_str(), "unmeasurable"));
	assert_null(strstr(line.c_str(), "median"));
}

// Warn when new frames < 0.9 x min(presents, expected), and only with a live
// reference. The copies never decide it: they can outnumber the new frames.
static void test_game_hook_below(void **)
{
	struct Case {
		uint32_t presents;
		uint32_t copies;
		uint32_t newTicks;
		bool below;
	};
	const Case cases[] = {
		{60, 40, 40, true},    // lost a third of the game's frames
		{60, 54, 54, false},   // exactly 0.9 x 60
		{60, 53, 53, true},    // just under
		{30, 30, 30, false},   // a 30 fps game on a 60 fps canvas
		{30, 26, 26, true},    // 26 < 0.9 x 30
		{120, 55, 55, false},  // judged against the canvas (60), not the presents
		{120, 50, 50, true},   // 50 < 0.9 x 60
		{60, 60, 50, true},    // phase drift: every frame copied, but ten pairs landed in one tick
		{240, 120, 60, false}, // a frame generation ring copying two per tick, drawn one per tick
		{240, 120, 50, true},  // the same ring, its slots drawn on only 50 ticks
		{0, 0, 0, false},      // a paused game presents nothing, and misses nothing
	};
	for (const Case &c : cases) {
		Tracker t;
		Feed game("g", Kind::GameHook);
		Prime(t, game);
		StepGame(t, game, 60, c.presents, c.copies, c.newTicks);
		const Row *r = RowFor(t, "g");
		assert_non_null(r);
		assert_int_equal((int)r->below, (int)c.below);
		assert_true(std::fabs(*r->copiesFps - c.copies) < 0.01);
		assert_true(std::fabs(*r->renderedFps - c.newTicks) < 0.01);
	}

	// Off air (no live canvas) there is no rule, so no warning.
	Tracker off;
	Feed game("g", Kind::GameHook, kMainFps, false);
	Prime(off, game);
	StepGame(off, game, 60, 60, 10, 10);
	assert_false(RowFor(off, "g")->below);
	assert_true(std::fabs(*RowFor(off, "g")->renderedFps - 10.0) < 0.01);

	// A 30 fps canvas halves what the canvas can take: 28 new frames of 60
	// presents is fine there, 26 is not.
	for (uint32_t frames : {28u, 26u}) {
		Tracker slow;
		Feed g30("g", Kind::GameHook, 30.0, true);
		Prime(slow, g30);
		StepGame(slow, g30, 60, 60, frames, frames);
		assert_int_equal((int)RowFor(slow, "g")->below, (int)(frames == 26u));
	}

	// The session line carries the share of live seconds that were below.
	Tracker s;
	s.SessionBegin(0);
	Feed g("g", Kind::GameHook);
	Prime(s, g);
	for (int i = 0; i < 9; i++) {
		StepGame(s, g, 60, 60, 60, 60);
	}
	StepGame(s, g, 60, 60, 30, 30);
	assert_non_null(strstr(s.SessionEnd(10ull * 1000000000ull).c_str(), ", below 10.0%"));
}

// Unhook and re-hook: the kind goes NONE and back, and the counters come back
// wherever the plugin's cumulative sum left them. No delta crosses the gap; the
// first sample after the re-hook is a new baseline.
static void test_game_hook_rebaselines_on_rehook(void **)
{
	Tracker t;
	Feed game("g", Kind::GameHook);
	Prime(t, game);
	StepGame(t, game, 60, 60, 60, 60);
	assert_true(std::fabs(*RowFor(t, "g")->renderedFps - 60.0) < 0.01);

	game.src.counts.kind = Kind::None; // unhooked: no size, nothing to count
	StepGame(t, game, 0, 0, 0, 0);
	assert_int_equal((int)RowFor(t, "g")->status, (int)Status::Idle);

	game.src.counts.kind = Kind::GameHook;
	StepGame(t, game, 60, 5000, 5000, 60);
	const Row *r = RowFor(t, "g");
	assert_false(r->inputFps.has_value());
	assert_false(r->renderedFps.has_value());
	assert_false(r->copiesFps.has_value());
	assert_false(r->below);
	StepGame(t, game, 60, 60, 59, 59);
	r = RowFor(t, "g");
	assert_true(std::fabs(*r->inputFps - 60.0) < 0.01);
	assert_true(std::fabs(*r->renderedFps - 59.0) < 0.01);

	// A new source object behind the same uuid: new identity, lower counts, no
	// delta and never read as a reset from the drop.
	game.src.identity = 2;
	game.src.counts = Counts{Kind::GameHook, 0, 0, 0, 0};
	StepGame(t, game, 60, 60, 60, 60);
	assert_false(RowFor(t, "g")->inputFps.has_value());
}

// Presents and copies wrap as uint32 like every other counter.
static void test_game_hook_uint32_wrap(void **)
{
	Tracker t;
	Feed game("g", Kind::GameHook);
	game.src.counts = Counts{Kind::GameHook, 0xFFFFFFF0u, 0xFFFFFFF0u, 0xFFFFFFF0u, 0xFFFFFFE0u};
	Prime(t, game);
	StepGame(t, game, 60, 120, 60, 60);
	const Row *r = RowFor(t, "g");
	assert_true(std::fabs(*r->inputFps - 120.0) < 0.01);
	assert_true(std::fabs(*r->copiesFps - 60.0) < 0.01);
	assert_true(std::fabs(*r->renderedFps - 60.0) < 0.01);
	assert_false(r->below);
}

static void AssertReport(const hook_frame_report &report, uint32_t offered, uint32_t delivered, bool newFrame)
{
	assert_int_equal(report.offered, offered);
	assert_int_equal(report.delivered, delivered);
	assert_int_equal((int)report.new_frame, (int)newFrame);
}

// The plugin's relay from the hook's counters to one tick's report. The first
// step after a reset only baselines, whatever the counters read.
static void test_hook_relay_baselines_after_reset(void **)
{
	hook_frame_relay relay = {};
	hook_frame_relay_reset(&relay);
	AssertReport(hook_frame_relay_step(&relay, 5000, 4000, false, 0), 0, 0, false);
	AssertReport(hook_frame_relay_step(&relay, 5002, 4001, false, 0), 2, 1, true);

	// A re-hook resets: counters that went down are a baseline, not a huge delta.
	hook_frame_relay_reset(&relay);
	AssertReport(hook_frame_relay_step(&relay, 3, 2, false, 0), 0, 0, false);
	AssertReport(hook_frame_relay_step(&relay, 4, 3, false, 0), 1, 1, true);
}

// One shared texture: any copy since the last tick is a new frame, and two in
// one tick are still one.
static void test_hook_relay_plain_path(void **)
{
	hook_frame_relay relay = {};
	hook_frame_relay_step(&relay, 100, 100, false, 0);
	AssertReport(hook_frame_relay_step(&relay, 102, 102, false, 0), 2, 2, true);
	AssertReport(hook_frame_relay_step(&relay, 103, 102, false, 0), 1, 0, false);
	AssertReport(hook_frame_relay_step(&relay, 104, 103, false, 0), 1, 1, true);
}

// A frame generation ring: a burst of copies lands in one tick, and the source
// draws it out over the following ones. A tick is new when the host drew a newer
// slot, whatever the copies did.
static void test_hook_relay_ring_path(void **)
{
	hook_frame_relay relay = {};
	hook_frame_relay_step(&relay, 10, 10, true, 7);
	AssertReport(hook_frame_relay_step(&relay, 12, 14, true, 8), 2, 4, true);  // burst, first slot drawn
	AssertReport(hook_frame_relay_step(&relay, 12, 14, true, 9), 0, 0, true);  // no copy, next slot drawn
	AssertReport(hook_frame_relay_step(&relay, 12, 14, true, 9), 0, 0, false); // nothing newer to draw
	AssertReport(hook_frame_relay_step(&relay, 13, 16, true, 9), 1, 2, false); // copied, not yet due
	AssertReport(hook_frame_relay_step(&relay, 13, 16, true, 11), 0, 0, true); // drawn, skipping one
}

// Counter wrap is an ordinary step.
static void test_hook_relay_uint32_wrap(void **)
{
	hook_frame_relay relay = {};
	hook_frame_relay_step(&relay, 0xFFFFFFFFu, 0xFFFFFFFEu, false, 0);
	AssertReport(hook_frame_relay_step(&relay, 1, 0, false, 0), 2, 2, true);
}

// A display capture that showed all session and never counted a frame (a stale
// plugin, a failed duplicator) is named, not reported as an absence (#28).
static void test_display_capture_that_never_counted_is_named(void **)
{
	SourceTraits traits;
	traits.id = kMonitorCaptureId;
	Tracker t;
	t.SessionBegin(0);
	Feed disp("Display", Kind::None);
	disp.src.frameSignal = HasFrameSignal(traits);
	Prime(t, disp);
	for (int s = 0; s < 10; s++) {
		Step(t, disp, 0, 0, 0);
		const Row *r = RowFor(t, "Display");
		assert_non_null(r);
		assert_int_equal((int)r->status, (int)Status::Idle);
	}
	const std::string line = t.SessionEnd(10ull * 1000000000ull);
	assert_non_null(strstr(line.c_str(), "'Display' idle (never counted)"));
	assert_null(strstr(line.c_str(), "no capture sources"));
}

// Render lag above half: the display capture counts frames every second, but no
// second is mostly live, so none is judged. It counted, which "idle (never
// counted)" would deny.
static void test_counted_under_heavy_lag_is_not_idle(void **)
{
	Tracker t;
	t.SessionBegin(0);
	Feed disp("Display", Kind::Dxgi);
	Prime(t, disp);
	for (int s = 0; s < 10; s++) {
		Step(t, disp, 20, 20, 20);
		const Row *r = RowFor(t, "Display");
		assert_non_null(r);
		assert_int_equal((int)r->status, (int)Status::Ok);
	}
	const std::string line = t.SessionEnd(10ull * 1000000000ull);
	assert_non_null(strstr(line.c_str(), "'Display' DXGI counted, no second measured"));
	assert_null(strstr(line.c_str(), "never counted"));
	assert_null(strstr(line.c_str(), "no capture sources"));
}

// A session that only ever takes the restart sample (a go-live that fails within
// a second): the source reported its kind, and the line says so.
static void test_restart_only_session_is_not_idle(void **)
{
	Tracker t;
	t.SessionBegin(0);
	Feed disp("Display", Kind::Wgc);
	Prime(t, disp);
	const std::string line = t.SessionEnd(1000000000ull);
	assert_non_null(strstr(line.c_str(), "'Display' WGC counted, no second measured"));
	assert_null(strstr(line.c_str(), "never counted"));
	assert_null(strstr(line.c_str(), "no capture sources"));
}

// A DXGI display capture and a hooked game capture live together: one line names
// both.
static void test_display_and_game_capture_share_the_line(void **)
{
	Tracker t;
	t.SessionBegin(0);
	Feed disp("Display", Kind::Dxgi);
	Feed game = GameCaptureFeed(true);
	auto second = [&](uint32_t frames) {
		disp.Advance(60, frames, frames);
		SampleAll(t, {&disp, &game});
	};
	second(0);
	for (int s = 0; s < 10; s++) {
		second(60);
	}
	const std::string line = t.SessionEnd(10ull * 1000000000ull);
	assert_non_null(strstr(line.c_str(), "'Display' DXGI median 60.0/s"));
	assert_non_null(strstr(line.c_str(), "'Game Capture' unmeasurable"));
	assert_null(strstr(line.c_str(), "no capture sources"));
}

// The session line names each source with its median and lock; stats.reset
// rebases the panel window only and leaves the session sums alone.
static void test_session_summary_and_reset(void **)
{
	Tracker t;
	t.SessionBegin(0);
	Feed disp("SM - Display Capture", Kind::Wgc);
	Feed cam("Cam", Kind::Async);
	auto both = [&](uint32_t dispFrames, uint32_t camIn, uint32_t camOut) {
		disp.Advance(60, dispFrames, dispFrames);
		cam.Advance(60, camOut, camIn);
		SampleAll(t, {&disp, &cam});
	};
	both(0, 0, 0);
	for (int s = 0; s < 40; s++) {
		both(30, 30, 30);
	}
	t.ResetWindows();
	const Row *r = RowFor(t, "SM - Display Capture");
	assert_true(r->sinceReset.liveSec == 0.0);
	for (int s = 0; s < 10; s++) {
		both(30, 30, 30);
	}
	const std::string line = t.SessionEnd(5412ull * 1000000000ull);
	assert_non_null(strstr(line.c_str(), "[capture-rate] session 5412 s: "));
	assert_non_null(strstr(line.c_str(), "'SM - Display Capture' WGC median 30.0/s (ref 60)"));
	// 50 live seconds, locked from the 15th (5 s grace plus a 10 s streak): 36 of 50.
	// Had the reset touched the session sums this would read 100%.
	assert_non_null(strstr(line.c_str(), "locked 1/2 72%"));
	assert_non_null(strstr(line.c_str(), "'Cam' async in 30.0 out 30.0, below 0.0%"));
	assert_false(t.InSession());
}

// Several live transitions can coalesce into one edge, and the history database
// that used to gate the edge may not be open at all, so a begin can arrive while a
// session is already open. It must neither wipe the sums nor move the start.
static void test_repeated_session_begin_keeps_the_session(void **)
{
	Tracker t;
	t.SessionBegin(0);
	Feed f("disp", Kind::Dxgi);
	Prime(t, f);
	for (int s = 0; s < 15; s++) {
		Step(t, f, 60, 60, 60);
	}
	t.SessionBegin(15ull * 1000000000ull);
	for (int s = 0; s < 5; s++) {
		Step(t, f, 60, 30, 30);
	}
	const std::string line = t.SessionEnd(20ull * 1000000000ull);
	assert_non_null(strstr(line.c_str(), "[capture-rate] session 20 s: "));
	// 15 s at 60 and 5 s at 30: a wiped session would read 30.0.
	assert_non_null(strstr(line.c_str(), "'disp' DXGI median 60.0/s"));
	assert_true(t.SessionEnd(21ull * 1000000000ull).empty());
}

// Watching with nothing live (a lease) samples a source for as long as the panel
// is open. Going live is where the reference appears, and the grace period has to
// run from there: a lock needs 5 s of grace plus 10 eligible seconds after it.
static void test_grace_restarts_on_going_live(void **)
{
	Tracker t;
	Feed f("disp", Kind::Wgc, kMainFps, false);
	Prime(t, f);
	for (int s = 0; s < 20; s++) {
		Step(t, f, 60, 30, 30);
	}
	assert_false(RowFor(t, "disp")->inGrace);

	f.src.reach = {Reach{kMainFps, true}};
	for (int s = 1; s <= 5; s++) {
		Step(t, f, 60, 30, 30);
		assert_true(RowFor(t, "disp")->inGrace);
	}
	for (int s = 6; s <= 14; s++) {
		Step(t, f, 60, 30, 30);
		assert_false(RowFor(t, "disp")->inGrace);
		assert_null(RowFor(t, "disp")->lockedFraction);
	}
	Step(t, f, 60, 30, 30);
	assert_non_null(RowFor(t, "disp")->lockedFraction);

	// A second broadcast starts over rather than inheriting the first one's lock.
	f.src.reach = {Reach{kMainFps, false}};
	Step(t, f, 60, 30, 30);
	f.src.reach = {Reach{kMainFps, true}};
	Step(t, f, 60, 30, 30);
	assert_true(RowFor(t, "disp")->inGrace);
	assert_null(RowFor(t, "disp")->lockedFraction);
}

// No reference means no rule (plan 1.4): once the broadcast ends and only a
// lease keeps sampling, the row shows the rate alone. A lock from the broadcast
// must not linger, nor count as locked time.
static void test_lock_ends_with_the_broadcast(void **)
{
	Tracker t;
	Feed f("disp", Kind::Wgc);
	Prime(t, f);
	for (int s = 0; s < 15; s++) {
		Step(t, f, 60, 30, 30);
	}
	assert_non_null(RowFor(t, "disp")->lockedFraction);

	f.src.reach = {Reach{kMainFps, false}};
	t.ResetWindows();
	t.SessionBegin(0);
	for (int s = 0; s < 8; s++) {
		Step(t, f, 60, 30, 30);
		const Row *r = RowFor(t, "disp");
		assert_false(r->refFps.has_value());
		assert_null(r->lockedFraction);
		assert_true(r->rate.has_value());
	}
	assert_true(RowFor(t, "disp")->sinceReset.lockedSec == 0.0);
	assert_null(strstr(t.SessionEnd(8ull * 1000000000ull).c_str(), "locked"));
}

// A source removed mid-session keeps its name and final sums in the line.
static void test_removed_source_keeps_its_sums(void **)
{
	Tracker t;
	t.SessionBegin(0);
	Feed f("Gone", Kind::Dxgi);
	Prime(t, f);
	for (int s = 0; s < 3; s++) {
		Step(t, f, 60, 60, 60);
	}
	SampleInput empty;
	empty.dtSec = 1.0;
	empty.mainFps = kMainFps;
	t.Sample(empty);
	assert_null(RowFor(t, "Gone"));
	const std::string line = t.SessionEnd(10ull * 1000000000ull);
	assert_non_null(strstr(line.c_str(), "'Gone' DXGI median 60.0/s"));
}

// An overlay's paint rate is a rate and nothing more: a steady half cadence gets
// no fraction and no lock note, and the session line names its median, which is
// the answer to "does this overlay really paint at 60".
static void test_browser_paint_reports_a_rate_only(void **)
{
	Tracker t;
	t.SessionBegin(0);
	Feed overlay("Alerts", Kind::BrowserPaint);
	Prime(t, overlay);
	for (int s = 0; s < 30; s++) {
		Step(t, overlay, 60, 30, 30);
		const Row *r = RowFor(t, "Alerts");
		assert_non_null(r);
		assert_int_equal((int)r->status, (int)Status::Ok);
		assert_true(std::fabs(*r->rate - 30.0) < 0.01);
		assert_false(r->fraction.has_value());
		assert_null(r->lockedFraction);
		assert_false(r->inGrace);
		assert_false(r->below);
	}
	const std::string line = t.SessionEnd(30ull * 1000000000ull);
	assert_non_null(strstr(line.c_str(), "'Alerts' paint median 30.0/s (ref 60)"));
	assert_null(strstr(line.c_str(), "locked"));
}

// While live, a game hook source logs one line per 10 s of measured samples with
// the window's presents, copies and new frames per second, tagged like game
// capture's own per-window lines. Off air nothing accumulates, and a partial
// window ends with the session.
static void test_game_hook_window_lines(void **)
{
	Tracker t;
	Feed game("Game Capture", Kind::GameHook);
	Prime(t, game);
	for (int s = 0; s < 12; s++) {
		StepGame(t, game, 60, 120, 115, 59);
	}
	assert_true(t.TakeWindowLines().empty());

	t.SessionBegin(0);
	for (int s = 0; s < 9; s++) {
		StepGame(t, game, 60, 120, 115, 59);
	}
	assert_true(t.TakeWindowLines().empty());
	StepGame(t, game, 60, 120, 115, 59);
	std::vector<std::string> lines = t.TakeWindowLines();
	assert_int_equal(lines.size(), 1);
	assert_string_equal(lines[0].c_str(), "[capture-rate] [10s] 'Game Capture' game 10.0 s: presents 120.0 copies "
					      "115.0 new 59.0 /s, below 0.0 s");
	assert_non_null(strstr(lines[0].c_str(), FGC_STATS_TAG));
	assert_true(t.TakeWindowLines().empty());

	for (int s = 0; s < 10; s++) {
		StepGame(t, game, 60, 60, 60, 40);
	}
	lines = t.TakeWindowLines();
	assert_int_equal(lines.size(), 1);
	assert_non_null(strstr(lines[0].c_str(), "presents 60.0 copies 60.0 new 40.0 /s, below 10.0 s"));

	for (int s = 0; s < 5; s++) {
		StepGame(t, game, 60, 120, 115, 59);
	}
	t.SessionEnd(25ull * 1000000000ull);
	t.SessionBegin(0);
	for (int s = 0; s < 5; s++) {
		StepGame(t, game, 60, 120, 115, 59);
	}
	assert_true(t.TakeWindowLines().empty());
}

// Clear drops a finished window line nobody took yet, along with the sums.
static void test_clear_drops_pending_window_lines(void **)
{
	Tracker t;
	t.SessionBegin(0);
	Feed game("Game Capture", Kind::GameHook);
	Prime(t, game);
	for (int s = 0; s < 10; s++) {
		StepGame(t, game, 60, 120, 115, 59);
	}
	t.Clear();
	assert_true(t.TakeWindowLines().empty());
}

// The session line adds the 10th and 25th percentile of presents and new frames,
// so a dip that holds a fifth of the stream shows even when the median hides it.
static void test_game_hook_session_quantiles(void **)
{
	Tracker t;
	t.SessionBegin(0);
	Feed game("Game Capture", Kind::GameHook);
	Prime(t, game);
	for (int s = 0; s < 4; s++) {
		StepGame(t, game, 60, 60, 50, 40);
	}
	for (int s = 0; s < 16; s++) {
		StepGame(t, game, 60, 120, 115, 60);
	}
	const std::string line = t.SessionEnd(20ull * 1000000000ull);
	assert_non_null(strstr(line.c_str(), "'Game Capture' game presents 120.0 copies 115.0 new 60.0 (ref 60), below "
					     "20.0%, presents p10 60.0 p25 120.0, new p10 40.0 p25 60.0"));
}

// The capture hook's per-window counters: burst sizes with the size cap apart,
// the extremes, and copy-time percentiles from 25 us bins.
static void test_fgc_hook_stats_line(void **)
{
	struct fgc_hook_stats s;
	memset(&s, 0, sizeof(s));
	s.presents = 1198;
	s.copies = 1150;
	for (int i = 0; i < 3; i++) {
		fgc_hook_stats_burst(&s, 1, false);
	}
	fgc_hook_stats_burst(&s, 3, false);
	for (int i = 0; i < 295; i++) {
		fgc_hook_stats_burst(&s, 4, false);
	}
	fgc_hook_stats_burst(&s, 4, true);
	fgc_hook_stats_burst(&s, 4, true);
	s.pace_resets = 1;
	s.lead_clamps = 2;
	s.lead_resyncs = 3;
	s.ring_full = 4;
	s.bucket_skips = 48;
	fgc_hook_stats_span(&s, 400000);
	fgc_hook_stats_span(&s, 620000);
	fgc_hook_stats_span(&s, 100000);
	fgc_hook_stats_gap(&s, 30000000);
	fgc_hook_stats_gap(&s, 24100000);
	fgc_hook_stats_gap(&s, 33000000);
	for (int i = 0; i < 98; i++) {
		fgc_hook_stats_copy_time(&s, 200000);
	}
	fgc_hook_stats_copy_time(&s, 900000);
	fgc_hook_stats_copy_time(&s, 900000);

	char line[512];
	assert_true(fgc_hook_stats_format(line, sizeof(line), &s, 10003000000ull, 8333000ull) > 0);
	assert_string_equal(
		line,
		"[10s] hook 10.0 s: presents 119.8/s copies 115.0/s, bursts 1:3 2:0 3:1 4:295 >4:2"
		", step 8333 us, pace resets 1, lead clamps 2, lead resyncs 3, bucket skips 48, ring full 4"
		", slot busy 0, copy fails 0, burst span max 620 us, burst gap min 24100 us, copy p50 225 p99 925 us");

	// Slower than the last bin reads as its upper edge; no copies read 0.
	struct fgc_hook_stats slow;
	memset(&slow, 0, sizeof(slow));
	assert_int_equal(fgc_hook_stats_copy_percentile_us(&slow, 50), 0);
	fgc_hook_stats_copy_time(&slow, 10000000);
	assert_int_equal(fgc_hook_stats_copy_percentile_us(&slow, 50), 4000);
	assert_int_equal(fgc_rate_x10(5, 0), 0);
}

// The host's ring pick, one count per tick end.
static void test_fgc_ring_stats_line(void **)
{
	struct fgc_ring_stats s = {590, 5, 3, 1, 1};
	char line[256];
	assert_true(fgc_ring_stats_format(line, sizeof(line), &s, 10000000000ull) > 0);
	assert_string_equal(line, "[10s] ring 10.0 s: ticks 600, new slot 590, no due slot 8 (none newer 5, newer "
				  "not due 3), acquire failed 1, seq changed 1");
}

// A zero-length window (a clock that did not move) formats as zero rates rather
// than dividing by it.
static void test_fgc_formatters_zero_window(void **)
{
	struct fgc_hook_stats hook;
	memset(&hook, 0, sizeof(hook));
	hook.presents = 5;
	hook.copies = 4;
	char line[512];
	assert_true(fgc_hook_stats_format(line, sizeof(line), &hook, 0, 0) > 0);
	assert_non_null(strstr(line, "[10s] hook 0.0 s: presents 0.0/s copies 0.0/s, bursts 1:0 2:0 3:0 4:0 >4:0"));

	struct fgc_ring_stats ring = {1, 0, 0, 0, 0};
	assert_true(fgc_ring_stats_format(line, sizeof(line), &ring, 0) > 0);
	assert_string_equal(line, "[10s] ring 0.0 s: ticks 1, new slot 1, no due slot 0 (none newer 0, newer not due "
				  "0), acquire failed 0, seq changed 0");
}

// The hook, the ring and capture-rate share one window length and one tag.
static_assert(Tracker::kWindowSec * 1e9 == FGC_STATS_WINDOW_NS, "capture-rate and FGC windows differ");

// The first call opens the window; it is due once its length has run.
static void test_fgc_stats_window(void **)
{
	uint64_t start = 0;
	assert_false(fgc_stats_window_due(&start, 5000));
	assert_int_equal(start, 5000);
	assert_false(fgc_stats_window_due(&start, 5000 + FGC_STATS_WINDOW_NS - 1));
	assert_true(fgc_stats_window_due(&start, 5000 + FGC_STATS_WINDOW_NS));
}

// Frame generation capture end to end: the pacer's stamps through the hook's
// bucket limiter and slot guard into the shared-texture ring, and the host's
// pick_ring_slot on canvas ticks, over synthetic Present traces. Each trace runs
// at three tick phases. Times are ns.
//
// Nothing here holds heap memory: a failed cmocka assertion leaves by longjmp,
// which does not reliably run C++ destructors on the way.
constexpr uint64_t kMs = 1000000;
constexpr uint64_t kSec = 1000000000;
constexpr uint64_t kCanvasNs = 16666667;
// SHTEX_RING_MAX; graphics-hook-info.h needs windows.h, so the model keeps its own.
constexpr uint32_t kRingSlots = 8;
constexpr size_t kTraceMax = 4000;
// Measures skip each trace's first half second, while the window fills.
constexpr uint64_t kSkipNs = 500000000;
constexpr uint64_t kBase20 = 50000000;
constexpr uint64_t kBase30 = 33333333;
constexpr uint64_t kBase45 = 22222222;
constexpr uint64_t k60Hz = 16666667;

// Deterministic per seed, unlike rand().
struct Lcg {
	uint64_t state;
	uint64_t Next()
	{
		state = state * 6364136223846793005ull + 1442695040888963407ull;
		return state >> 33;
	}
	// Uniform in [lo, hi) ns.
	uint64_t Uniform(uint64_t lo, uint64_t hi)
	{
		return lo + (uint64_t)((double)Next() / 2147483648.0 * (double)(hi - lo));
	}
};

struct PresentTrace {
	uint64_t t[kTraceMax];
	size_t count;
	// The first Present after a change of pace; 0 when there is none.
	uint64_t split;
	// When the hook restarts: a fresh pacer, bucket limiter and ring; 0 for never.
	uint64_t restart_at;
};

static void Clear(PresentTrace *tr)
{
	tr->count = 0;
	tr->split = 0;
	tr->restart_at = 0;
}

static void Push(PresentTrace *tr, uint64_t t)
{
	assert_true(tr->count < kTraceMax);
	tr->t[tr->count++] = t;
}

enum class Fg { Clean, Frag, Even, Rand, Split3Plus1 };

// One real frame per base period (jittered by up to 1 ms, and swung by drift
// over a 2 s cycle), each with factor Presents clustered as the kind places
// them, plus up to 0.2 ms of submission noise each. Appends, sorted.
static void FgTrace(PresentTrace *tr, Fg kind, size_t factor, uint64_t base, uint64_t seed, uint64_t dur = 10 * kSec,
		    double drift = 0.0, uint64_t start = 10 * kMs)
{
	Lcg rng{seed};
	const size_t first_index = tr->count;
	uint64_t frame = start;
	while (frame < start + dur) {
		uint64_t b = base;
		if (drift != 0.0) {
			b = (uint64_t)((double)base * (1.0 + drift * std::sin(2.0 * 3.141592653589793 *
									      (double)(frame - start) / 2e9)));
		}
		const uint64_t period = b - kMs + rng.Uniform(0, 2 * kMs);
		uint64_t offs[10] = {};
		assert_true(factor <= 10);
		switch (kind) {
		case Fg::Clean:
			for (size_t i = 1; i < factor; i++) {
				offs[i] = i * 150000;
			}
			break;
		case Fg::Frag: {
			const uint64_t first = factor > 1 ? 1 + rng.Next() % (factor - 1) : 1;
			const uint64_t hi = period - 3 * kMs > 3 * kMs + 1 ? period - 3 * kMs : 3 * kMs + 1;
			for (size_t i = 1; i < factor; i++) {
				offs[i] = i < first ? i * 600000 : rng.Uniform(3 * kMs, hi);
			}
			break;
		}
		case Fg::Even:
			for (size_t i = 1; i < factor; i++) {
				offs[i] = i * period / factor;
			}
			break;
		case Fg::Rand: {
			const uint64_t clusters = 1 + rng.Next() % factor;
			for (size_t i = 1; i < factor; i++) {
				const bool new_cluster = i * clusters / factor != (i - 1) * clusters / factor;
				offs[i] = offs[i - 1] + (new_cluster ? rng.Uniform(3 * kMs, 7 * kMs) : 200000);
			}
			break;
		}
		case Fg::Split3Plus1:
			offs[1] = 600000;
			offs[2] = 1200000;
			offs[3] = rng.Uniform(3 * kMs, 25 * kMs);
			break;
		}
		for (size_t i = 0; i < factor; i++) {
			Push(tr, frame + offs[i] + rng.Uniform(0, 200000));
		}
		frame += period;
	}
	std::sort(tr->t + first_index, tr->t + tr->count);
}

// Single Presents every interval, each jittered by up to jitter.
static void Singles(PresentTrace *tr, uint64_t interval, size_t n, uint64_t jitter = 0, uint64_t start = 10 * kMs)
{
	Lcg rng{1};
	uint64_t t = start;
	for (size_t i = 0; i < n; i++) {
		Push(tr, t + (jitter ? rng.Uniform(0, jitter) : 0));
		t += interval;
	}
}

// Variable refresh: intervals uniform in [lo, hi).
static void Vrr(PresentTrace *tr, uint64_t lo, uint64_t hi, size_t n, uint64_t seed)
{
	Lcg rng{seed};
	uint64_t t = 10 * kMs;
	for (size_t i = 0; i < n; i++) {
		Push(tr, t);
		t += rng.Uniform(lo, hi);
	}
}

struct Pace {
	uint64_t interval;
	size_t count;
};

// Runs of evenly spaced Presents, back to back: each run's Presents, and then
// the time to the next run, are its interval apart.
static void Seq(PresentTrace *tr, const Pace *parts, size_t n, uint64_t start = 10 * kMs)
{
	uint64_t t = start;
	for (size_t p = 0; p < n; p++) {
		for (size_t i = 0; i < parts[p].count; i++) {
			Push(tr, t);
			t += parts[p].interval;
		}
	}
}

static void Seq(PresentTrace *tr, std::initializer_list<Pace> parts, uint64_t start = 10 * kMs)
{
	Seq(tr, parts.begin(), parts.size(), start);
}

// Drops every Present from ns after the first on.
static void Cut(PresentTrace *tr, uint64_t ns)
{
	const uint64_t end = tr->t[0] + ns;
	size_t n = 0;
	while (n < tr->count && tr->t[n] < end) {
		n++;
	}
	tr->count = n;
}

// Appends b after tr, its first Present one of b's own first intervals after
// tr's last, and marks the change of pace there.
static void Cat(PresentTrace *tr, const PresentTrace *b)
{
	const uint64_t gap = b->t[1] - b->t[0];
	const uint64_t base = tr->t[tr->count - 1] + gap;
	tr->split = base;
	for (size_t i = 0; i < b->count; i++) {
		Push(tr, base + (b->t[i] - b->t[0]));
	}
}

struct RingRun {
	// Averaged over the tick phases.
	double distinct_per_s;
	// The rest are the worst over the phases.
	uint64_t max_gap_ns;
	// Waits counted from the change of pace on, so a slow pace before it
	// does not count.
	uint64_t max_gap_after_split_ns;
	uint64_t max_lead_ns;
	uint64_t max_latency_ns;
	uint64_t last_lead_ns;
};

static uint64_t WaitSinceSplit(const PresentTrace *tr, uint64_t tick, uint64_t last_new)
{
	if (!tr->split || tick < tr->split) {
		return 0;
	}
	return tick - (last_new > tr->split ? last_new : tr->split);
}

struct RingSlot {
	uint64_t show_ns;
	uint64_t frame_no;
	uint64_t present_ns;
};

// One phase. The hook side mirrors d3d12_ring_capture: every Present is
// stamped, the half-interval bucket limiter passes at most one per bucket, and
// d3d12_ring_acquire_slot walks round robin from the next slot, skipping slots
// whose stamp is under two canvas intervals old and the slot the host holds; a
// copy with no slot leaves the bucket open for the next Present. The host side
// mirrors pick_ring_slot: each tick takes the newest slot newer than the shown
// frame whose stamp is a canvas interval old, or the newest at all before the
// first. Each stamp is checked as it is made: strictly rising, never behind its
// Present, and at most the lead cap (plus the one ns strict increase may add)
// ahead of it.
static RingRun RunRing(const PresentTrace *tr, uint64_t canvas, uint64_t phase)
{
	const uint64_t lead_cap = fgc_pacer_lead_cap(canvas, kRingSlots);
	struct fgc_pacer pacer;
	fgc_pacer_init(&pacer, lead_cap);

	RingSlot slots[kRingSlots];
	memset(slots, 0, sizeof(slots));
	uint32_t next_slot = 0;
	int shown_slot = -1;
	uint64_t shown_frame = 0;
	uint64_t frame_no = 0;
	uint64_t last_bucket = 0;
	bool have_bucket = false;
	uint64_t prev_stamp = 0;
	bool restarted = false;

	RingRun run = {0.0, 0, 0, 0, 0, 0};
	const uint64_t t0 = tr->t[0];
	const uint64_t end = tr->t[tr->count - 1];
	uint64_t shown = 0;
	uint64_t tick = t0 + canvas + phase;
	uint64_t last_new = tick;
	size_t pi = 0;
	while (tick < end) {
		for (; pi < tr->count && tr->t[pi] <= tick; pi++) {
			const uint64_t t = tr->t[pi];
			if (tr->restart_at && !restarted && t >= tr->restart_at) {
				fgc_pacer_init(&pacer, lead_cap);
				memset(slots, 0, sizeof(slots));
				next_slot = 0;
				shown_slot = -1;
				shown_frame = 0;
				frame_no = 0;
				have_bucket = false;
				prev_stamp = 0;
				restarted = true;
			}

			const uint64_t stamp = fgc_pacer_stamp(&pacer, t).stamp;
			assert_true(stamp > prev_stamp);
			assert_true(stamp >= t);
			assert_true(stamp - t <= lead_cap + 1);
			prev_stamp = stamp;
			run.last_lead_ns = stamp - t;
			if (t - t0 >= kSkipNs && stamp - t > run.max_lead_ns) {
				run.max_lead_ns = stamp - t;
			}

			const uint64_t bucket = stamp / (canvas / 2);
			if (have_bucket && bucket <= last_bucket) {
				continue;
			}
			const uint64_t horizon = t > 2 * canvas ? t - 2 * canvas : 0;
			int slot = -1;
			for (uint32_t k = 0; k < kRingSlots; k++) {
				const uint32_t candidate = (next_slot + k) % kRingSlots;
				const uint64_t show = slots[candidate].show_ns;
				if ((show && show > horizon) || (int)candidate == shown_slot) {
					continue;
				}
				slot = (int)candidate;
				break;
			}
			if (slot < 0) {
				continue;
			}
			slots[slot] = {stamp, ++frame_no, t};
			next_slot = (uint32_t)(slot + 1) % kRingSlots;
			last_bucket = bucket;
			have_bucket = true;
		}

		const uint64_t target = shown_slot >= 0 ? tick - canvas : UINT64_MAX;
		int best = -1;
		for (int i = 0; i < (int)kRingSlots; i++) {
			const RingSlot &s = slots[i];
			if (!s.show_ns || i == shown_slot || s.frame_no <= shown_frame || s.show_ns > target) {
				continue;
			}
			if (best < 0 || s.show_ns > slots[best].show_ns) {
				best = i;
			}
		}
		if (best >= 0) {
			shown_slot = best;
			shown_frame = slots[best].frame_no;
			if (tick - t0 >= kSkipNs) {
				shown++;
				const uint64_t gap = tick - last_new;
				run.max_gap_ns = gap > run.max_gap_ns ? gap : run.max_gap_ns;
				const uint64_t wait = WaitSinceSplit(tr, tick, last_new);
				if (wait > run.max_gap_after_split_ns) {
					run.max_gap_after_split_ns = wait;
				}
				const uint64_t latency = tick - slots[best].present_ns;
				run.max_latency_ns = latency > run.max_latency_ns ? latency : run.max_latency_ns;
			}
			last_new = tick;
		}
		tick += canvas;
	}
	// A run that ends without a new frame for a while counts that wait too.
	if (shown && tick > last_new) {
		const uint64_t gap = tick - last_new;
		run.max_gap_ns = gap > run.max_gap_ns ? gap : run.max_gap_ns;
		const uint64_t wait = WaitSinceSplit(tr, tick, last_new);
		if (wait > run.max_gap_after_split_ns) {
			run.max_gap_after_split_ns = wait;
		}
	}
	run.distinct_per_s = (double)shown / ((double)(end - t0 - kSkipNs) / 1e9);
	return run;
}

static RingRun RunRingPhases(const PresentTrace *tr)
{
	RingRun worst = {0.0, 0, 0, 0, 0, 0};
	for (double f : {0.13, 0.47, 0.81}) {
		const RingRun r = RunRing(tr, kCanvasNs, (uint64_t)((double)kCanvasNs * f));
		worst.distinct_per_s += r.distinct_per_s / 3.0;
		worst.max_gap_ns = r.max_gap_ns > worst.max_gap_ns ? r.max_gap_ns : worst.max_gap_ns;
		worst.max_gap_after_split_ns = r.max_gap_after_split_ns > worst.max_gap_after_split_ns
						       ? r.max_gap_after_split_ns
						       : worst.max_gap_after_split_ns;
		worst.max_lead_ns = r.max_lead_ns > worst.max_lead_ns ? r.max_lead_ns : worst.max_lead_ns;
		worst.max_latency_ns = r.max_latency_ns > worst.max_latency_ns ? r.max_latency_ns
									       : worst.max_latency_ns;
		worst.last_lead_ns = r.last_lead_ns > worst.last_lead_ns ? r.last_lead_ns : worst.last_lead_ns;
	}
	return worst;
}

// What the design scorecard measured for each case on a 60 fps canvas: its
// distinct new frames per second less 5%, and its longest wait for a new frame
// in canvas ticks. Frame generation rows cover both seeds of a case.
struct RingFloor {
	const char *name;
	double distinct_per_s;
	uint64_t max_gap_ticks;
};

static const RingFloor kRingFloors[] = {
	{"fg 2x@20 clean", 38.0, 2},
	{"fg 2x@20 clean drift", 39.4, 3},
	{"fg 2x@20 frag", 37.7, 3},
	{"fg 2x@20 frag drift", 38.7, 4},
	{"fg 2x@20 even", 38.0, 2},
	{"fg 2x@20 even drift", 39.1, 2},
	{"fg 2x@30 clean", 56.6, 2},
	{"fg 2x@30 clean drift", 51.9, 2},
	{"fg 2x@30 frag", 55.4, 2},
	{"fg 2x@30 frag drift", 51.0, 3},
	{"fg 2x@30 even", 56.8, 2},
	{"fg 2x@30 even drift", 52.0, 2},
	{"fg 2x@45 clean", 57.0, 1},
	{"fg 2x@45 clean drift", 57.0, 1},
	{"fg 2x@45 frag", 57.0, 1},
	{"fg 2x@45 frag drift", 56.5, 2},
	{"fg 2x@45 even", 57.0, 1},
	{"fg 2x@45 even drift", 57.0, 1},
	{"fg 3x@20 clean", 55.4, 2},
	{"fg 3x@20 clean drift", 49.7, 2},
	{"fg 3x@20 frag", 53.4, 3},
	{"fg 3x@20 frag drift", 50.0, 3},
	{"fg 3x@20 even", 56.7, 2},
	{"fg 3x@20 even drift", 52.0, 2},
	{"fg 3x@30 clean", 57.0, 1},
	{"fg 3x@30 clean drift", 56.4, 2},
	{"fg 3x@30 frag", 56.5, 2},
	{"fg 3x@30 frag drift", 56.1, 2},
	{"fg 3x@30 even", 57.0, 1},
	{"fg 3x@30 even drift", 57.0, 2},
	{"fg 3x@45 clean", 57.0, 1},
	{"fg 3x@45 clean drift", 56.9, 2},
	{"fg 3x@45 frag", 56.8, 2},
	{"fg 3x@45 frag drift", 56.8, 2},
	{"fg 3x@45 even", 57.0, 1},
	{"fg 3x@45 even drift", 57.0, 1},
	{"fg 4x@20 clean", 57.0, 2},
	{"fg 4x@20 clean drift", 51.3, 3},
	{"fg 4x@20 frag", 55.5, 3},
	{"fg 4x@20 frag drift", 54.5, 3},
	{"fg 4x@20 even", 57.0, 1},
	{"fg 4x@20 even drift", 57.0, 1},
	{"fg 4x@30 clean", 57.0, 2},
	{"fg 4x@30 clean drift", 56.3, 2},
	{"fg 4x@30 frag", 56.7, 2},
	{"fg 4x@30 frag drift", 56.2, 2},
	{"fg 4x@30 even", 57.0, 1},
	{"fg 4x@30 even drift", 57.0, 1},
	{"fg 4x@45 clean", 57.0, 1},
	{"fg 4x@45 clean drift", 56.8, 2},
	{"fg 4x@45 frag", 57.0, 1},
	{"fg 4x@45 frag drift", 56.8, 2},
	{"fg 4x@45 even", 57.0, 1},
	{"fg 4x@45 even drift", 57.0, 1},
	{"fg 5x@20 clean", 55.1, 2},
	{"fg 5x@20 clean drift", 51.7, 2},
	{"fg 5x@20 frag", 56.2, 2},
	{"fg 5x@20 frag drift", 55.4, 3},
	{"fg 5x@20 even", 57.0, 1},
	{"fg 5x@20 even drift", 57.0, 1},
	{"fg 5x@30 clean", 56.9, 2},
	{"fg 5x@30 clean drift", 55.9, 2},
	{"fg 5x@30 frag", 56.7, 2},
	{"fg 5x@30 frag drift", 56.7, 2},
	{"fg 5x@30 even", 57.0, 1},
	{"fg 5x@30 even drift", 57.0, 1},
	{"fg 5x@45 clean", 57.0, 1},
	{"fg 5x@45 clean drift", 57.0, 1},
	{"fg 5x@45 frag", 57.0, 1},
	{"fg 5x@45 frag drift", 57.0, 1},
	{"fg 5x@45 even", 57.0, 1},
	{"fg 5x@45 even drift", 57.0, 1},
	{"fg 6x@20 clean", 56.3, 2},
	{"fg 6x@20 clean drift", 51.6, 3},
	{"fg 6x@20 frag", 56.2, 2},
	{"fg 6x@20 frag drift", 55.9, 3},
	{"fg 6x@20 even", 57.0, 1},
	{"fg 6x@20 even drift", 57.0, 1},
	{"fg 6x@30 clean", 56.9, 2},
	{"fg 6x@30 clean drift", 56.6, 2},
	{"fg 6x@30 frag", 56.9, 2},
	{"fg 6x@30 frag drift", 56.7, 2},
	{"fg 6x@30 even", 57.0, 1},
	{"fg 6x@30 even drift", 57.0, 1},
	{"fg 6x@45 clean", 57.0, 1},
	{"fg 6x@45 clean drift", 57.0, 1},
	{"fg 6x@45 frag", 57.0, 1},
	{"fg 6x@45 frag drift", 57.0, 1},
	{"fg 6x@45 even", 57.0, 1},
	{"fg 6x@45 even drift", 57.0, 1},
	{"fg 8x@20 clean", 56.1, 2},
	{"fg 8x@20 clean drift", 51.7, 3},
	{"fg 8x@20 frag", 55.9, 2},
	{"fg 8x@20 frag drift", 55.9, 2},
	{"fg 8x@20 even", 57.0, 1},
	{"fg 8x@20 even drift", 57.0, 1},
	{"fg 8x@30 clean", 57.0, 2},
	{"fg 8x@30 clean drift", 56.1, 2},
	{"fg 8x@30 frag", 56.8, 2},
	{"fg 8x@30 frag drift", 56.7, 2},
	{"fg 8x@30 even", 57.0, 1},
	{"fg 8x@30 even drift", 57.0, 1},
	{"fg 8x@45 clean", 57.0, 1},
	{"fg 8x@45 clean drift", 57.0, 1},
	{"fg 8x@45 frag", 57.0, 1},
	{"fg 8x@45 frag drift", 56.9, 2},
	{"fg 8x@45 even", 57.0, 1},
	{"fg 8x@45 even drift", 57.0, 1},
	{"fg 10x@20 clean", 51.2, 2},
	{"fg 10x@20 clean drift", 51.9, 3},
	{"fg 10x@20 frag", 56.3, 2},
	{"fg 10x@20 frag drift", 55.9, 3},
	{"fg 10x@20 even", 57.0, 1},
	{"fg 10x@20 even drift", 57.0, 1},
	{"fg 10x@30 clean", 56.7, 2},
	{"fg 10x@30 clean drift", 56.7, 2},
	{"fg 10x@30 frag", 56.9, 2},
	{"fg 10x@30 frag drift", 56.9, 2},
	{"fg 10x@30 even", 57.0, 1},
	{"fg 10x@30 even drift", 57.0, 1},
	{"fg 10x@45 clean", 57.0, 1},
	{"fg 10x@45 clean drift", 57.0, 1},
	{"fg 10x@45 frag", 57.0, 1},
	{"fg 10x@45 frag drift", 57.0, 1},
	{"fg 10x@45 even", 57.0, 1},
	{"fg 10x@45 even drift", 57.0, 1},
	{"fg 4x@30 rand", 57.0, 1},
	{"fg 4x@30 3p1", 56.9, 2},
	{"singles 60", 57.0, 1},
	{"singles 60 jit1ms", 57.0, 1},
	{"singles 144", 57.0, 1},
	{"singles 240", 57.0, 1},
	{"singles vrr 42-100", 53.9, 2},
	{"singles vrr 70-110", 57.0, 2},
	{"hitch 60 +80ms", 56.4, 5},
	{"hitch 144 +50ms", 56.7, 3},
	{"hitch 60 +95ms", 56.3, 6},
	{"hitch 60 60ms/2s", 56.0, 4},
	{"hitch 60 40ms/0.5s", 54.5, 3},
	{"trans singles 30->120", 46.9, 2},
	{"trans singles 60->240", 57.1, 1},
	{"trans singles 60->120", 57.1, 1},
	{"trans singles ~10->120", 40.3, 6},
	{"trans singles 30->60", 46.4, 2},
	{"trans singles 120->60", 57.0, 1},
	{"trans singles 60->30", 38.3, 2},
	{"trans singles 240->60", 57.0, 1},
	{"trans 4x@30 -> 4x@45 frag", 56.8, 2},
	{"trans 4x@45 -> 4x@30 frag", 56.8, 2},
	{"trans 4x@40 -> 4x@60 frag", 57.0, 1},
	{"trans 4x@60 -> 4x@30 clean", 56.6, 2},
	{"trans 60 -> 2x@60 clean (FG on)", 57.0, 1},
	{"trans 2x@60 clean -> 60 (FG off)", 56.8, 2},
	{"trans 60 -> 4x@30 frag (MFG on)", 56.9, 2},
	{"trans 4x@30 frag -> 30 (MFG off)", 40.9, 3},
	{"trans 30 -> 60 even", 44.0, 2},
	{"trans 2x@30 clean -> 4x@30 clean", 56.8, 2},
	{"trans 4x@30 clean -> 2x@30 clean", 55.9, 2},
	{"trans 10x@20 clean -> 2x@45 clean", 56.6, 2},
	{"stall 300ms after 4x lead, burst", 52.6, 17},
	{"stall 150ms after 6x lead, burst", 53.1, 7},
	{"stall 500ms singles 60", 50.1, 30},
	{"restart hook @2s 4x@30 clean", 56.2, 3},
	{"restart hook @2s 6x@20 frag", 55.5, 2},
};

static const RingFloor *FindRingFloor(const char *name)
{
	for (const RingFloor &floor : kRingFloors) {
		if (strcmp(floor.name, name) == 0) {
			return &floor;
		}
	}
	print_error("no floor for '%s'\n", name);
	fail();
	return nullptr;
}

static void Expect(bool ok, const char *name, const char *what, double got, double limit)
{
	if (!ok) {
		print_error("'%s': %s %.3f against %.3f\n", name, what, got, limit);
	}
	assert_true(ok);
}

// Runs a trace through the ring and holds it to its floors.
static RingRun CheckRing(const char *name, const PresentTrace *tr)
{
	const RingFloor *floor = FindRingFloor(name);
	const RingRun run = RunRingPhases(tr);
	Expect(run.distinct_per_s >= floor->distinct_per_s, name, "distinct/s", run.distinct_per_s,
	       floor->distinct_per_s);
	Expect(run.max_gap_ns <= floor->max_gap_ticks * kCanvasNs + 1, name, "max gap ms", (double)run.max_gap_ns / 1e6,
	       (double)(floor->max_gap_ticks * kCanvasNs) / 1e6);
	return run;
}

static PresentTrace g_trace;
static PresentTrace g_part;

// 2x to 10x frame generation over 20, 30 and 45 fps games, clustered cleanly,
// fragmented or evenly paced, with and without a slow swing in frame time. A
// burst cut at 4 Presents split every clean frame above 4x in two.
static void test_fgc_ring_frame_gen_matrix(void **)
{
	struct Base {
		uint64_t ns;
		const char *name;
	};
	struct Kind {
		Fg kind;
		const char *name;
	};
	char name[64];
	for (size_t factor : {2, 3, 4, 5, 6, 8, 10}) {
		for (Base base : {Base{kBase20, "20"}, Base{kBase30, "30"}, Base{kBase45, "45"}}) {
			for (Kind kind : {Kind{Fg::Clean, "clean"}, Kind{Fg::Frag, "frag"}, Kind{Fg::Even, "even"}}) {
				for (double drift : {0.0, 0.3}) {
					snprintf(name, sizeof(name), "fg %zux@%s %s%s", factor, base.name, kind.name,
						 drift != 0.0 ? " drift" : "");
					for (uint64_t seed : {1, 2}) {
						Clear(&g_trace);
						FgTrace(&g_trace, kind.kind, factor, base.ns, seed, 6 * kSec, drift);
						CheckRing(name, &g_trace);
					}
				}
			}
		}
	}
}

// The 4x shapes seen live, where one frame's Presents arrive in uneven clusters.
static void test_fgc_ring_fragmented_4x(void **)
{
	for (uint64_t seed : {1, 2}) {
		Clear(&g_trace);
		FgTrace(&g_trace, Fg::Rand, 4, kBase30, seed, 6 * kSec);
		CheckRing("fg 4x@30 rand", &g_trace);
		Clear(&g_trace);
		FgTrace(&g_trace, Fg::Split3Plus1, 4, kBase30, seed, 6 * kSec);
		CheckRing("fg 4x@30 3p1", &g_trace);
	}
}

// Without frame generation, at fixed and variable refresh.
static void test_fgc_ring_single_presents(void **)
{
	Clear(&g_trace);
	Singles(&g_trace, k60Hz, 600);
	CheckRing("singles 60", &g_trace);
	Clear(&g_trace);
	Singles(&g_trace, k60Hz, 600, kMs);
	CheckRing("singles 60 jit1ms", &g_trace);
	Clear(&g_trace);
	Singles(&g_trace, 6944444, 1400);
	CheckRing("singles 144", &g_trace);
	Clear(&g_trace);
	Singles(&g_trace, 4166667, 2400);
	CheckRing("singles 240", &g_trace);
	Clear(&g_trace);
	Vrr(&g_trace, 10 * kMs, 24 * kMs, 900, 9);
	CheckRing("singles vrr 42-100", &g_trace);
	Clear(&g_trace);
	Vrr(&g_trace, 9 * kMs, 14 * kMs, 900, 5);
	CheckRing("singles vrr 70-110", &g_trace);
}

// A hitch is one long interval in an otherwise steady game. Its lead must clear
// again, and no frame may wait longer than under the burst stamping, whose worst
// latency on the same traces is the bound.
static void test_fgc_ring_hitches(void **)
{
	struct Hitch {
		const char *name;
		uint64_t interval;
		size_t before;
		uint64_t hitch;
		size_t after;
		uint64_t burst_latency_ns;
	};
	for (const Hitch &h : {Hitch{"hitch 60 +80ms", k60Hz, 100, 80 * kMs, 300, 30166667},
			       Hitch{"hitch 144 +50ms", 6944444, 300, 50 * kMs, 600, 30166842}}) {
		Clear(&g_trace);
		Seq(&g_trace, {{h.interval, h.before}, {h.hitch, 1}, {h.interval, h.after}});
		const RingRun run = CheckRing(h.name, &g_trace);
		Expect(run.last_lead_ns == 0, h.name, "last lead ms", (double)run.last_lead_ns / 1e6, 0.0);
		Expect(run.max_latency_ns <= h.burst_latency_ns, h.name, "max latency ms",
		       (double)run.max_latency_ns / 1e6, (double)h.burst_latency_ns / 1e6);
	}

	Clear(&g_trace);
	Seq(&g_trace, {{k60Hz, 100}, {95 * kMs, 1}, {k60Hz, 300}});
	CheckRing("hitch 60 +95ms", &g_trace);
	struct Repeated {
		const char *name;
		size_t steady;
		uint64_t hitch;
		size_t times;
	};
	for (const Repeated &r :
	     {Repeated{"hitch 60 60ms/2s", 120, 60 * kMs, 5}, Repeated{"hitch 60 40ms/0.5s", 30, 40 * kMs, 10}}) {
		Pace parts[20];
		for (size_t i = 0; i < r.times; i++) {
			parts[2 * i] = {k60Hz, r.steady};
			parts[2 * i + 1] = {r.hitch, 1};
		}
		Clear(&g_trace);
		Seq(&g_trace, parts, 2 * r.times);
		CheckRing(r.name, &g_trace);
	}
}

// Three canvas ticks, 50 ms on a 60 fps canvas.
constexpr uint64_t kMaxWaitAfterRiseNs = 3 * kCanvasNs;

static void ExpectNoFreezeAfterRise(const char *name, const RingRun &run)
{
	Expect(run.max_gap_after_split_ns <= kMaxWaitAfterRiseNs, name, "max wait after the change ms",
	       (double)run.max_gap_after_split_ns / 1e6, (double)kMaxWaitAfterRiseNs / 1e6);
}

// A game whose Present rate changes. The step measured before a rise leads
// every stamp further ahead until it is re-measured; until then the canvas
// must keep getting new frames, so after a rise no wait may pass three ticks.
static void test_fgc_ring_transitions(void **)
{
	struct Change {
		const char *name;
		uint64_t from;
		uint64_t to;
		bool rise;
	};
	for (const Change &c : {Change{"trans singles 30->120", kBase30, 8333333, true},
				Change{"trans singles 60->240", k60Hz, 4166667, true},
				Change{"trans singles 60->120", k60Hz, 8333333, true},
				Change{"trans singles ~10->120", 100 * kMs - 1, 8333333, true},
				Change{"trans singles 30->60", kBase30, k60Hz, true},
				Change{"trans singles 120->60", 8333333, k60Hz, false},
				Change{"trans singles 60->30", k60Hz, kBase30, false},
				Change{"trans singles 240->60", 4166667, k60Hz, false}}) {
		const size_t before = (size_t)(1500000000ull / c.from);
		Clear(&g_trace);
		Seq(&g_trace, {{c.from, before}, {c.to, (size_t)(2 * kSec / c.to)}});
		g_trace.split = g_trace.t[before];
		const RingRun run = CheckRing(c.name, &g_trace);
		if (c.rise) {
			ExpectNoFreezeAfterRise(c.name, run);
		}
	}

	struct FgChange {
		const char *name;
		Fg from_kind;
		size_t from_factor;
		uint64_t from_base;
		Fg to_kind;
		size_t to_factor;
		uint64_t to_base;
		bool rise;
	};
	for (const FgChange &c : {
		     FgChange{"trans 4x@30 -> 4x@45 frag", Fg::Frag, 4, kBase30, Fg::Frag, 4, kBase45, true},
		     FgChange{"trans 4x@45 -> 4x@30 frag", Fg::Frag, 4, kBase45, Fg::Frag, 4, kBase30, false},
		     FgChange{"trans 4x@40 -> 4x@60 frag", Fg::Frag, 4, 25000000, Fg::Frag, 4, k60Hz, true},
		     FgChange{"trans 4x@60 -> 4x@30 clean", Fg::Clean, 4, k60Hz, Fg::Clean, 4, kBase30, false},
		     FgChange{"trans 60 -> 2x@60 clean (FG on)", Fg::Even, 1, k60Hz, Fg::Clean, 2, k60Hz, true},
		     FgChange{"trans 2x@60 clean -> 60 (FG off)", Fg::Clean, 2, k60Hz, Fg::Even, 1, k60Hz, false},
		     FgChange{"trans 60 -> 4x@30 frag (MFG on)", Fg::Even, 1, k60Hz, Fg::Frag, 4, kBase30, true},
		     FgChange{"trans 4x@30 frag -> 30 (MFG off)", Fg::Frag, 4, kBase30, Fg::Even, 1, kBase30, false},
		     FgChange{"trans 30 -> 60 even", Fg::Even, 1, kBase30, Fg::Even, 1, k60Hz, true},
		     FgChange{"trans 2x@30 clean -> 4x@30 clean", Fg::Clean, 2, kBase30, Fg::Clean, 4, kBase30, true},
		     FgChange{"trans 4x@30 clean -> 2x@30 clean", Fg::Clean, 4, kBase30, Fg::Clean, 2, kBase30, false},
		     FgChange{"trans 10x@20 clean -> 2x@45 clean", Fg::Clean, 10, kBase20, Fg::Clean, 2, kBase45,
			      false},
	     }) {
		Clear(&g_trace);
		FgTrace(&g_trace, c.from_kind, c.from_factor, c.from_base, 1);
		Cut(&g_trace, 2 * kSec);
		Clear(&g_part);
		FgTrace(&g_part, c.to_kind, c.to_factor, c.to_base, 2, 2 * kSec);
		Cat(&g_trace, &g_part);
		const RingRun run = CheckRing(c.name, &g_trace);
		if (c.rise) {
			ExpectNoFreezeAfterRise(c.name, run);
		}
	}
}

// A stall after the stamps have built up a lead, then a burst: the stall resets
// the window, and stamps keep rising through it.
static void test_fgc_ring_stalls(void **)
{
	struct Stall {
		const char *name;
		size_t factor;
		uint64_t base;
		uint64_t stall;
		size_t burst;
	};
	for (const Stall &s : {Stall{"stall 300ms after 4x lead, burst", 4, kBase30, 300 * kMs, 6},
			       Stall{"stall 150ms after 6x lead, burst", 6, kBase20, 150 * kMs, 4}}) {
		Clear(&g_trace);
		FgTrace(&g_trace, Fg::Clean, s.factor, s.base, 3);
		Cut(&g_trace, 2 * kSec);
		const uint64_t t_end = g_trace.t[g_trace.count - 1];
		for (size_t i = 0; i < s.burst; i++) {
			Push(&g_trace, t_end + s.stall + i * 100000);
		}
		FgTrace(&g_trace, Fg::Clean, s.factor, s.base, 4, 2 * kSec, 0.0, t_end + s.stall + 10 * kMs);
		CheckRing(s.name, &g_trace);
	}

	Clear(&g_trace);
	Seq(&g_trace, {{k60Hz, 200}});
	Cut(&g_trace, 2 * kSec);
	Seq(&g_trace, {{k60Hz, 120}}, g_trace.t[g_trace.count - 1] + 500 * kMs);
	CheckRing("stall 500ms singles 60", &g_trace);
}

// The hook restarting mid-stream starts a fresh pacer, limiter and ring.
static void test_fgc_ring_hook_restart(void **)
{
	Clear(&g_trace);
	FgTrace(&g_trace, Fg::Clean, 4, kBase30, 5, 4 * kSec);
	g_trace.restart_at = g_trace.t[0] + 2 * kSec;
	CheckRing("restart hook @2s 4x@30 clean", &g_trace);
	Clear(&g_trace);
	FgTrace(&g_trace, Fg::Frag, 6, kBase20, 5, 4 * kSec);
	g_trace.restart_at = g_trace.t[0] + 2 * kSec;
	CheckRing("restart hook @2s 6x@20 frag", &g_trace);
}

// A fresh pacer stamps its first Presents at their own times and starts pacing
// once the window holds FGC_PACER_MIN_INTERVALS.
static void test_fgc_pacer_fresh(void **)
{
	struct fgc_pacer pacer;
	fgc_pacer_init(&pacer, fgc_pacer_lead_cap(kCanvasNs, kRingSlots));
	uint64_t t = 50 * kMs;
	for (uint32_t i = 0; i < FGC_PACER_MIN_INTERVALS; i++) {
		const struct fgc_pace pace = fgc_pacer_stamp(&pacer, t);
		assert_int_equal(pace.stamp, t);
		assert_false(pace.reset);
		assert_int_equal(fgc_pacer_step(&pacer), 0);
		t += 8 * kMs;
	}
	assert_int_equal(fgc_pacer_stamp(&pacer, t).stamp, t);
	assert_int_equal(fgc_pacer_step(&pacer), 8 * kMs);
}

// The cap leaves the ring the slots its guard keeps, and never drops below a
// 30 fps frame.
static void test_fgc_pacer_lead_cap(void **)
{
	assert_int_equal(fgc_pacer_lead_cap(kCanvasNs, 8), 33333334);
	assert_int_equal(fgc_pacer_lead_cap(kCanvasNs * 2, 8), 66666668);
	assert_int_equal(fgc_pacer_lead_cap(kCanvasNs / 2, 8), FGC_PACER_MIN_LEAD_CAP_NS);
}

// A stall empties the window: the next stamp is the Present's own time and the
// step stays 0 until the window holds enough intervals again.
static void test_fgc_pacer_stall_resets(void **)
{
	struct fgc_pacer pacer;
	fgc_pacer_init(&pacer, fgc_pacer_lead_cap(kCanvasNs, kRingSlots));
	uint64_t t = 10 * kMs;
	for (int i = 0; i < 100; i++) {
		assert_false(fgc_pacer_stamp(&pacer, t).reset);
		t += 8333333;
	}
	assert_true(fgc_pacer_step(&pacer) != 0);
	t += 200 * kMs;
	const struct fgc_pace after = fgc_pacer_stamp(&pacer, t);
	assert_true(after.reset);
	assert_int_equal(after.stamp, t);
	assert_int_equal(fgc_pacer_step(&pacer), 0);
	for (uint32_t i = 0; i < FGC_PACER_MIN_INTERVALS; i++) {
		t += 8333333;
		assert_false(fgc_pacer_stamp(&pacer, t).reset);
	}
	assert_true(fgc_pacer_step(&pacer) != 0);
}

// A lead that does not clear within FGC_PACER_CLEAR_NS resyncs: the window is
// cut back to the Presents since it last cleared, stamps hold just past the last
// one, and the first Present past it is stamped at its own time.
static void test_fgc_pacer_resync(void **)
{
	struct fgc_pacer pacer;
	fgc_pacer_init(&pacer, fgc_pacer_lead_cap(kCanvasNs, kRingSlots));
	uint64_t t = 10 * kMs;
	for (int i = 0; i < 60; i++) {
		fgc_pacer_stamp(&pacer, t);
		t += kBase30;
	}
	bool resynced = false;
	uint64_t prev = 0;
	for (int i = 0; i < 60 && !resynced; i++) {
		const struct fgc_pace pace = fgc_pacer_stamp(&pacer, t);
		resynced = pace.resync;
		prev = pace.stamp;
		t += 8333333;
	}
	assert_true(resynced);
	assert_true(prev > t - 8333333);
	assert_true(pacer.count < FGC_PACER_INTERVALS + 1);
	bool snapped = false;
	for (int i = 0; i < 10 && !snapped; i++) {
		const struct fgc_pace pace = fgc_pacer_stamp(&pacer, t);
		assert_true(pace.stamp > prev);
		snapped = pace.stamp == t;
		prev = pace.stamp;
		t += 8333333;
	}
	assert_true(snapped);
}

// A clock that steps back reads as a zero interval: the stamp still rises.
static void test_fgc_pacer_clock_step_back(void **)
{
	struct fgc_pacer pacer;
	fgc_pacer_init(&pacer, fgc_pacer_lead_cap(kCanvasNs, kRingSlots));
	const uint64_t first = fgc_pacer_stamp(&pacer, 50 * kMs).stamp;
	const struct fgc_pace back = fgc_pacer_stamp(&pacer, 40 * kMs);
	assert_false(back.reset);
	assert_int_equal(back.stamp, first + 1);
	assert_int_equal(fgc_pacer_stamp(&pacer, 60 * kMs).stamp, 60 * kMs);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_lag_is_not_async_below),
		cmocka_unit_test(test_display_capture_is_never_below),
		cmocka_unit_test(test_display_rate_is_capped_at_expected),
		cmocka_unit_test(test_half_cadence_locks_after_grace),
		cmocka_unit_test(test_zero_rate_never_locks),
		cmocka_unit_test(test_mostly_hidden_second_is_skipped),
		cmocka_unit_test(test_lock_exits_after_five_seconds_outside_band),
		cmocka_unit_test(test_async_below),
		cmocka_unit_test(test_ref_from_live_canvases_only),
		cmocka_unit_test(test_new_identity_rebaselines),
		cmocka_unit_test(test_uint32_wrap_delta),
		cmocka_unit_test(test_kind_change_rebaselines),
		cmocka_unit_test(test_unmeasurable_and_idle),
		cmocka_unit_test(test_listing_and_frame_signal),
		cmocka_unit_test(test_game_capture_reads_unmeasurable),
		cmocka_unit_test(test_unhooked_game_capture_reads_idle),
		cmocka_unit_test(test_game_hook_reports_presents_and_copies),
		cmocka_unit_test(test_game_hook_below),
		cmocka_unit_test(test_game_hook_rebaselines_on_rehook),
		cmocka_unit_test(test_game_hook_uint32_wrap),
		cmocka_unit_test(test_hook_relay_baselines_after_reset),
		cmocka_unit_test(test_hook_relay_plain_path),
		cmocka_unit_test(test_hook_relay_ring_path),
		cmocka_unit_test(test_hook_relay_uint32_wrap),
		cmocka_unit_test(test_display_capture_that_never_counted_is_named),
		cmocka_unit_test(test_display_and_game_capture_share_the_line),
		cmocka_unit_test(test_counted_under_heavy_lag_is_not_idle),
		cmocka_unit_test(test_restart_only_session_is_not_idle),
		cmocka_unit_test(test_session_summary_and_reset),
		cmocka_unit_test(test_removed_source_keeps_its_sums),
		cmocka_unit_test(test_repeated_session_begin_keeps_the_session),
		cmocka_unit_test(test_grace_restarts_on_going_live),
		cmocka_unit_test(test_lock_ends_with_the_broadcast),
		cmocka_unit_test(test_browser_paint_reports_a_rate_only),
		cmocka_unit_test(test_game_hook_window_lines),
		cmocka_unit_test(test_game_hook_session_quantiles),
		cmocka_unit_test(test_clear_drops_pending_window_lines),
		cmocka_unit_test(test_fgc_formatters_zero_window),
		cmocka_unit_test(test_fgc_hook_stats_line),
		cmocka_unit_test(test_fgc_ring_stats_line),
		cmocka_unit_test(test_fgc_stats_window),
		cmocka_unit_test(test_fgc_pacer_fresh),
		cmocka_unit_test(test_fgc_pacer_lead_cap),
		cmocka_unit_test(test_fgc_pacer_stall_resets),
		cmocka_unit_test(test_fgc_pacer_resync),
		cmocka_unit_test(test_fgc_pacer_clock_step_back),
		cmocka_unit_test(test_fgc_ring_frame_gen_matrix),
		cmocka_unit_test(test_fgc_ring_fragmented_4x),
		cmocka_unit_test(test_fgc_ring_single_presents),
		cmocka_unit_test(test_fgc_ring_hitches),
		cmocka_unit_test(test_fgc_ring_transitions),
		cmocka_unit_test(test_fgc_ring_stalls),
		cmocka_unit_test(test_fgc_ring_hook_restart),
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
