#include "diag/capture_rate.hpp"

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

	// Game capture's hook has no counter until Phase 4: listed, and once hooked
	// (it has a size) never measured. Unhooked, it has nothing to count.
	SourceTraits game = traits(kGameCaptureId);
	assert_true(IsListed(game));
	assert_true(HasFrameSignal(game));
	game.hasSize = true;
	assert_false(HasFrameSignal(game));

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

// A Game Capture feed as the sampler builds it for a source reporting no kind.
static Feed GameCaptureFeed(bool hooked)
{
	SourceTraits traits;
	traits.id = kGameCaptureId;
	traits.hasSize = hooked;
	Feed game("Game Capture", Kind::None, 30.0, true);
	game.src.frameSignal = HasFrameSignal(traits);
	return game;
}

// A broadcast that captures only through a hooked Game Capture, on a 30 fps
// canvas (a vertical Shorts destination): the source is named as unmeasurable,
// never warns (C2), and the session line no longer claims there were no capture
// sources.
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
		assert_false(r->below);
		assert_false(r->rate.has_value());
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
	const std::string line = t.SessionEnd(2ull * 1000000000ull);
	assert_non_null(strstr(line.c_str(), "'Game Capture' idle (never counted)"));
	assert_null(strstr(line.c_str(), "no capture sources"));
}

// Once the hook reports its own kind (Phase 4) the game capture keeps its row:
// unmeasurable until something measures that kind, and never read as a display
// capture in the session line.
static void test_game_hook_kind_keeps_its_row(void **)
{
	Tracker t;
	t.SessionBegin(0);
	Feed game("Game Capture", Kind::GameHook, 30.0, true);
	Prime(t, game);
	for (int s = 0; s < 5; s++) {
		Step(t, game, 60, 60, 60);
		const Row *r = RowFor(t, "Game Capture");
		assert_non_null(r);
		assert_int_equal((int)r->status, (int)Status::Unmeasurable);
		assert_false(r->rate.has_value());
		assert_false(r->below);
	}
	const std::string line = t.SessionEnd(5ull * 1000000000ull);
	assert_non_null(strstr(line.c_str(), "'Game Capture' unmeasurable"));
	assert_null(strstr(line.c_str(), "median"));
	assert_null(strstr(line.c_str(), "no capture sources"));
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
		cmocka_unit_test(test_game_hook_kind_keeps_its_row),
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
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
