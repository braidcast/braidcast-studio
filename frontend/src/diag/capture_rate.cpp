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

// Async "below": rendered fell under this share of what it could have rendered.
constexpr double kBelowFactor = 0.9;

constexpr double kEpsilon = 1e-9;

int Bin(double rate)
{
	return static_cast<int>(std::lround(rate * 10.0));
}

// The rate at which half of the recorded seconds were at or below.
double Median(const std::map<int, double> &hist)
{
	double total = 0.0;
	for (const auto &[bin, sec] : hist) {
		total += sec;
	}
	double seen = 0.0;
	for (const auto &[bin, sec] : hist) {
		seen += sec;
		if (seen >= total / 2.0) {
			return bin / 10.0;
		}
	}
	return 0.0;
}

int MatchFraction(double fraction)
{
	for (int i = 0; i < static_cast<int>(std::size(kFractions)); i++) {
		if (std::fabs(fraction - kFractions[i].value) <= kFractionBand) {
			return i;
		}
	}
	return -1;
}

bool IsDisplayRate(Kind kind)
{
	return kind == Kind::Wgc || kind == Kind::Dxgi;
}

// Phase 1 measures display capture and async sources. Overlay paint counts go to
// diagnostics only, and the game-capture hook has no counter yet.
bool IsReported(Kind kind)
{
	return kind != Kind::BrowserPaint && kind != Kind::GameHook;
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

void Tracker::Rebaseline(Entry &e, const SourceInput &src)
{
	e.identity = src.identity;
	e.kind = src.counts.kind;
	e.last = src.counts;
	e.baselined = true;
	e.sinceBaselineSec = 0.0;
	e.streakFraction = -1;
	e.streakCount = 0;
	e.lockedFraction = -1;
	e.exitCount = 0;
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

Row Tracker::Evaluate(Entry &e, const SourceInput &src, double dt, double mainFps)
{
	Row r;
	r.uuid = src.uuid;
	r.name = src.name;
	r.kind = src.counts.kind;
	r.refFps = RefFps(src.reach, mainFps);
	r.status = !src.showing ? Status::Idle : (r.kind == Kind::None ? Status::Unmeasurable : Status::Ok);

	// uint32 modular deltas: the counters wrap, and only a new identity or kind
	// (handled by the caller) means they restarted.
	const uint32_t dLive = src.counts.liveTicks - e.last.liveTicks;
	const uint32_t dNew = src.counts.newFrameTicks - e.last.newFrameTicks;
	const uint32_t dDelivered = src.counts.framesDelivered - e.last.framesDelivered;
	e.last = src.counts;
	e.sinceBaselineSec += dt;

	const double loops = dt * mainFps;
	const bool mostlyLive = loops > 0.0 && dLive >= kMinLiveShare * loops;
	const double expected = dLive * (r.refFps ? std::min(1.0, *r.refFps / mainFps) : 1.0);
	const bool inGrace = e.sinceBaselineSec <= kGraceSec + kEpsilon;

	std::optional<double> sessionRate;
	std::optional<double> sessionInput;
	if (r.status == Status::Ok && IsDisplayRate(r.kind)) {
		// Capped: WGC can land two frames in one pump, and the canvas shows one.
		const double shown = std::min(static_cast<double>(dDelivered), expected);
		r.rate = shown / dt;
		if (r.refFps && expected > 0.0) {
			r.fraction = shown / expected;
		}
		r.inGrace = inGrace;
		const bool eligible = r.refFps && !inGrace && mostlyLive && r.fraction && *r.fraction >= kMinLockRatio;
		UpdateLock(e, r.fraction, eligible);
		sessionRate = r.rate;
	} else if (r.status == Status::Ok && r.kind == Kind::Async) {
		r.inputFps = dDelivered / dt;
		r.renderedFps = dNew / dt;
		if (r.refFps && dLive > 0) {
			r.below = dNew < kBelowFactor * std::min(static_cast<double>(dDelivered), expected);
		}
		sessionRate = r.renderedFps;
		sessionInput = r.inputFps;
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
			if (r.refFps) {
				s.ref[static_cast<int>(std::lround(*r.refFps))] += dt;
			}
			if (e.lockedFraction >= 0) {
				s.lockedSec[e.lockedFraction] += dt;
			}
			s.belowSec += r.below ? dt : 0.0;
		}
	}
	if (inSession_ && r.status == Status::Unmeasurable) {
		e.session.everUnmeasurable = true;
	}
	r.sinceReset = e.window;
	return r;
}

void Tracker::Sample(const SampleInput &in)
{
	rows_.clear();
	const bool gapOk = in.dtSec > 0.0 && in.dtSec <= kMaxSampleGapSec;
	std::set<std::string> seen;
	for (const SourceInput &src : in.sources) {
		if (!IsReported(src.counts.kind) || !seen.insert(src.uuid).second) {
			continue;
		}
		Entry &e = entries_[src.uuid];
		e.name = src.name;
		const bool restart = !e.baselined || !gapOk || e.identity != src.identity || e.kind != src.counts.kind;
		if (restart) {
			// A new window: no delta across it, and the grace period runs again.
			Rebaseline(e, src);
			Row r;
			r.uuid = src.uuid;
			r.name = src.name;
			r.kind = src.counts.kind;
			r.refFps = RefFps(src.reach, in.mainFps);
			r.status = !src.showing ? Status::Idle
						: (r.kind == Kind::None ? Status::Unmeasurable : Status::Ok);
			r.inGrace = IsDisplayRate(r.kind);
			r.sinceReset = e.window;
			if (inSession_ && r.status == Status::Unmeasurable) {
				e.session.everUnmeasurable = true;
			}
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
	inSession_ = true;
	sessionStartNs_ = nowNs;
	for (auto &[uuid, e] : entries_) {
		e.session = Session{};
	}
}

std::string Tracker::Summarize(const std::string &name, const Entry &e) const
{
	const Session &s = e.session;
	const std::string quoted = "'" + name + "'";
	if (s.liveSec <= 0.0) {
		return s.everUnmeasurable ? quoted + " unmeasurable" : std::string();
	}
	if (s.measuredAs == Kind::Async) {
		const double belowPct = s.belowSec / s.liveSec * 100.0;
		return quoted +
		       Format(" async in %.1f out %.1f, below %.1f%%", Median(s.input), Median(s.rate), belowPct);
	}

	std::string out =
		quoted + (s.measuredAs == Kind::Dxgi ? " DXGI" : " WGC") + Format(" median %.1f/s", Median(s.rate));
	if (s.ref.empty()) {
		out += " (no ref)";
	} else {
		const auto mode = std::max_element(s.ref.begin(), s.ref.end(),
						   [](const auto &a, const auto &b) { return a.second < b.second; });
		out += Format(" (ref %.0f)", static_cast<double>(mode->first));
	}
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
		// Entries kept only for their sums are done with once the line is written.
		it = it->second.baselined ? std::next(it) : entries_.erase(it);
	}
	return line;
}

void Tracker::Clear()
{
	entries_.clear();
	rows_.clear();
	inSession_ = false;
	sessionStartNs_ = 0;
}

} // namespace CaptureRate
