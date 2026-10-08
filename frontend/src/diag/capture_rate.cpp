#include "capture_rate.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <set>

namespace CaptureRate {

namespace {

struct Fraction {
	double value;
	const char *label;
};

// The cadences a display capture settles into when the screen updates at a
// fraction of the canvas rate (a 30 fps game on a 60 fps canvas, a 40 Hz panel).
constexpr Fraction kFractions[] = {
	{1.0 / 2.0, "1/2"}, {1.0 / 3.0, "1/3"}, {2.0 / 3.0, "2/3"}, {1.0 / 4.0, "1/4"}, {3.0 / 4.0, "3/4"},
};
constexpr double kFractionBand = 0.04;

// A lock is a sustained pattern, never a single second: enter after this many
// eligible seconds in one band, leave after this many outside it. Seconds right
// after a (re)baseline are warm-up and do not count.
constexpr double kGraceSec = 5.0;
constexpr int kEnterSeconds = 10;
constexpr int kExitSeconds = 5;

// A second is judged only when the source was live for most of it and delivered
// something meaningful; an alt-tabbed or frozen second is skipped, not a reset.
constexpr double kMinLiveShare = 0.5;
constexpr double kMinLockRatio = 0.2;

// "Below" (async, game hook): the ticks that brought a new frame fell under
// this share of what the producer offered and the canvas could show.
constexpr double kBelowFactor = 0.9;

constexpr double kEpsilon = 1e-9;

int Bin(double rate)
{
	return static_cast<int>(std::lround(rate * 10.0));
}

// The rate at which a share q of the recorded seconds were at or below.
double Quantile(const std::map<int, double> &hist, double q)
{
	double total = 0.0;
	for (const auto &[bin, sec] : hist) {
		total += sec;
	}
	double seen = 0.0;
	for (const auto &[bin, sec] : hist) {
		seen += sec;
		if (seen >= total * q) {
			return bin / 10.0;
		}
	}
	return 0.0;
}

double Median(const std::map<int, double> &hist)
{
	return Quantile(hist, 0.5);
}

// The tag game capture's per-window lines carry too (FGC_STATS_TAG in
// plugins/win-capture/frame-gen-stats.h), so one grep lines all of them up.
constexpr const char *kWindowTag = "[10s]";

int MatchFraction(double fraction)
{
	for (int i = 0; i < static_cast<int>(std::size(kFractions)); i++) {
		if (std::fabs(fraction - kFractions[i].value) <= kFractionBand) {
			return i;
		}
	}
	return -1;
}

// Whether a capture type's frames reach the counters.
enum class Signal {
	Always,        // counting nothing means not capturing
	WgcMethodOnly, // BitBlt delivers frames no counter sees
	// A counting hook reports GAME_HOOK; hooked without it (a game still holding
	// an older hook) it has frames and no counter. Unhooked, it has no size and
	// nothing to count.
	NoneOnceSized,
};

struct CaptureType {
	const char *id;
	Signal signal;
	const char *unmeasurableNote; // why it reads unmeasurable, when more is known than "no counter"
};

// Capture types that may count nothing right now (a failed duplicator, a BitBlt
// window capture, a game capture), so they get a row without a kind.
constexpr CaptureType kCaptureTypes[] = {
	{kMonitorCaptureId, Signal::Always, nullptr},
	{kWindowCaptureId, Signal::WgcMethodOnly, nullptr},
	{kGameCaptureId, Signal::NoneOnceSized, "the game's capture hook may predate frame counting; restart the game"},
};

const CaptureType *CaptureTypeOf(const std::string &id)
{
	for (const CaptureType &type : kCaptureTypes) {
		if (id == type.id) {
			return &type;
		}
	}
	return nullptr;
}

bool IsDisplayRate(Kind kind)
{
	return kind == Kind::Wgc || kind == Kind::Dxgi;
}

// Kinds that deliver a frame only when something changed (C2), so their rate is
// a fact about the content, never a fault, and never reads "below".
bool IsChangeRate(Kind kind)
{
	return IsDisplayRate(kind) || kind == Kind::BrowserPaint;
}

// Kinds whose producer rate is known, so they can read "below": an async source
// (frames received) and the game hook (presents).
bool HasProducerRate(Kind kind)
{
	return kind == Kind::Async || kind == Kind::GameHook;
}

bool IsMeasured(Kind kind)
{
	return IsChangeRate(kind) || HasProducerRate(kind);
}

Status StatusOf(const SourceInput &src)
{
	if (!src.showing) {
		return Status::Idle;
	}
	if (src.counts.kind == Kind::None) {
		return src.frameSignal ? Status::Idle : Status::Unmeasurable;
	}
	return IsMeasured(src.counts.kind) ? Status::Ok : Status::Unmeasurable;
}

// What a row says of a source before any delta: who it is and whether it counts.
Row RowOf(const SourceInput &src, double mainFps)
{
	Row r;
	r.uuid = src.uuid;
	r.name = src.name;
	r.kind = src.counts.kind;
	r.refFps = RefFps(src.reach, mainFps);
	r.status = StatusOf(src);
	r.unmeasurableNote = r.status == Status::Unmeasurable ? src.unmeasurableNote : nullptr;
	return r;
}

// A counted kind as the session line names it.
const char *LineLabel(Kind kind)
{
	switch (kind) {
	case Kind::Dxgi:
		return "DXGI";
	case Kind::Wgc:
		return "WGC";
	case Kind::BrowserPaint:
		return "paint";
	case Kind::GameHook:
		return "game";
	default:
		return KindName(kind);
	}
}

std::string Format(const char *fmt, double a, double b = 0.0, double c = 0.0)
{
	char buf[128];
	snprintf(buf, sizeof(buf), fmt, a, b, c);
	return buf;
}

} // namespace

const char *KindName(Kind kind)
{
	switch (kind) {
	case Kind::Wgc:
		return "wgc";
	case Kind::Dxgi:
		return "dxgi";
	case Kind::Async:
		return "async";
	case Kind::BrowserPaint:
		return "browserPaint";
	case Kind::GameHook:
		return "gameHook";
	case Kind::None:
		break;
	}
	return "none";
}

const char *StatusName(Status status)
{
	switch (status) {
	case Status::Ok:
		return "ok";
	case Status::Unmeasurable:
		return "unmeasurable";
	case Status::Idle:
		break;
	}
	return "idle";
}

bool IsListed(const SourceTraits &traits)
{
	return traits.async || CaptureTypeOf(traits.id) != nullptr;
}

bool HasFrameSignal(const SourceTraits &traits)
{
	if (traits.async) {
		// Deinterlacing hides an async source's frames from the fold.
		return !traits.deinterlaced;
	}
	const CaptureType *type = CaptureTypeOf(traits.id);
	if (!type) {
		return true;
	}
	switch (type->signal) {
	case Signal::Always:
		return true;
	case Signal::WgcMethodOnly:
		// A WGC setting on a system without WGC runs BitBlt and reads idle.
		return traits.windowWgc;
	case Signal::NoneOnceSized:
		return !traits.hasSize;
	}
	return true;
}

const char *UnmeasurableNote(const SourceTraits &traits)
{
	const CaptureType *type = traits.async ? nullptr : CaptureTypeOf(traits.id);
	return type ? type->unmeasurableNote : nullptr;
}

std::optional<double> RefFps(const std::vector<Reach> &reach, double mainFps)
{
	std::optional<double> ref;
	for (const Reach &r : reach) {
		if (!r.live || r.canvasFps <= 0.0) {
			continue;
		}
		const double bounded = std::min(r.canvasFps, mainFps);
		ref = ref ? std::max(*ref, bounded) : bounded;
	}
	return ref;
}

void Tracker::RestartGrace(Entry &e)
{
	e.sinceBaselineSec = 0.0;
	ClearLock(e);
}

void Tracker::ClearLock(Entry &e)
{
	e.streakFraction = -1;
	e.streakCount = 0;
	e.lockedFraction = -1;
	e.exitCount = 0;
}

void Tracker::Rebaseline(Entry &e, const SourceInput &src, double mainFps)
{
	e.identity = src.identity;
	e.kind = src.counts.kind;
	e.last = src.counts;
	e.baselined = true;
	e.hadRef = RefFps(src.reach, mainFps).has_value();
	RestartGrace(e);
}

void Tracker::UpdateLock(Entry &e, std::optional<double> fraction, bool eligible)
{
	if (!eligible || !fraction) {
		return;
	}
	const int f = MatchFraction(*fraction);
	if (f >= 0 && f == e.streakFraction) {
		e.streakCount++;
	} else {
		e.streakFraction = f;
		e.streakCount = f >= 0 ? 1 : 0;
	}

	if (e.lockedFraction >= 0) {
		e.exitCount = f == e.lockedFraction ? 0 : e.exitCount + 1;
		if (e.exitCount >= kExitSeconds) {
			e.lockedFraction = -1;
			e.exitCount = 0;
		}
	}
	if (e.lockedFraction < 0 && e.streakFraction >= 0 && e.streakCount >= kEnterSeconds) {
		e.lockedFraction = e.streakFraction;
		e.exitCount = 0;
	}
}

// What the session line says of a source that never counted a live second.
void Tracker::NoteSession(Entry &e, const SourceInput &src, Status status)
{
	if (!inSession_) {
		return;
	}
	e.session.everShowing = e.session.everShowing || src.showing;
	e.session.everUnmeasurable = e.session.everUnmeasurable || status == Status::Unmeasurable;
	if (status == Status::Ok) {
		e.session.countedAs = src.counts.kind;
	}
}

Row Tracker::Evaluate(Entry &e, const SourceInput &src, double dt, double mainFps)
{
	Row r = RowOf(src, mainFps);

	if (r.refFps && !e.hadRef) {
		// Going live: grace runs from here, and a lock from watching before does
		// not carry over.
		RestartGrace(e);
	} else if (!r.refFps && e.hadRef) {
		// Going off air: no reference means no rule, so the broadcast's lock ends
		// with it rather than lingering on the rate.
		ClearLock(e);
	}
	e.hadRef = r.refFps.has_value();

	// uint32 modular deltas: the counters wrap, and only a new identity or kind
	// (handled by the caller) means they restarted.
	const uint32_t dLive = src.counts.liveTicks - e.last.liveTicks;
	const uint32_t dNew = src.counts.newFrameTicks - e.last.newFrameTicks;
	const uint32_t dDelivered = src.counts.framesDelivered - e.last.framesDelivered;
	const uint32_t dOffered = src.counts.framesOffered - e.last.framesOffered;
	e.last = src.counts;
	e.sinceBaselineSec += dt;

	const double loops = dt * mainFps;
	const bool mostlyLive = loops > 0.0 && dLive >= kMinLiveShare * loops;
	const double expected = dLive * (r.refFps ? std::min(1.0, *r.refFps / mainFps) : 1.0);
	const bool inGrace = e.sinceBaselineSec <= kGraceSec + kEpsilon;

	std::optional<double> sessionRate;
	std::optional<double> sessionInput;
	std::optional<double> sessionCopies;
	if (r.status == Status::Ok && IsChangeRate(r.kind)) {
		// Capped at what the canvas could show, one frame per tick: WGC can land two
		// frames in one pump, and a browser set to a custom rate above the canvas
		// paints frames the canvas never shows.
		const double shown = std::min(static_cast<double>(dDelivered), expected);
		r.rate = shown / dt;
		sessionRate = r.rate;
		// The lock note is for display capture only; an overlay reports its rate.
		if (IsDisplayRate(r.kind)) {
			if (r.refFps && expected > 0.0) {
				r.fraction = shown / expected;
			}
			r.inGrace = inGrace;
			const bool eligible = r.refFps && !inGrace && mostlyLive && r.fraction &&
					      *r.fraction >= kMinLockRatio;
			UpdateLock(e, r.fraction, eligible);
		}
	} else if (r.status == Status::Ok && HasProducerRate(r.kind)) {
		// What the producer offered (an async source's frames, a game's presents)
		// against the ticks that brought a frame the canvas had not shown, at most
		// one per tick. The hook's copies are not that measure: two copies landing
		// in one tick show as one frame, and a frame generation ring copies a burst
		// at once that the source draws out over the following ticks.
		const bool game = r.kind == Kind::GameHook;
		const uint32_t in = game ? dOffered : dDelivered;
		r.inputFps = in / dt;
		r.renderedFps = dNew / dt;
		if (game) {
			r.copiesFps = dDelivered / dt;
			sessionCopies = r.copiesFps;
		}
		if (r.refFps && dLive > 0) {
			r.below = dNew < kBelowFactor * std::min(static_cast<double>(in), expected);
		}
		sessionRate = r.renderedFps;
		sessionInput = r.inputFps;
		if (game && inSession_) {
			AddGameWindow(e, r, dt, dOffered, dDelivered, dNew);
		}
	}
	if (e.lockedFraction >= 0) {
		r.lockedFraction = kFractions[e.lockedFraction].label;
	}

	if (r.status == Status::Ok && mostlyLive) {
		e.window.liveSec += dt;
		e.window.lockedSec += e.lockedFraction >= 0 ? dt : 0.0;
		e.window.belowSec += r.below ? dt : 0.0;
		if (inSession_) {
			Session &s = e.session;
			s.measuredAs = r.kind;
			s.liveSec += dt;
			if (sessionRate) {
				s.rate[Bin(*sessionRate)] += dt;
			}
			if (sessionInput) {
				s.input[Bin(*sessionInput)] += dt;
			}
			if (sessionCopies) {
				s.copies[Bin(*sessionCopies)] += dt;
			}
			if (r.refFps) {
				s.ref[static_cast<int>(std::lround(*r.refFps))] += dt;
			}
			if (e.lockedFraction >= 0) {
				s.lockedSec[e.lockedFraction] += dt;
			}
			s.belowSec += r.below ? dt : 0.0;
		}
	}
	NoteSession(e, src, r.status);
	r.sinceReset = e.window;
	return r;
}

void Tracker::Sample(const SampleInput &in)
{
	rows_.clear();
	const bool gapOk = in.dtSec > 0.0 && in.dtSec <= kMaxSampleGapSec;
	std::set<std::string> seen;
	for (const SourceInput &src : in.sources) {
		if (!seen.insert(src.uuid).second) {
			continue;
		}
		Entry &e = entries_[src.uuid];
		e.name = src.name;
		const bool restart = !e.baselined || !gapOk || e.identity != src.identity || e.kind != src.counts.kind;
		if (restart) {
			// A new window: no delta across it, and the grace period runs again.
			Rebaseline(e, src, in.mainFps);
			Row r = RowOf(src, in.mainFps);
			r.inGrace = IsDisplayRate(r.kind);
			r.sinceReset = e.window;
			NoteSession(e, src, r.status);
			rows_.push_back(std::move(r));
			continue;
		}
		rows_.push_back(Evaluate(e, src, in.dtSec, in.mainFps));
	}

	// A source that left keeps its entry while a session still needs its sums.
	for (auto it = entries_.begin(); it != entries_.end();) {
		if (!inSession_ && !seen.count(it->first)) {
			it = entries_.erase(it);
		} else {
			if (!seen.count(it->first)) {
				it->second.baselined = false;
			}
			++it;
		}
	}
}

void Tracker::AddGameWindow(Entry &e, const Row &r, double dt, uint32_t presents, uint32_t copies, uint32_t newFrames)
{
	GameWindow &w = e.gameWindow;
	w.sec += dt;
	w.presents += presents;
	w.copies += copies;
	w.newFrames += newFrames;
	w.belowSec += r.below ? dt : 0.0;
	if (w.sec < kWindowSec - kEpsilon) {
		return;
	}
	windowLines_.push_back(
		std::string("[capture-rate] ") + kWindowTag + " '" + e.name + "' game" +
		Format(" %.1f s: presents %.1f", w.sec, w.presents / w.sec) +
		Format(" copies %.1f new %.1f /s, below %.1f s", w.copies / w.sec, w.newFrames / w.sec, w.belowSec));
	w = GameWindow{};
}

std::vector<std::string> Tracker::TakeWindowLines()
{
	std::vector<std::string> lines;
	lines.swap(windowLines_);
	return lines;
}

void Tracker::Pause()
{
	rows_.clear();
	for (auto &[uuid, e] : entries_) {
		e.baselined = false;
	}
}

void Tracker::ResetWindows()
{
	for (auto &[uuid, e] : entries_) {
		e.window = {};
	}
	for (Row &r : rows_) {
		r.sinceReset = {};
	}
}

void Tracker::SessionBegin(uint64_t nowNs)
{
	if (inSession_) {
		return;
	}
	inSession_ = true;
	sessionStartNs_ = nowNs;
	for (auto &[uuid, e] : entries_) {
		e.session = Session{};
		e.gameWindow = GameWindow{};
	}
}

// The reference the session was mostly judged against: " (ref 60)", or " (no ref)".
std::string Tracker::RefNote(const Session &s)
{
	if (s.ref.empty()) {
		return " (no ref)";
	}
	const auto mode = std::max_element(s.ref.begin(), s.ref.end(),
					   [](const auto &a, const auto &b) { return a.second < b.second; });
	return Format(" (ref %.0f)", static_cast<double>(mode->first));
}

std::string Tracker::Summarize(const std::string &name, const Entry &e) const
{
	const Session &s = e.session;
	const std::string quoted = "'" + name + "'";
	if (s.liveSec <= 0.0) {
		if (s.everUnmeasurable) {
			return quoted + " unmeasurable";
		}
		// It counted, but render lag or a session under a second left no second
		// that could be judged.
		if (s.countedAs != Kind::None) {
			return quoted + " " + LineLabel(s.countedAs) + " counted, no second measured";
		}
		// A showing capture that never counted is the finding (a stale plugin, a
		// failed duplicator), not an absence.
		return s.everShowing ? quoted + " idle (never counted)" : std::string();
	}
	const double belowPct = s.belowSec / s.liveSec * 100.0;
	if (s.measuredAs == Kind::Async) {
		return quoted +
		       Format(" async in %.1f out %.1f, below %.1f%%", Median(s.input), Median(s.rate), belowPct);
	}
	if (s.measuredAs == Kind::GameHook) {
		return quoted + " " + LineLabel(s.measuredAs) +
		       Format(" presents %.1f copies %.1f new %.1f", Median(s.input), Median(s.copies),
			      Median(s.rate)) +
		       RefNote(s) + Format(", below %.1f%%", belowPct) +
		       Format(", presents p10 %.1f p25 %.1f", Quantile(s.input, 0.1), Quantile(s.input, 0.25)) +
		       Format(", new p10 %.1f p25 %.1f", Quantile(s.rate, 0.1), Quantile(s.rate, 0.25));
	}

	std::string out =
		quoted + " " + LineLabel(s.measuredAs) + Format(" median %.1f/s", Median(s.rate)) + RefNote(s);
	if (!s.lockedSec.empty()) {
		const auto most = std::max_element(s.lockedSec.begin(), s.lockedSec.end(),
						   [](const auto &a, const auto &b) { return a.second < b.second; });
		out += std::string(", locked ") + kFractions[most->first].label +
		       Format(" %.0f%%", most->second / s.liveSec * 100.0);
	}
	return out;
}

std::string Tracker::SessionEnd(uint64_t nowNs)
{
	if (!inSession_) {
		return std::string();
	}
	inSession_ = false;

	std::vector<std::pair<std::string, const Entry *>> byName;
	for (const auto &[uuid, e] : entries_) {
		byName.emplace_back(e.name, &e);
	}
	std::sort(byName.begin(), byName.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

	std::string parts;
	for (const auto &[name, e] : byName) {
		const std::string part = Summarize(name, *e);
		if (!part.empty()) {
			parts += (parts.empty() ? "" : " | ") + part;
		}
	}

	const double seconds = nowNs > sessionStartNs_ ? (nowNs - sessionStartNs_) / 1e9 : 0.0;
	std::string line = Format("[capture-rate] session %.0f s: ", seconds);
	line += parts.empty() ? "no capture sources" : parts;

	for (auto it = entries_.begin(); it != entries_.end();) {
		it->second.session = Session{};
		it->second.gameWindow = GameWindow{};
		// Entries kept only for their sums are done with once the line is written.
		it = it->second.baselined ? std::next(it) : entries_.erase(it);
	}
	return line;
}

void Tracker::Clear()
{
	entries_.clear();
	rows_.clear();
	windowLines_.clear();
	inSession_ = false;
	sessionStartNs_ = 0;
}

} // namespace CaptureRate
