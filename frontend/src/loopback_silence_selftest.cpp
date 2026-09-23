#include "loopback_silence_selftest.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <obs.h>
#include <obs.hpp>

#include "bridge.hpp"
#include "log.hpp"
#include "obs_bootstrap.hpp"
#include "util/env_config.hpp"
#include "util/selftest_paths.hpp"
#include "util/session_log.hpp"

namespace {

// State-machine phases, advanced one step per WM_TIMER tick (see
// ObsBootstrap::RunLoopbackSilenceSelfTest). BootSettle, AwaitFirstPacket and
// Measuring are the only phases that span multiple ticks; every other phase runs
// to completion within the tick it is entered on (see the `continue`-driven loop).
enum class Phase {
	Idle,
	BootSettle,
	OpenCapture,
	AwaitFirstPacket,
	Measuring,
	Verdict,
	Teardown,
	Finished,
};

// Written from the audio thread by OnAudio, read from the UI thread between ticks.
struct Counters {
	std::mutex lock;
	ULONGLONG firstTick = 0;
	ULONGLONG lastTick = 0;
	ULONGLONG maxGapMs = 0;
	uint64_t frames = 0;
	uint64_t packets = 0;
};

struct State {
	Phase phase = Phase::Idle;
	int exitCode = -1; // -1 = not yet decided; see LoopbackSilenceSelfTestExitCode()
	std::string skipReason;

	obs_source_t *capture = nullptr;
	obs_source_t *priorChannelSource = nullptr;
	bool callbackAdded = false;
	std::string endpoint;

	std::chrono::steady_clock::time_point phaseStart;

	int durationSec = 0;
	ULONGLONG maxGapThresholdMs = 0;

	// Filled by Verdict, and left zeroed by a run that ended before it measured anything.
	// Kept on the state rather than as locals so Teardown can write one summary for every
	// path, measured or not: a run that did not measure still has to say so on disk.
	bool measured = false;
	double elapsedSec = 0.0;
	double impliedRateHz = 0.0;
	double rateDeficitPct = 0.0;
	uint32_t endpointRateHz = 0;
	uint32_t mixRateHz = 0;
	ULONGLONG maxGapMs = 0;
	uint64_t frames = 0;
	uint64_t packets = 0;
	int tsJumps = 0;
	int audioRestarts = 0;
};

State g_state;

// Apart from State, which is reassigned wholesale on arm and cannot hold a mutex.
Counters g_counters;

// A live endpoint delivers a packet every engine period, so an opened capture that has
// produced nothing this long is already sitting in the defect's first gap -- an endpoint
// whose engine was idle before the capture ever started. Measured: the unfixed build
// delivers exactly zero packets on a quiet endpoint, so this is the usual way it fails.
constexpr std::chrono::seconds kFirstPacketWait{20};

// Long enough that an endpoint which idles on silence has done so several times over: the
// three logged occurrences of this defect were gaps of 22.6 s, 53.6 s and 340 s.
constexpr int kDefaultDurationSec = 120;

// Gap gate. libobs buffers a lagging source for 45 ticks -- 960 ms at a 48 kHz mix -- before it
// restarts the source's audio, and routine jitter on a healthy endpoint is tens of
// milliseconds, so this sits well
// clear of both: it separates "the engine idled" from "a packet was late".
constexpr ULONGLONG kDefaultMaxGapMs = 500;

// Distinctive enough that a substring match against a log line cannot collide with another
// source's name. Private, so it is never written into a scene collection.
constexpr const char *kSourceName = "selftest-loopback-silence";

// libobs' two downstream complaints about a source whose packets stopped arriving:
// obs-source.c's TS_SMOOTHING_THRESHOLD warning and obs-audio.c's restart.
constexpr const char *kTimestampMarker = "exceeded TS_SMOOTHING_THRESHOLD";
constexpr const char *kRestartMarker = "Restarting source audio";

// win-wasapi's own line for a capture whose device opened. It separates the two reasons no
// audio arrives: an endpoint that never opened (nothing to measure) from one that opened and
// then delivered nothing, which is the defect itself.
constexpr const char *kInitializedMarker = "] initialized (source:";

void OnAudio(void *param, obs_source_t *, const struct audio_data *data, bool)
{
	Counters &counters = *static_cast<Counters *>(param);
	const ULONGLONG now = GetTickCount64();

	std::lock_guard<std::mutex> guard(counters.lock);
	if (counters.lastTick == 0) {
		counters.firstTick = now;
	} else {
		counters.maxGapMs = std::max(counters.maxGapMs, now - counters.lastTick);
	}
	counters.lastTick = now;
	counters.frames += data->frames;
	++counters.packets;
}

// What this session's log says about `source`, in one pass over the file. Counts are -1 if the
// log could not be read at all, and endpointRateHz is 0 if no readable "initialized" line named
// the source.
//
// The file rather than obs_bootstrap.cpp's LogLineCounter, which counts single substrings through
// an installed handler: every needle here is a conjunction of a marker AND this source's name,
// and LogLineCounter's tally is process-global state built for a scoped case rather than a run
// this long. Reading the file back also means the verdict reflects what an auditor will read
// afterwards, not a private tally. The session log handler flushes every message, so lines
// emitted moments ago are already on disk.
//
// Every marker is a substring of a log line owned by another file, so a reworded line reads here
// as "it never happened". kInitializedMarker's producer carries a back-reference comment for
// that reason, because its drift would route the defect itself to SKIP; the libobs two are left
// bare, since their drift only degrades the gate to its two measured checks, which catch the
// defect directly.
struct LogScan {
	int tsJumps = -1;
	int audioRestarts = -1;
	int opened = -1;
	uint32_t endpointRateHz = 0;
};

LogScan ScanSessionLog(const std::string &path, const std::string &source)
{
	LogScan scan;
	std::ifstream in(path);
	if (!in) {
		return scan;
	}

	scan.tsJumps = 0;
	scan.audioRestarts = 0;
	scan.opened = 0;

	std::string line;
	while (std::getline(in, line)) {
		if (line.find(source) == std::string::npos) {
			continue;
		}
		if (line.find(kTimestampMarker) != std::string::npos) {
			++scan.tsJumps;
		}
		if (line.find(kRestartMarker) != std::string::npos) {
			++scan.audioRestarts;
		}
		const size_t init = line.find(kInitializedMarker);
		if (init == std::string::npos) {
			continue;
		}
		++scan.opened;

		// The endpoint's own sample rate, out of the same line:
		//   WASAPI: Device '<name>' [<rate> Hz] initialized (source: <source>)
		// Reported, not gated on: libobs resamples a source to the mix rate before the capture
		// callback ever sees it, so this is NOT the rate the counted frames are in. It is here
		// because the two differing is worth seeing in a summary. Walking backwards from the
		// marker means a device name containing a bracket or " Hz]" cannot confuse the parse.
		const size_t open = line.rfind('[', init);
		const size_t hz = line.find(" Hz]", open == std::string::npos ? 0 : open);
		if (open == std::string::npos || hz == std::string::npos || hz <= open) {
			continue;
		}
		// Last one wins: a restart re-initializes, and what this reports is the endpoint rate
		// in force at the end of the window.
		scan.endpointRateHz = uint32_t(strtoul(line.substr(open + 1, hz - open - 1).c_str(), nullptr, 10));
	}
	return scan;
}

// Puts the run's numbers where someone who was not there can find them, in perf-repro's shape
// and beside its summaries. `endpoint` is the load-bearing field: a green run only means
// anything if the endpoint it ran against was genuinely quiet, and that cannot be checked
// afterwards from a verdict that does not say which one it was. Written for every path,
// including the ones that never measured, so a missing file means the run did not finish.
void WriteSummary(const State &st)
{
	// One value behind both fields. Naming the raw code and numbering a clamped one is how a
	// record ends up calling itself NOT RUN beside an exit code of 0.
	const int exitCode = st.exitCode < 0 ? 3 : st.exitCode;
	const Bridge::json summary{
		{"result", SelfTest::ResultName(exitCode)},
		{"exitCode", exitCode},
		{"reason", st.skipReason},
		{"endpoint", st.endpoint},
		{"measured", st.measured},
		{"durationSec", st.durationSec},
		{"elapsedSec", st.elapsedSec},
		{"packets", st.packets},
		{"frames", st.frames},
		{"endpointRateHz", st.endpointRateHz},
		{"mixRateHz", st.mixRateHz},
		{"impliedRateHz", st.impliedRateHz},
		{"rateDeficitPct", st.rateDeficitPct},
		{"rateDeficitThresholdPct", SelfTest::kMaxRateDeficitPct},
		{"maxGapMs", st.maxGapMs},
		{"maxGapThresholdMs", st.maxGapThresholdMs},
		{"tsJumps", st.tsJumps},
		{"audioRestarts", st.audioRestarts},
	};

	const std::string path = SelfTest::WriteSummaryFile("loopback-silence", summary.dump(2));
	HostLog("[selftest-stream] loopback-silence summary=" + (path.empty() ? "(unwritten)" : path));
}

void Teardown(State &st)
{
	if (st.callbackAdded && st.capture) {
		// Returns with no callback running and none to follow, so nothing touches the
		// counters after this.
		obs_source_remove_audio_capture_callback(st.capture, OnAudio, &g_counters);
		st.callbackAdded = false;
	}
	if (st.capture) {
		obs_set_output_source(ObsBootstrap::kSelfTestOutputChannel,
				      st.priorChannelSource); // null or the prior source
		obs_source_release(st.capture);
		st.capture = nullptr;
	}
	if (st.priorChannelSource) {
		obs_source_release(st.priorChannelSource);
		st.priorChannelSource = nullptr;
	}
}

// The ways this flow ends before it has a measurement to report: one line each, then
// teardown. A verdict reports itself.
void Bail(State &st, int exitCode, const std::string &reason)
{
	st.exitCode = exitCode;
	st.skipReason = reason;
	HostLog("[selftest-stream] loopback-silence " + std::string(SelfTest::ResultName(exitCode)) + " " + reason);
	st.phase = Phase::Teardown;
}

} // namespace

void ObsBootstrap::ArmLoopbackSilenceSelfTest(HWND)
{
	g_state = State{};
	{
		std::lock_guard<std::mutex> guard(g_counters.lock);
		g_counters.firstTick = 0;
		g_counters.lastTick = 0;
		g_counters.maxGapMs = 0;
		g_counters.frames = 0;
		g_counters.packets = 0;
	}
	g_state.phase = Phase::BootSettle;
	g_state.phaseStart = std::chrono::steady_clock::now();

	const long durationRaw = Env::Number("BRAIDCAST_SELFTEST_DURATION", kDefaultDurationSec);
	g_state.durationSec = durationRaw > 0 ? int(durationRaw) : kDefaultDurationSec;
	const long gapRaw = Env::Number("BRAIDCAST_SELFTEST_MAXGAP", long(kDefaultMaxGapMs));
	g_state.maxGapThresholdMs = gapRaw > 0 ? ULONGLONG(gapRaw) : kDefaultMaxGapMs;

	HostLog("[selftest-stream] loopback-silence armed: duration=" + std::to_string(g_state.durationSec) +
		"s maxGapMs=" + std::to_string(g_state.maxGapThresholdMs));
}

bool ObsBootstrap::RunLoopbackSilenceSelfTest()
{
	State &st = g_state;

	for (;;) {
		switch (st.phase) {
		case Phase::Idle:
			return true; // never armed

		case Phase::BootSettle: {
			if (std::chrono::steady_clock::now() - st.phaseStart < SelfTest::kBootSettle) {
				return false;
			}
			st.phase = Phase::OpenCapture;
			continue;
		}

		case Phase::OpenCapture: {
			// This case only exists on an endpoint whose audio engine has nothing else
			// keeping it running, and any other render session on it -- another app's, or
			// this very fix's -- holds the engine up and makes both a fixed and an unfixed
			// build pass. So an unconfigured run does not run at all; a guessed endpoint
			// would report PASS while proving nothing.
			const SelfTestEndpoint endpoint = ResolveSelfTestEndpoint("[selftest-stream] loopback-silence");
			if (endpoint.id.empty()) {
				Bail(st, endpoint.exitCode, endpoint.reason);
				continue;
			}
			st.endpoint = endpoint.id;

			OBSDataAutoRelease settings = obs_data_create();
			obs_data_set_string(settings, "device_id", st.endpoint.c_str());
			st.capture = obs_source_create_private("wasapi_output_capture", kSourceName, settings);
			if (!st.capture) {
				Bail(st, 3, "wasapi_output_capture create failed");
				continue;
			}

			st.priorChannelSource =
				obs_get_output_source(ObsBootstrap::kSelfTestOutputChannel); // saved to restore
			obs_set_output_source(ObsBootstrap::kSelfTestOutputChannel, st.capture);
			obs_source_add_audio_capture_callback(st.capture, OnAudio, &g_counters);
			st.callbackAdded = true;

			HostLog("[selftest-stream] loopback-silence capture open on endpoint " + st.endpoint);
			st.phaseStart = std::chrono::steady_clock::now();
			st.phase = Phase::AwaitFirstPacket;
			return false;
		}

		case Phase::AwaitFirstPacket: {
			ULONGLONG firstTick = 0;
			{
				std::lock_guard<std::mutex> guard(g_counters.lock);
				firstTick = g_counters.firstTick;
			}
			if (firstTick != 0) {
				HostLog("[selftest-stream] loopback-silence measuring for " +
					std::to_string(st.durationSec) + "s");
				st.phaseStart = std::chrono::steady_clock::now();
				st.phase = Phase::Measuring;
				return false;
			}
			if (std::chrono::steady_clock::now() - st.phaseStart < kFirstPacketWait) {
				return false;
			}
			// An endpoint whose engine is already idle when the capture opens delivers
			// nothing from the start, so this stretch is the defect's first gap rather
			// than a run that failed to get going -- as long as the device did open.
			// An unreadable log cannot tell those apart, and must not answer "never
			// opened", which would route the defect itself to a pass-adjacent verdict.
			const int opened = ScanSessionLog(SessionLog::CurrentPath(), kSourceName).opened;
			if (opened < 0) {
				Bail(st, 3,
				     "no audio arrived and this session's log could not be read back to "
				     "tell a silent endpoint from a capture that never opened");
			} else if (opened > 0) {
				Bail(st, 1,
				     "the capture opened on this endpoint and then delivered no audio at all in " +
					     std::to_string(kFirstPacketWait.count()) + "s");
			} else {
				Bail(st, 2, "the capture never opened on this endpoint");
			}
			continue;
		}

		case Phase::Measuring: {
			if (std::chrono::steady_clock::now() - st.phaseStart < std::chrono::seconds(st.durationSec)) {
				return false;
			}
			st.phase = Phase::Verdict;
			continue;
		}

		case Phase::Verdict: {
			ULONGLONG firstTick = 0;
			ULONGLONG lastTick = 0;
			ULONGLONG maxGapMs = 0;
			uint64_t frames = 0;
			uint64_t packets = 0;
			{
				std::lock_guard<std::mutex> guard(g_counters.lock);
				firstTick = g_counters.firstTick;
				lastTick = g_counters.lastTick;
				maxGapMs = g_counters.maxGapMs;
				frames = g_counters.frames;
				packets = g_counters.packets;
			}
			// A window that ends mid-gap is the shape this defect produces most often,
			// so the trailing stretch counts as much as one between two packets.
			const ULONGLONG now = GetTickCount64();
			maxGapMs = std::max(maxGapMs, now - lastTick);

			const double elapsedSec = double(now - firstTick) / 1000.0;
			// Frames per second of wall clock: with no gap this lands on the rate the
			// counted frames are in, and an engine that idled drags it below.
			const double impliedRateHz = elapsedSec > 0.0 ? double(frames) / elapsedSec : 0.0;

			const LogScan scan = ScanSessionLog(SessionLog::CurrentPath(), kSourceName);
			const int tsJumps = scan.tsJumps;
			const int audioRestarts = scan.audioRestarts;
			const uint32_t endpointRateHz = scan.endpointRateHz;
			// The MIX rate, because libobs resamples a source to it before a capture
			// callback sees the buffer: process_audio() lets the resampler rewrite the
			// frame count, copy_audio_data() stores that post-resample count as
			// source->audio_data.frames, and that is what reaches the callback. libobs then
			// advances its own timeline with conv_frames_to_time(mix rate, frames), so the
			// mix rate is the only rate these frames are ever in. Dividing by the endpoint's
			// rate instead reads an 8% deficit on a healthy 44.1 kHz mix of a 48 kHz
			// endpoint, and the mix rate is user-settable to 44100.
			// Null-checked because obs_get_audio() hands back obs->audio.audio raw and
			// audio_output_get_sample_rate dereferences it unchecked: obs_reset_audio2
			// leaves that pointer NULL while it swaps the mix, so reading it bare would
			// crash where the guard below wants to report "could not judge".
			audio_t *const mix = obs_get_audio();
			const uint32_t mixRateHz = mix ? audio_output_get_sample_rate(mix) : 0;
			// Only a shortfall fails: a reading a hair above is tick granularity, not audio
			// arriving from nowhere.
			const double rateDeficitPct = mixRateHz > 0 ? (1.0 - impliedRateHz / double(mixRateHz)) * 100.0
								    : 0.0;

			st.measured = true;
			st.elapsedSec = elapsedSec;
			st.impliedRateHz = impliedRateHz;
			st.rateDeficitPct = rateDeficitPct;
			st.endpointRateHz = endpointRateHz;
			st.mixRateHz = mixRateHz;
			st.maxGapMs = maxGapMs;
			st.frames = frames;
			st.packets = packets;
			st.tsJumps = tsJumps;
			st.audioRestarts = audioRestarts;

			if (tsJumps < 0 || audioRestarts < 0) {
				st.exitCode = 3;
				st.skipReason = "this session's log could not be read back";
			} else if (mixRateHz == 0) {
				st.exitCode = 3;
				st.skipReason = "the audio mix sample rate could not be read, so the received "
						"sample count cannot be held against wall clock";
			} else {
				st.exitCode = (maxGapMs <= st.maxGapThresholdMs &&
					       rateDeficitPct <= SelfTest::kMaxRateDeficitPct && tsJumps == 0 &&
					       audioRestarts == 0)
						      ? 0
						      : 1;
			}

			HostLog("[selftest-stream] loopback-silence " + std::string(SelfTest::ResultName(st.exitCode)) +
				" endpoint=" + st.endpoint + " elapsedSec=" + std::to_string(elapsedSec) +
				" packets=" + std::to_string(packets) + " frames=" + std::to_string(frames) +
				" impliedRateHz=" + std::to_string(impliedRateHz) + " endpointRateHz=" +
				std::to_string(endpointRateHz) + " mixRateHz=" + std::to_string(mixRateHz) +
				" rateDeficitPct=" + std::to_string(rateDeficitPct) + " rateDeficitThresholdPct=" +
				std::to_string(SelfTest::kMaxRateDeficitPct) + " maxGapMs=" + std::to_string(maxGapMs) +
				" maxGapThresholdMs=" + std::to_string(st.maxGapThresholdMs) + " tsJumps=" +
				std::to_string(tsJumps) + " audioRestarts=" + std::to_string(audioRestarts) +
				(st.skipReason.empty() ? "" : " reason=" + st.skipReason));

			st.phase = Phase::Teardown;
			continue;
		}

		case Phase::Teardown: {
			Teardown(st);
			WriteSummary(st);
			st.phase = Phase::Finished;
			continue;
		}

		case Phase::Finished:
			return true;
		}
	}
}

int ObsBootstrap::LoopbackSilenceSelfTestExitCode()
{
	// 3, not 0: a run that was armed and never reached a verdict -- the window closed early,
	// an operator closed it, a CI step terminated it -- measured nothing, and a caller reading
	// 0 as PASS would record a pass for it.
	return g_state.exitCode < 0 ? 3 : g_state.exitCode;
}
