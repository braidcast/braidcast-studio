#include "diag/capture_rate.hpp"
#include "hook-frame-relay.h"
#include "frame-gen-stats.h"

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
	s.step_resets = 1;
	s.stamp_clamps = 2;
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
	assert_string_equal(line,
			    "[10s] hook 10.0 s: presents 119.8/s copies 115.0/s, bursts 1:3 2:0 3:1 4:295 >4:2"
			    ", step 8333 us, step resets 1, stamp clamps 2, bucket skips 48, slot busy 0"
			    ", copy fails 0, burst span max 620 us, burst gap min 24100 us, copy p50 225 p99 925 us");

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
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
