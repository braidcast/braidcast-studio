#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Capture rate: how many real frames per second each capture source delivers,
// judged against what the live canvases reaching it could show.
//
// This header is the pure tracker: no libobs, no CEF, so the rules below are unit
// tested directly. capture_rate_sampler.hpp feeds it from the live source graph.
//
// The rules, and why:
//   - The denominator is the source's own live ticks, never the render loop's slot
//     count: a loop that overruns counts one tick, so render lag is not read as
//     capture lag.
//   - expected = live ticks x min(1, ref / main fps), where ref is the best frame
//     rate among LIVE canvases reaching the source. No live canvas, no rule.
//   - WGC and DXGI report only when the screen changes, and a browser source only
//     when its page repaints, so a low rate there is a fact about the content,
//     never a fault. They show a rate; display capture adds a neutral lock note
//     when it holds steady at a simple fraction of the canvas. Only async sources,
//     whose producer rate is known, can read "below".
//   - Browser (overlay) rows go to the session line and diagnostics.get only,
//     never stats.get or the Stats panel; the sampler hands them out separately.
//
// UI thread only, like its owner.
namespace CaptureRate {

// Mirrors libobs's enum obs_frame_count_kind.
enum class Kind { None, Wgc, Dxgi, Async, BrowserPaint, GameHook };

const char *KindName(Kind kind);

struct Counts {
	Kind kind = Kind::None;
	uint32_t liveTicks = 0;
	uint32_t newFrameTicks = 0;
	uint32_t framesDelivered = 0;
};

// One canvas root that reaches a source.
struct Reach {
	double canvasFps = 0.0;
	bool live = false;
};

struct SourceInput {
	std::string uuid;
	std::string name;
	// Changes whenever the uuid names a different source object. uuids survive
	// recreation (a load restores the saved uuid), so the uuid alone cannot say
	// whether two counter readings come from the same counters.
	uint64_t identity = 0;
	bool showing = false;
	// False when the capture method delivers frames with no counter behind them
	// (BitBlt window capture, a deinterlaced async source, a hooked game
	// capture). Only then does a showing source that counts nothing read
	// "unmeasurable"; otherwise it is simply not capturing right now.
	bool frameSignal = true;
	Counts counts;
	std::vector<Reach> reach;
};

// The capture source types, by obs_source_get_id.
inline constexpr const char *kMonitorCaptureId = "monitor_capture";
inline constexpr const char *kWindowCaptureId = "window_capture";
inline constexpr const char *kGameCaptureId = "game_capture";

// What the sampler reads off a source that reports no frame-count kind.
struct SourceTraits {
	std::string id;            // obs_source_get_id
	bool async = false;        // OBS_SOURCE_ASYNC_VIDEO
	bool deinterlaced = false; // async only
	bool windowWgc = false;    // window capture set to the WGC method
	// obs_source_get_base_width > 0. Game capture reports no size until it has
	// hooked a process and is capturing from it.
	bool hasSize = false;
};

// Whether a source that reports no kind still gets a row: async video and the
// capture source types, so a missing number is visible as such rather than the
// source vanishing from the stats and the session line.
bool IsListed(const SourceTraits &traits);

// Whether the source's capture method counts its frames at all, for a source
// that reports no kind. SourceInput::frameSignal.
bool HasFrameSignal(const SourceTraits &traits);

struct SampleInput {
	double dtSec = 0.0; // measured time since the previous sample
	double mainFps = 0.0;
	std::vector<SourceInput> sources;
};

enum class Status {
	Ok,           // measured
	Unmeasurable, // showing, but no frame signal or none this phase measures (the game hook)
	Idle,         // not capturing now: hidden, or nothing to count (camera stopped, capture failed)
};

const char *StatusName(Status status);

struct Row {
	std::string uuid;
	std::string name;
	Kind kind = Kind::None;
	Status status = Status::Idle;
	std::optional<double> refFps;
	// WGC / DXGI / browser paint: frames shown per second. WGC / DXGI also give
	// that as a share of what the canvas could show over the same ticks.
	std::optional<double> rate;
	std::optional<double> fraction;
	// Async: frames the producer delivered and frames that reached the render.
	std::optional<double> inputFps;
	std::optional<double> renderedFps;
	bool below = false;                   // async only
	const char *lockedFraction = nullptr; // "1/2", ... while locked
	bool inGrace = false;
	struct {
		double liveSec = 0.0;
		double belowSec = 0.0;
		double lockedSec = 0.0;
	} sinceReset;
};

// Best min(canvas fps, main fps) over the live canvases in `reach`; nullopt when
// none is live.
std::optional<double> RefFps(const std::vector<Reach> &reach, double mainFps);

class Tracker {
public:
	// Feeds one sample. A dt outside (0, kMaxSampleGapSec] re-baselines every
	// source, since a rate over a long pause means nothing.
	void Sample(const SampleInput &in);

	// The rows computed by the last Sample, one per source it carried.
	const std::vector<Row> &Rows() const { return rows_; }

	// Forget the last rows and every baseline; the next sample starts over.
	void Pause();

	// stats.reset: rebase the per-row "since reset" window. Session sums stay.
	void ResetWindows();

	// Opens a session; a no-op while one is open, since live edges can repeat.
	void SessionBegin(uint64_t nowNs);
	// Closes the session and returns its one-line summary ("" without a session).
	std::string SessionEnd(uint64_t nowNs);
	bool InSession() const { return inSession_; }

	void Clear();

	static constexpr double kMaxSampleGapSec = 2.5;

private:
	// Rates are binned at 0.1 fps; value = seconds spent at that rate.
	using Histogram = std::map<int, double>;

	struct Session {
		Kind measuredAs = Kind::None;
		double liveSec = 0.0;
		Histogram rate;  // WGC/DXGI displayed, async rendered
		Histogram input; // async input
		Histogram ref;
		std::map<int, double> lockedSec; // by fraction index
		double belowSec = 0.0;
		Kind countedAs = Kind::None; // at the last sample that read ok, live or not
		bool everShowing = false;
		bool everUnmeasurable = false;
	};

	struct Entry {
		std::string name;
		Kind kind = Kind::None;
		uint64_t identity = 0;
		bool baselined = false;
		bool hadRef = false; // a live canvas reached it at the last sample
		Counts last;
		double sinceBaselineSec = 0.0;
		int streakFraction = -1;
		int streakCount = 0;
		int lockedFraction = -1;
		int exitCount = 0;
		Session session;
		decltype(Row::sinceReset) window;
	};

	void RestartGrace(Entry &e);
	void ClearLock(Entry &e);
	void Rebaseline(Entry &e, const SourceInput &src, double mainFps);
	Row Evaluate(Entry &e, const SourceInput &src, double dt, double mainFps);
	void UpdateLock(Entry &e, std::optional<double> fraction, bool eligible);
	void NoteSession(Entry &e, const SourceInput &src, Status status);
	std::string Summarize(const std::string &name, const Entry &e) const;

	std::map<std::string, Entry> entries_;
	std::vector<Row> rows_;
	bool inSession_ = false;
	uint64_t sessionStartNs_ = 0;
};

} // namespace CaptureRate
