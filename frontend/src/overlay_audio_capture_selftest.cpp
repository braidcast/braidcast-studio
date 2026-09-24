#include "overlay_audio_capture_selftest.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <obs.h>
#include <obs.hpp>
#include <util/platform.h>

#include "bridge.hpp"
#include "log.hpp"
#include "obs_bootstrap.hpp"
#include "overlay/overlay_sources.hpp"
#include "overlay/overlay_store.hpp"
#include "util/env_config.hpp"
#include "util/selftest_paths.hpp"

namespace {

using json = Bridge::json;

// State-machine phases, advanced one step per WM_TIMER tick. Setup, Verdict and Teardown run to
// completion within the tick they are entered on; the rest wait on a clock or a callback.
enum class Phase {
	Idle,
	BootSettle,
	Setup,
	AwaitPage,
	PageSettle,
	Firing,
	Drain,
	Verdict,
	Teardown,
	Finished,
};

// The channels each capture keeps: the front pair, which is where obs-browser subtracts the
// keepalive (AUDIO_KEEPALIVE_CHANNELS), so a subtraction missed on either one shows in that
// channel's floor. Scoring against the clip reads channel 0 alone -- an alert plays the same
// signal on both.
constexpr size_t kMeasuredChannels = 2;

// One block of audio as libobs handed it over. `offset` indexes every Capture::samples channel
// alike, or is kNoSamples for a block that arrived outside every alert's recording window and
// was only counted.
constexpr size_t kNoSamples = SIZE_MAX;
struct Packet {
	uint64_t arrivalNs;
	uint64_t timestampNs;
	uint32_t frames;
	size_t offset;
};

// Written from an audio thread, read from the UI thread once the alerts are over. Every block is
// counted, but samples are kept only until `recordUntilNs`, which each fire pushes out by one
// recording window: that bounds memory however long the gaps between alerts are.
struct Capture {
	std::mutex lock;
	std::vector<Packet> packets;
	std::array<std::vector<float>, kMeasuredChannels> samples;
	// How many of kMeasuredChannels every recorded block carried: a mono mix has only one.
	size_t channels = kMeasuredChannels;
	// Samples per channel it may keep; set from the mix rate before either capture is tapped.
	size_t capacity = 0;
	bool overflowed = false;
	std::atomic<uint64_t> recordUntilNs{0};

	void Reset()
	{
		std::lock_guard<std::mutex> guard(lock);
		packets.clear();
		for (std::vector<float> &channel : samples) {
			channel.clear();
		}
		channels = kMeasuredChannels;
		capacity = 0;
		overflowed = false;
		recordUntilNs = 0;
	}
};

struct Snapshot {
	std::vector<Packet> packets;
	std::array<std::vector<float>, kMeasuredChannels> samples;
	size_t channels = 0;
	bool overflowed = false;
};

// A bridge call's answer, filled on the UI thread when the async lane resolves it.
struct CallResult {
	bool done = false;
	bool ok = false;
	json result;
	std::string error;
};

// The async bridge lane answers through a CEF message-router callback; this one stores the
// answer where the state machine polls for it instead of handing it to a page.
class ResultCallback : public CefMessageRouterBrowserSide::Callback {
public:
	explicit ResultCallback(std::shared_ptr<CallResult> out) : out_(std::move(out)) {}

	void Success(const CefString &response) override
	{
		out_->done = true;
		out_->ok = true;
		out_->result = json::parse(response.ToString(), nullptr, false);
	}
	void Success(const void *, size_t) override
	{
		out_->done = true;
		out_->ok = true;
	}
	void Failure(int, const CefString &message) override
	{
		out_->done = true;
		out_->ok = false;
		out_->error = message.ToString();
	}

private:
	std::shared_ptr<CallResult> out_;
	IMPLEMENT_REFCOUNTING(ResultCallback);
};

std::shared_ptr<CallResult> CallAsync(const std::string &method, const json &params)
{
	auto out = std::make_shared<CallResult>();
	if (!Bridge::DispatchAsync(method, params, new ResultCallback(out))) {
		out->done = true;
		out->error = method + " is not an async bridge method";
	}
	return out;
}

size_t Delivered(const CallResult &call)
{
	return call.ok && call.result.is_object() ? call.result.value("delivered", size_t(0)) : 0;
}

// How the overlay source itself delivered one alert, before the mix.
struct SourceStats {
	size_t packets = 0;
	double firstPacketMs = -1.0; // first packet after the fire, relative to it
	double maxHoleMs = 0.0;      // largest forward jump between consecutive packet timestamps
};

// How one alert reached the mix.
struct MixScore {
	double latencyMs = -1.0;    // fire -> the mix block holding the clip's first sample, by arrival
	double gain = 0.0;          // the path's level relative to the clip, over the blocks that arrived
	double retained = 0.0;      // share of the clip's energy that arrived at that level
	double onsetRetained = 0.0; // the same, over the clip's loudest stretch only
	// Per measured channel: the mix's level in the quiet stretch before the next alert, and the
	// DC component of that level. A channel the mix does not carry stays at -200.
	std::array<double, kMeasuredChannels> floorDbfs{-200.0, -200.0};
	std::array<double, kMeasuredChannels> floorDcDbfs{-200.0, -200.0};
	std::string missing; // stretches of the clip that did not arrive, as "from-to ms(share%)"
};

struct AlertResult {
	uint64_t fireNs = 0;
	std::shared_ptr<CallResult> call;
	SourceStats source;
	MixScore mix;
};

struct State {
	Phase phase = Phase::Idle;
	int exitCode = -1; // -1 = not yet decided; see OverlayAudioCaptureSelfTestExitCode()
	std::string reason;

	bool monitorVariant = false;
	int alertCount = 0;
	uint32_t mixRateHz = 0;
	std::string clipOrigin;
	std::vector<unsigned char> clipFile;
	std::vector<float> reference;

	std::string widgetId;
	obs_source_t *source = nullptr;
	obs_scene_t *scene = nullptr;
	obs_source_t *priorChannelSource = nullptr;
	bool outputTapped = false;
	bool mixTapped = false;
	// Every other source that fed the measured track, with the mixer mask it had, so the track
	// carries the case and nothing else and every mask goes back afterwards.
	std::vector<std::pair<OBSWeakSource, uint32_t>> maskedSources;

	// Monitor variant only.
	std::string endpoint;
	obs_source_t *loopback = nullptr;
	bool monitorDeviceSwapped = false;
	std::string priorMonitorName;
	std::string priorMonitorId;

	std::shared_ptr<CallResult> probe;
	std::chrono::steady_clock::time_point phaseStart;
	uint64_t firingStartNs = 0;
	int jumpsBeforeFiring = -1;
	int spacingOverrideMs = 0;
	int uiStallMs = 0;
	int fired = 0;
	std::vector<AlertResult> alerts;

	bool measured = false;
	double elapsedSec = 0.0;
	uint64_t sourceFrames = 0;
	double impliedRateHz = 0.0;
	double rateDeficitPct = 0.0;
	int tsJumps = -1;
	int smoothingMisses = -1;
	double maxFloorDbfs = -200.0;
	size_t mixChannels = 0; // how many of kMeasuredChannels the mix carried, and so were judged
};

State g_state;

// Apart from State, which is reassigned wholesale on arm and cannot hold a mutex.
Capture g_output; // the overlay source's own packets
Capture g_mix;    // the measured mix track

// Loading the page, opening its event socket and decoding its sound take well under a second
// here; the deadline only separates "slow" from "never".
constexpr std::chrono::seconds kPageWait{20};

// After the page is listening: long enough that the sound has decoded and, on a fixed build,
// that the keepalive's capture stream has been running for a while before the first alert.
constexpr std::chrono::milliseconds kPageSettle{3000};

// Gaps before each successive alert, cycled. All clear CEF's two-second recently-audible timeout
// by at least three times, so on an unfixed build every alert's capture has been torn down before
// the next; they differ so no alert lands at a fixed phase of anything periodic.
constexpr int kSpacingMs[] = {6000, 7500, 9000, 6500, 8000, 10000};
// BRAIDCAST_SELFTEST_SPACING_MS replaces every gap with one fixed value, for probing what a long
// silence between alerts does; values below the shortest gap above are raised to it.
constexpr int kMinSpacingMs = 6000;

// How long after a fire the captures keep samples. The shortest gap, so windows never overlap.
constexpr uint64_t kRecordNs = uint64_t(kMinSpacingMs) * 1000000;

constexpr int kMinAlerts = 5;

// How long each fire holds the UI thread, standing in for the load it carries on air. CEF detects
// audibility and creates its capturer on this thread, so an unfixed build attaches only once the
// stall ends; measured at idle it attaches within 75-90 ms, soon enough to keep the onset, which
// is why a case without the stall passes the defect. On air the session log showed 0.1-1.4 s
// capture holes on top of the late attach, and the VOD was missing 0.61-1.15 s from the front of
// each alert; a 1 s stall lands inside that.
constexpr int kDefaultUiStallMs = 1000;

// The mix track the case is measured on: track 6, emptied of every other source for the run.
constexpr size_t kTrack = 5;
constexpr uint32_t kTrackBit = 1u << kTrack;

// Per alert: this share of the clip's energy must reach the mix, overall and over the onset.
// On air the defect cost 0.61-1.15 s from the front of each alert in the VOD, which is most of a
// notification sound's energy.
constexpr double kMinRetained = 0.95;
constexpr double kOnsetFromSec = 0.1;
constexpr double kOnsetToSec = 0.4;

// Fire -> first sample in the mix. Covers the event socket, the page, Chromium's output, the
// capture and libobs's own buffering; far past any of those, it is an alert that arrived late.
constexpr double kMaxLatencyMs = 1000.0;

// The keepalive plays a -66 dBFS DC level and obs-browser subtracts that same level from what it
// captures, so the quiet stretch between alerts should be digital silence. This bounds what is
// left: -90 dBFS is 24 dB under the keepalive itself, so a floor above it means the subtraction
// missed (a stale level, a capture that scales or resamples it) or something else reached the
// track, and several rerouted sources' leftovers could add up toward the meter.
constexpr double kMaxFloorDbfs = -90.0;
// The quiet stretch the floor is read over starts this long after a fire, well past the clip.
constexpr double kFloorAfterSec = 3.0;

// Scoring granularity. 10 ms holds three cycles of the synthesized clip's lowest tone, so a
// block's mean is its DC and nothing else.
constexpr double kBlockSec = 0.01;

// A block counts as arrived when the mix matches the clip's waveform this closely there. Lost
// audio is silence (or the keepalive's DC, which the per-block mean removes), whose correlation
// with the clip is near zero; an intact block is above 0.99.
constexpr double kMinBlockCorrelation = 0.9;

// Bounds what each capture records: this long per channel at the mix rate.
constexpr int kMaxCapturedSec = 120;
// As many alerts as that holds: each records one window plus at most the block that straddles its
// end, which 100 ms covers for any block size libobs or CEF uses. More would overflow a capture
// and end the run NOT RUN, so BRAIDCAST_SELFTEST_ALERTS is clamped to this.
constexpr int kMaxAlerts = kMaxCapturedSec * 1000 / (kMinSpacingMs + 100);

// Distinctive enough that a substring match against a log line cannot collide with another
// source's name, including the smoke battery's selftest-overlay-audio.
constexpr const char *kSourceName = "selftest-overlay-audio-capture";
constexpr const char *kLoopbackName = "selftest-overlay-audio-loopback";
constexpr const char *kSceneName = "selftest-overlay-audio-scene";
constexpr const char *kWidgetName = "selftest-overlay-audio-capture";
constexpr const char *kClipAssetKey = "selftest-alert.wav";
// Every line this mode logs, verdict included, in the shape loopback-silence uses. Distinct from
// the smoke battery's "[selftest] overlay-audio", which proves the routing settings instead.
constexpr const char *kLogTag = "[selftest-stream] overlay-audio";

// obs-source.c's two lines for a source whose audio timestamps did not follow on: the jump is
// the one every capture restart produces.
const std::string kJumpNeedle = std::string("Timestamp for source '") + kSourceName + "' jumped";
const std::string kSmoothingNeedle =
	std::string("Audio timestamp for '") + kSourceName + "' exceeded TS_SMOOTHING_THRESHOLD";

// `data` is libobs's planar float block, one plane per channel.
void Record(Capture &capture, const struct audio_data &data)
{
	const uint64_t now = os_gettime_ns();
	std::lock_guard<std::mutex> guard(capture.lock);
	if (!data.data[0]) {
		return;
	}
	if (now >= capture.recordUntilNs) {
		capture.packets.push_back(Packet{now, data.timestamp, data.frames, kNoSamples});
		return;
	}
	const size_t offset = capture.samples[0].size();
	if (offset + data.frames > capture.capacity) {
		capture.overflowed = true;
		return;
	}
	capture.packets.push_back(Packet{now, data.timestamp, data.frames, offset});
	for (size_t c = 0; c < kMeasuredChannels; ++c) {
		std::vector<float> &channel = capture.samples[c];
		const float *pcm = reinterpret_cast<const float *>(data.data[c]);
		if (pcm) {
			channel.insert(channel.end(), pcm, pcm + data.frames);
		} else {
			// Kept aligned with channel 0, and left out of every measurement.
			capture.channels = std::min(capture.channels, c);
			channel.resize(channel.size() + data.frames, 0.0f);
		}
	}
}

void OnSourceAudio(void *param, obs_source_t *, const struct audio_data *data, bool)
{
	Record(*static_cast<Capture *>(param), *data);
}

void OnMixAudio(void *param, size_t, struct audio_data *data)
{
	Record(*static_cast<Capture *>(param), *data);
}

Snapshot Take(Capture &capture)
{
	std::lock_guard<std::mutex> guard(capture.lock);
	return Snapshot{capture.packets, capture.samples, capture.channels, capture.overflowed};
}

void SetCapacity(Capture &capture, size_t samplesPerChannel)
{
	std::lock_guard<std::mutex> guard(capture.lock);
	capture.capacity = samplesPerChannel;
}

void PutLe(std::vector<unsigned char> &out, uint32_t value, int bytes)
{
	for (int i = 0; i < bytes; ++i) {
		out.push_back(static_cast<unsigned char>(value >> (8 * i)));
	}
}

// A 1.85 s mono notification-shaped clip at `rate`, written as 16-bit PCM WAV. The envelope
// follows a real alert sound -- attack over 0.2 s, then an exponential decay that leaves almost
// nothing after a second -- so most of its energy sits in exactly the stretch the defect drops.
// The carrier sweeps 300-900 Hz: a sweep makes the correlation peak unique, and staying low keeps
// a sub-sample misalignment from a resampler from reading as lost audio.
// `reference` receives the samples as the WAV holds them, quantization included.
std::vector<unsigned char> SynthesizeClip(uint32_t rate, std::vector<float> &reference)
{
	constexpr double kDurationSec = 1.85;
	constexpr double kAttackSec = 0.2;
	constexpr double kDecaySec = 0.22;
	constexpr double kFadeSec = 0.02;
	constexpr double kStartHz = 300.0;
	constexpr double kEndHz = 900.0;
	constexpr double kPeak = 0.7;
	constexpr double kPi = 3.14159265358979323846;

	const size_t count = size_t(kDurationSec * rate);
	std::vector<int16_t> pcm(count);
	reference.assign(count, 0.0f);
	for (size_t n = 0; n < count; ++n) {
		const double t = double(n) / rate;
		double env = t < kAttackSec ? t / kAttackSec : std::exp(-(t - kAttackSec) / kDecaySec);
		env *= std::min(1.0, (kDurationSec - t) / kFadeSec);
		const double phase = 2.0 * kPi * (kStartHz * t + (kEndHz - kStartHz) * t * t / (2.0 * kDurationSec));
		pcm[n] = int16_t(std::lround(kPeak * env * std::sin(phase) * 32767.0));
		reference[n] = float(pcm[n]) / 32768.0f;
	}

	std::vector<unsigned char> wav;
	const uint32_t dataBytes = uint32_t(count * sizeof(int16_t));
	wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
	PutLe(wav, 36 + dataBytes, 4);
	wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
	PutLe(wav, 16, 4);
	PutLe(wav, 1, 2); // PCM
	PutLe(wav, 1, 2); // mono
	PutLe(wav, rate, 4);
	PutLe(wav, rate * 2, 4);
	PutLe(wav, 2, 2);
	PutLe(wav, 16, 2);
	wav.insert(wav.end(), {'d', 'a', 't', 'a'});
	PutLe(wav, dataBytes, 4);
	const unsigned char *raw = reinterpret_cast<const unsigned char *>(pcm.data());
	wav.insert(wav.end(), raw, raw + dataBytes);
	return wav;
}

uint32_t GetLe(const std::vector<unsigned char> &in, size_t at, int bytes)
{
	uint32_t value = 0;
	for (int i = 0; i < bytes; ++i) {
		value |= uint32_t(in[at + i]) << (8 * i);
	}
	return value;
}

// The first channel of a PCM16 or float32 WAV. The rate must be the mix rate: the clip is
// compared sample for sample against what the mix receives.
bool ReadWavReference(const std::vector<unsigned char> &wav, uint32_t mixRate, std::vector<float> &reference,
		      std::string &error)
{
	if (wav.size() < 12 || std::memcmp(wav.data(), "RIFF", 4) != 0 || std::memcmp(wav.data() + 8, "WAVE", 4) != 0) {
		error = "not a RIFF/WAVE file";
		return false;
	}
	uint32_t format = 0, channels = 0, rate = 0, bits = 0;
	size_t dataAt = 0, dataBytes = 0;
	for (size_t at = 12; at + 8 <= wav.size();) {
		const uint32_t size = GetLe(wav, at + 4, 4);
		if (at + 8 + size > wav.size()) {
			break;
		}
		if (std::memcmp(wav.data() + at, "fmt ", 4) == 0 && size >= 16) {
			format = GetLe(wav, at + 8, 2);
			channels = GetLe(wav, at + 10, 2);
			rate = GetLe(wav, at + 12, 4);
			bits = GetLe(wav, at + 22, 2);
			if (format == 0xFFFE && size >= 26) {
				format = GetLe(wav, at + 32, 2); // WAVE_FORMAT_EXTENSIBLE's sub-format
			}
		} else if (std::memcmp(wav.data() + at, "data", 4) == 0) {
			dataAt = at + 8;
			dataBytes = size;
		}
		at += 8 + size + (size & 1);
	}
	const bool pcm16 = format == 1 && bits == 16;
	const bool float32 = format == 3 && bits == 32;
	if (!(pcm16 || float32) || channels == 0 || dataAt == 0) {
		error = "only 16-bit PCM and 32-bit float WAV are supported";
		return false;
	}
	if (rate != mixRate) {
		error = "the clip is " + std::to_string(rate) + " Hz and the mix is " + std::to_string(mixRate) +
			" Hz; resample it to the mix rate";
		return false;
	}
	const size_t stride = size_t(channels) * (bits / 8);
	reference.clear();
	for (size_t at = dataAt; at + stride <= dataAt + dataBytes; at += stride) {
		if (pcm16) {
			reference.push_back(float(int16_t(GetLe(wav, at, 2))) / 32768.0f);
		} else {
			const uint32_t word = GetLe(wav, at, 4);
			float value;
			std::memcpy(&value, &word, sizeof(value));
			reference.push_back(value);
		}
	}
	if (reference.empty()) {
		error = "no samples";
		return false;
	}
	return true;
}

// In-place iterative radix-2 FFT; `inverse` leaves the result unscaled.
void Fft(std::vector<std::complex<double>> &a, bool inverse)
{
	const size_t n = a.size();
	for (size_t i = 1, j = 0; i < n; ++i) {
		size_t bit = n >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			std::swap(a[i], a[j]);
		}
	}
	for (size_t len = 2; len <= n; len <<= 1) {
		const double angle = 2.0 * 3.14159265358979323846 / double(len) * (inverse ? 1.0 : -1.0);
		const std::complex<double> step(std::cos(angle), std::sin(angle));
		for (size_t i = 0; i < n; i += len) {
			std::complex<double> w(1.0);
			for (size_t k = 0; k < len / 2; ++k) {
				const std::complex<double> u = a[i + k];
				const std::complex<double> v = a[i + k + len / 2] * w;
				a[i + k] = u + v;
				a[i + k + len / 2] = u - v;
				w *= step;
			}
		}
	}
}

// The lag at which `reference` correlates best with `captured`, negative when the clip started
// before the window did. A capture that lost the onset still lines up, on the surviving tail.
long long BestLag(const std::vector<float> &captured, const std::vector<float> &reference)
{
	const size_t len = captured.size();
	const size_t refLen = reference.size();
	size_t n = 1;
	while (n < len + refLen) {
		n <<= 1;
	}
	std::vector<std::complex<double>> c(n), r(n);
	for (size_t i = 0; i < len; ++i) {
		c[i] = captured[i];
	}
	for (size_t i = 0; i < refLen; ++i) {
		r[i] = reference[i];
	}
	Fft(c, false);
	Fft(r, false);
	for (size_t i = 0; i < n; ++i) {
		c[i] *= std::conj(r[i]);
	}
	Fft(c, true);

	// c[m] is now n * sum_k captured[k + m] * reference[k], with a negative lag at n + lag.
	auto at = [&](long long lag) {
		return c[size_t(lag < 0 ? lag + (long long)n : lag)].real();
	};
	long long best = -(long long)refLen + 1;
	for (long long lag = best; lag < (long long)len; ++lag) {
		if (at(lag) > at(best)) {
			best = lag;
		}
	}
	return best;
}

struct Alignment {
	double gain = 0.0;
	double retained = 0.0;
	double onsetRetained = 0.0;
	std::string missing;
};

// Score the clip against `captured` at alignment `lag`, block by block. A block arrived when its
// waveform matches the clip's there, and is credited with its share of the clip's energy; a block
// that did not -- silence where the clip had sound -- is credited nothing. Arrival is judged on
// waveform, not level, because the monitor path runs through the endpoint's own processing: on the
// development machine's speakers that compresses the onset and lifts the tail by a fixed curve
// (identical on every alert), which a level test would misread as loss. The path's gain is
// reported, not judged.
Alignment Score(const std::vector<float> &captured, const std::vector<float> &reference, long long lag, uint32_t rate)
{
	struct Block {
		double energy;
		double gain;
		bool arrived;
		bool onset;
	};
	std::vector<Block> blocks;
	const size_t blockLen = std::max<size_t>(1, size_t(kBlockSec * rate));
	const auto sampleAt = [&](size_t k) {
		const long long i = lag + (long long)k;
		return (i >= 0 && i < (long long)captured.size()) ? double(captured[size_t(i)]) : 0.0;
	};
	for (size_t start = 0; start < reference.size(); start += blockLen) {
		const size_t end = std::min(reference.size(), start + blockLen);
		double mean = 0.0;
		for (size_t k = start; k < end; ++k) {
			mean += sampleAt(k);
		}
		mean /= double(end - start);

		double refEnergy = 0.0, capEnergy = 0.0, cross = 0.0;
		for (size_t k = start; k < end; ++k) {
			const double c = sampleAt(k) - mean;
			refEnergy += double(reference[k]) * reference[k];
			capEnergy += c * c;
			cross += c * reference[k];
		}
		const double correlation = refEnergy > 0.0 && capEnergy > 0.0 ? cross / std::sqrt(refEnergy * capEnergy)
									      : 0.0;
		const double t = double(start) / rate;
		blocks.push_back(Block{refEnergy, refEnergy > 0.0 ? cross / refEnergy : 0.0,
				       correlation >= kMinBlockCorrelation, t >= kOnsetFromSec && t < kOnsetToSec});
	}

	// Weighted by the clip's energy, so the loud blocks set the reported level rather than a
	// decaying tail that loudness processing may have lifted many times over.
	Alignment out;
	std::vector<std::pair<double, double>> gains; // {gain, clip energy}
	double arrivedEnergy = 0.0;
	for (const Block &b : blocks) {
		if (b.arrived) {
			gains.emplace_back(b.gain, b.energy);
			arrivedEnergy += b.energy;
		}
	}
	if (gains.empty() || arrivedEnergy <= 0.0) {
		return out;
	}
	std::sort(gains.begin(), gains.end());
	double below = 0.0;
	for (const auto &[gain, energy] : gains) {
		below += energy;
		if (below >= arrivedEnergy / 2.0) {
			out.gain = gain;
			break;
		}
	}

	double total = 0.0, credited = 0.0, onsetTotal = 0.0, onsetCredited = 0.0;
	for (const Block &b : blocks) {
		const double arrived = b.arrived ? b.energy : 0.0;
		total += b.energy;
		credited += arrived;
		if (b.onset) {
			onsetTotal += b.energy;
			onsetCredited += arrived;
		}
	}
	out.retained = total > 0.0 ? credited / total : 0.0;
	out.onsetRetained = onsetTotal > 0.0 ? onsetCredited / onsetTotal : 0.0;

	// Where the loss is, so a failure says which part of the clip went: runs of blocks that did
	// not arrive, listed when they held at least half a percent of the clip's energy.
	for (size_t i = 0; i < blocks.size();) {
		if (blocks[i].arrived) {
			++i;
			continue;
		}
		size_t end = i;
		double lost = 0.0;
		while (end < blocks.size() && !blocks[end].arrived) {
			lost += blocks[end].energy;
			++end;
		}
		if (total > 0.0 && lost / total >= 0.005) {
			out.missing += (out.missing.empty() ? "" : ",") + std::to_string(i * blockLen * 1000 / rate) +
				       "-" + std::to_string(end * blockLen * 1000 / rate) + "ms(" +
				       std::to_string(int(std::lround(lost / total * 100.0))) + "%)";
		}
		i = end;
	}
	return out;
}

// The packets that arrived in [fromNs, toNs), as index bounds into snap.packets.
std::pair<size_t, size_t> Window(const Snapshot &snap, uint64_t fromNs, uint64_t toNs)
{
	size_t first = 0;
	while (first < snap.packets.size() && snap.packets[first].arrivalNs < fromNs) {
		++first;
	}
	size_t last = first;
	while (last < snap.packets.size() && snap.packets[last].arrivalNs < toNs) {
		++last;
	}
	return {first, last};
}

SourceStats MeasureSource(const Snapshot &snap, uint64_t fromNs, uint64_t toNs, uint32_t rate)
{
	SourceStats stats;
	const auto [first, last] = Window(snap, fromNs, toNs);
	for (size_t i = first; i < last; ++i) {
		const Packet &p = snap.packets[i];
		if (i == first) {
			stats.firstPacketMs = double(p.arrivalNs - fromNs) / 1e6;
		} else {
			const Packet &prev = snap.packets[i - 1];
			const double expectedNs = double(prev.timestampNs) + double(prev.frames) * 1e9 / rate;
			stats.maxHoleMs = std::max(stats.maxHoleMs, (double(p.timestampNs) - expectedNs) / 1e6);
		}
		++stats.packets;
	}
	return stats;
}

MixScore MeasureMix(const Snapshot &snap, uint64_t fromNs, uint64_t toNs, const std::vector<float> &reference,
		    uint32_t rate)
{
	MixScore score;
	const auto [first, last] = Window(snap, fromNs, std::min(toNs, fromNs + kRecordNs));
	if (first == last || snap.packets[first].offset == kNoSamples || snap.packets[last - 1].offset == kNoSamples) {
		return score;
	}
	const size_t begin = snap.packets[first].offset;
	const size_t end = snap.packets[last - 1].offset + snap.packets[last - 1].frames;
	const std::vector<float> &scored = snap.samples[0];
	const std::vector<float> window(scored.begin() + begin, scored.begin() + end);

	const long long lag = BestLag(window, reference);
	const Alignment a = Score(window, reference, lag, rate);
	score.gain = a.gain;
	score.retained = a.retained;
	score.onsetRetained = a.onsetRetained;
	score.missing = a.missing;

	// The mix block that holds the clip's first sample, by when it left the mixer.
	const size_t onset = begin + size_t(std::max<long long>(0, lag));
	for (size_t i = first; i < last; ++i) {
		const Packet &p = snap.packets[i];
		if (onset < p.offset + p.frames) {
			score.latencyMs = double(p.arrivalNs - fromNs) / 1e6;
			break;
		}
	}

	// Mean square with the DC left in: whatever the keepalive's subtraction leaves is a DC level,
	// and that is what is measured.
	const size_t quietFrom = begin + size_t(kFloorAfterSec * rate);
	for (size_t c = 0; quietFrom < end && c < snap.channels; ++c) {
		const std::vector<float> &channel = snap.samples[c];
		double sum = 0.0, squares = 0.0;
		for (size_t i = quietFrom; i < end; ++i) {
			sum += channel[i];
			squares += double(channel[i]) * channel[i];
		}
		const double count = double(end - quietFrom);
		const double meanSquare = squares / count;
		const double mean = std::fabs(sum / count);
		score.floorDbfs[c] = meanSquare > 0.0 ? 10.0 * std::log10(meanSquare) : -200.0;
		// The DC part of that level on its own, so a floor over the bound can be told apart: DC is
		// the keepalive surviving its subtraction, the rest is something else on the track.
		score.floorDcDbfs[c] = mean > 0.0 ? 20.0 * std::log10(mean) : -200.0;
	}
	return score;
}

std::string Fixed(double value, int digits)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "%.*f", digits, value);
	return buf;
}

// The loudest measured channel's floor: every channel the keepalive is subtracted from must be quiet.
double WorstFloorDbfs(const MixScore &score)
{
	return *std::max_element(score.floorDbfs.begin(), score.floorDbfs.end());
}

// "a/b" across the measured channels, for the log line.
std::string PerChannel(const std::array<double, kMeasuredChannels> &values)
{
	std::string out;
	for (size_t c = 0; c < values.size(); ++c) {
		out += (c ? "/" : "") + Fixed(values[c], 1);
	}
	return out;
}

bool AlertPasses(const AlertResult &a)
{
	return a.mix.retained >= kMinRetained && a.mix.onsetRetained >= kMinRetained && a.mix.latencyMs >= 0.0 &&
	       a.mix.latencyMs <= kMaxLatencyMs && WorstFloorDbfs(a.mix) <= kMaxFloorDbfs;
}

void WriteSummary(const State &st)
{
	const int exitCode = st.exitCode < 0 ? 3 : st.exitCode;
	json alerts = json::array();
	for (const AlertResult &a : st.alerts) {
		alerts.push_back(json{
			{"delivered", a.call ? Delivered(*a.call) : 0},
			{"sourcePackets", a.source.packets},
			{"sourceFirstPacketMs", a.source.firstPacketMs},
			{"sourceMaxHoleMs", a.source.maxHoleMs},
			{"latencyMs", a.mix.latencyMs},
			{"gain", a.mix.gain},
			{"retained", a.mix.retained},
			{"onsetRetained", a.mix.onsetRetained},
			{"floorDbfsByChannel", a.mix.floorDbfs},
			{"floorDcDbfsByChannel", a.mix.floorDcDbfs},
			{"missing", a.mix.missing},
			{"pass", AlertPasses(a)},
		});
	}
	const json summary{
		{"result", SelfTest::ResultName(exitCode)},
		{"exitCode", exitCode},
		{"reason", st.reason},
		{"variant", st.monitorVariant ? "monitor" : "output"},
		{"endpoint", st.endpoint},
		{"clip", st.clipOrigin},
		{"clipSamples", st.reference.size()},
		{"mixTrack", kTrack + 1},
		{"measured", st.measured},
		{"mixRateHz", st.mixRateHz},
		{"mixChannels", st.mixChannels},
		{"minRetained", kMinRetained},
		{"maxLatencyMs", kMaxLatencyMs},
		{"maxFloorDbfs", kMaxFloorDbfs},
		{"uiStallMs", st.uiStallMs},
		{"spacingOverrideMs", st.spacingOverrideMs},
		{"alerts", alerts},
		{"elapsedSec", st.elapsedSec},
		{"sourceFrames", st.sourceFrames},
		{"impliedRateHz", st.impliedRateHz},
		{"rateDeficitPct", st.rateDeficitPct},
		{"rateDeficitThresholdPct", SelfTest::kMaxRateDeficitPct},
		{"jumpsBeforeFiring", st.jumpsBeforeFiring},
		{"tsJumps", st.tsJumps},
		{"smoothingMisses", st.smoothingMisses},
	};
	const std::string path = SelfTest::WriteSummaryFile("overlay-audio", summary.dump(2));
	HostLog(std::string(kLogTag) + " summary=" + (path.empty() ? "(unwritten)" : path));
}

void Teardown(State &st)
{
	if (st.mixTapped) {
		obs_remove_raw_audio_callback(kTrack, OnMixAudio, &g_mix);
		st.mixTapped = false;
	}
	if (st.outputTapped && st.source) {
		// Returns with no callback running and none to follow.
		obs_source_remove_audio_capture_callback(st.source, OnSourceAudio, &g_output);
		st.outputTapped = false;
	}
	if (st.scene) {
		obs_set_output_source(ObsBootstrap::kSelfTestOutputChannel, st.priorChannelSource);
		obs_scene_release(st.scene);
		st.scene = nullptr;
	}
	if (st.priorChannelSource) {
		obs_source_release(st.priorChannelSource);
		st.priorChannelSource = nullptr;
	}
	if (st.loopback) {
		obs_source_release(st.loopback);
		st.loopback = nullptr;
	}
	if (st.source) {
		obs_source_set_monitoring_type(st.source, OBS_MONITORING_TYPE_NONE);
		obs_source_release(st.source);
		st.source = nullptr;
	}
	if (st.monitorDeviceSwapped) {
		obs_set_audio_monitoring_device(st.priorMonitorName.c_str(), st.priorMonitorId.c_str());
		st.monitorDeviceSwapped = false;
	}
	for (const auto &[weak, mixers] : st.maskedSources) {
		OBSSource source = OBSGetStrongRef(weak);
		if (source) {
			obs_source_set_audio_mixers(source, mixers);
		}
	}
	st.maskedSources.clear();
	if (!st.widgetId.empty()) {
		Overlay::Store().Delete(st.widgetId);
		st.widgetId.clear();
	}
}

void Bail(State &st, int exitCode, const std::string &reason)
{
	st.exitCode = exitCode;
	st.reason = reason;
	HostLog(std::string(kLogTag) + " " + SelfTest::ResultName(exitCode) + " " + reason);
	st.phase = Phase::Teardown;
}

bool LoadClip(State &st)
{
	const std::string clipPath = Env::Value("BRAIDCAST_SELFTEST_CLIP");
	if (clipPath.empty()) {
		st.clipFile = SynthesizeClip(st.mixRateHz, st.reference);
		st.clipOrigin = "synthesized";
		return true;
	}
	std::ifstream in(clipPath, std::ios::binary);
	std::string error;
	if (!in) {
		error = "unreadable";
	} else {
		st.clipFile.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		ReadWavReference(st.clipFile, st.mixRateHz, st.reference, error);
	}
	if (!error.empty()) {
		Bail(st, 3, "BRAIDCAST_SELFTEST_CLIP '" + clipPath + "': " + error);
		return false;
	}
	st.clipOrigin = clipPath;
	return true;
}

// Empty the measured track of everything but this case. Collected first and changed after, so
// no mixer signal fires while libobs's source list is locked.
void MaskTrack(State &st)
{
	std::vector<OBSSource> feeding;
	obs_enum_all_sources(
		[](void *param, obs_source_t *source) {
			if (obs_source_get_audio_mixers(source) & kTrackBit) {
				static_cast<std::vector<OBSSource> *>(param)->emplace_back(source);
			}
			return true;
		},
		&feeding);
	for (const OBSSource &source : feeding) {
		const uint32_t mixers = obs_source_get_audio_mixers(source);
		st.maskedSources.emplace_back(OBSGetWeakRef(source), mixers);
		obs_source_set_audio_mixers(source, mixers & ~kTrackBit);
	}
}

// Monitor variant: send the overlay's monitor to a named endpoint and put a Desktop Audio capture
// of that same endpoint on the measured track -- the chain a viewer hears when an overlay is
// monitored to the device Desktop Audio captures, where libobs's deduplication silences the
// overlay's own output and the mix carries the monitor's rendition instead.
bool SetUpMonitorPath(State &st)
{
	// The clips are audible on this endpoint while the case runs, and its capture hears
	// everything else playing there too, so the operator names one they know is quiet.
	const ObsBootstrap::SelfTestEndpoint endpoint = ObsBootstrap::ResolveSelfTestEndpoint(kLogTag);
	if (endpoint.id.empty()) {
		Bail(st, endpoint.exitCode, endpoint.reason);
		return false;
	}
	st.endpoint = endpoint.id;

	const char *priorName = nullptr;
	const char *priorId = nullptr;
	obs_get_audio_monitoring_device(&priorName, &priorId);
	st.priorMonitorName = priorName ? priorName : "";
	st.priorMonitorId = priorId ? priorId : "";
	if (!obs_set_audio_monitoring_device(st.endpoint.c_str(), st.endpoint.c_str())) {
		Bail(st, 3, "could not set the monitoring device to " + st.endpoint);
		return false;
	}
	st.monitorDeviceSwapped = true;

	// Created after the device swap, so its creation is what registers it as the source that
	// duplicates the monitor.
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "device_id", st.endpoint.c_str());
	st.loopback = obs_source_create_private("wasapi_output_capture", kLoopbackName, settings);
	if (!st.loopback) {
		Bail(st, 3, "wasapi_output_capture create failed");
		return false;
	}
	obs_source_set_audio_mixers(st.loopback, kTrackBit);
	return true;
}

// Build the widget, its sound, the rerouted source and (monitor variant) the endpoint chain, all
// in one private scene on the self-test channel; false (after a Bail) if any step failed.
bool SetUp(State &st)
{
	struct obs_audio_info oai = {};
	if (!obs_get_audio_info(&oai) || oai.samples_per_sec == 0) {
		Bail(st, 3, "the audio mix sample rate could not be read");
		return false;
	}
	st.mixRateHz = oai.samples_per_sec;
	SetCapacity(g_output, size_t(kMaxCapturedSec) * st.mixRateHz);
	SetCapacity(g_mix, size_t(kMaxCapturedSec) * st.mixRateHz);
	if (!LoadClip(st)) {
		return false;
	}
	MaskTrack(st);
	if (st.monitorVariant && !SetUpMonitorPath(st)) {
		return false;
	}

	const std::optional<Overlay::Widget> widget = Overlay::Store().Create(kWidgetName, "alertbox");
	if (!widget) {
		Bail(st, 3, "could not create the alertbox widget");
		return false;
	}
	st.widgetId = widget->id;
	const std::string soundPath = Overlay::Store().AddAsset(st.widgetId, kClipAssetKey, "sound", st.clipFile);
	if (soundPath.empty()) {
		Bail(st, 3, "could not store the clip as the widget's sound");
		return false;
	}
	// Two-second alerts, so the card is long gone by the next one and every alert plays at once.
	const json patch{{"settings", {{"sound", soundPath}, {"duration", 2}}}};
	if (Overlay::Store().Update(st.widgetId, patch) != Overlay::MutateResult::Ok) {
		Bail(st, 3, "could not set the widget's sound");
		return false;
	}

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, Overlay::kOverlayIdKey, st.widgetId.c_str());
	obs_data_set_bool(settings, Overlay::kRerouteAudioKey, true);
	st.source = obs_source_create_private(Overlay::kOverlaySourceId, kSourceName, settings);
	if (!st.source) {
		Bail(st, 3, std::string(Overlay::kOverlaySourceId) + " create failed (obs-browser not loaded?)");
		return false;
	}
	obs_source_set_audio_mixers(st.source, kTrackBit);
	if (st.monitorVariant) {
		obs_source_set_monitoring_type(st.source, OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
	}
	obs_source_add_audio_capture_callback(st.source, OnSourceAudio, &g_output);
	st.outputTapped = true;

	// A scene on an output channel, so every source in it is active and showing as on a live
	// scene, and all of it feeds the mix.
	st.scene = obs_scene_create_private(kSceneName);
	obs_scene_add(st.scene, st.source);
	if (st.loopback) {
		obs_scene_add(st.scene, st.loopback);
	}
	st.priorChannelSource = obs_get_output_source(ObsBootstrap::kSelfTestOutputChannel);
	obs_set_output_source(ObsBootstrap::kSelfTestOutputChannel, obs_scene_get_source(st.scene));

	HostLog(std::string(kLogTag) +
		" up: variant=" + std::string(st.monitorVariant ? "monitor endpoint=" + st.endpoint : "output") +
		" clip=" + st.clipOrigin + " samples=" + std::to_string(st.reference.size()) +
		" mixRateHz=" + std::to_string(st.mixRateHz) + " track=" + std::to_string(kTrack + 1) +
		" masked=" + std::to_string(st.maskedSources.size()));
	return true;
}

// The gap between alert `index` and the next, in milliseconds.
int GapMs(const State &st, int index)
{
	return st.spacingOverrideMs > 0 ? st.spacingOverrideMs : kSpacingMs[index % std::size(kSpacingMs)];
}

// Whether the gap after the latest fire has run out. Measured from that fire's own time rather
// than from a schedule fixed at the first one: the tick that notices a fire is due can land up to
// one timer period late, and against a fixed schedule that lateness would come out of the next
// gap, pushing it below its value and, at the shortest gap, under the recording window.
bool GapElapsed(const State &st)
{
	return st.alerts.empty() ||
	       os_gettime_ns() - st.alerts.back().fireNs >= uint64_t(GapMs(st, st.fired - 1)) * 1000000;
}

void Judge(State &st)
{
	const Snapshot output = Take(g_output);
	const Snapshot mix = Take(g_mix);
	const uint64_t now = os_gettime_ns();
	st.mixChannels = mix.channels;

	bool alertsPass = true;
	for (size_t i = 0; i < st.alerts.size(); ++i) {
		AlertResult &a = st.alerts[i];
		const uint64_t until = i + 1 < st.alerts.size() ? st.alerts[i + 1].fireNs : now;
		a.source = MeasureSource(output, a.fireNs, until, st.mixRateHz);
		a.mix = MeasureMix(mix, a.fireNs, until, st.reference, st.mixRateHz);
		const bool pass = AlertPasses(a);
		alertsPass = alertsPass && pass;
		st.maxFloorDbfs = std::max(st.maxFloorDbfs, WorstFloorDbfs(a.mix));
		HostLog(std::string(kLogTag) + " alert " + std::to_string(i + 1) + ": retained=" +
			Fixed(a.mix.retained * 100.0, 2) + "% onsetRetained=" + Fixed(a.mix.onsetRetained * 100.0, 2) +
			"% gain=" + Fixed(a.mix.gain, 3) + " latencyMs=" + Fixed(a.mix.latencyMs, 1) +
			" floorDbfs=" + PerChannel(a.mix.floorDbfs) + " floorDcDbfs=" + PerChannel(a.mix.floorDcDbfs) +
			" missing=" + (a.mix.missing.empty() ? "none" : a.mix.missing) + " sourcePackets=" +
			std::to_string(a.source.packets) + " sourceFirstPacketMs=" + Fixed(a.source.firstPacketMs, 1) +
			" sourceMaxHoleMs=" + Fixed(a.source.maxHoleMs, 1) + " " + SelfTest::ResultName(pass ? 0 : 1));
	}

	// The source's own packets since firing began, held against wall clock: an unfixed capture is
	// down between alerts, a fixed one never is.
	const auto [first, last] = Window(output, st.firingStartNs, now);
	for (size_t i = first; i < last; ++i) {
		st.sourceFrames += output.packets[i].frames;
	}
	st.measured = true;
	st.elapsedSec = double(now - st.firingStartNs) / 1e9;
	st.impliedRateHz = st.elapsedSec > 0.0 ? double(st.sourceFrames) / st.elapsedSec : 0.0;
	st.rateDeficitPct = (1.0 - st.impliedRateHz / double(st.mixRateHz)) * 100.0;

	const int jumps = SelfTest::CountSessionLogLines(kJumpNeedle);
	st.tsJumps = jumps < 0 || st.jumpsBeforeFiring < 0 ? -1 : jumps - st.jumpsBeforeFiring;
	st.smoothingMisses = SelfTest::CountSessionLogLines(kSmoothingNeedle);

	if (st.tsJumps < 0 || st.smoothingMisses < 0) {
		st.exitCode = 3;
		st.reason = "this session's log could not be read back";
	} else if (output.overflowed || mix.overflowed) {
		st.exitCode = 3;
		st.reason = "a capture outgrew its buffer, so there is nothing whole to judge";
	} else if (mix.packets.empty()) {
		st.exitCode = 3;
		st.reason = "the mix track delivered nothing";
	} else {
		st.exitCode = (alertsPass && st.tsJumps == 0 && st.rateDeficitPct <= SelfTest::kMaxRateDeficitPct) ? 0
														   : 1;
	}

	HostLog(std::string(kLogTag) + " " + SelfTest::ResultName(st.exitCode) + " variant=" +
		(st.monitorVariant ? "monitor" : "output") + " alerts=" + std::to_string(st.alerts.size()) +
		" tsJumps=" + std::to_string(st.tsJumps) + " (before firing " + std::to_string(st.jumpsBeforeFiring) +
		") smoothingMisses=" + std::to_string(st.smoothingMisses) +
		" maxFloorDbfs=" + Fixed(st.maxFloorDbfs, 1) + " elapsedSec=" + Fixed(st.elapsedSec, 2) +
		" sourceFrames=" + std::to_string(st.sourceFrames) + " impliedRateHz=" + Fixed(st.impliedRateHz, 1) +
		" mixRateHz=" + std::to_string(st.mixRateHz) + " mixChannels=" + std::to_string(st.mixChannels) +
		" rateDeficitPct=" + Fixed(st.rateDeficitPct, 3) + " clip=" + st.clipOrigin +
		(st.reason.empty() ? "" : " reason=" + st.reason));
}

void Arm(bool monitorVariant)
{
	g_state = State{};
	g_output.Reset();
	g_mix.Reset();
	g_state.monitorVariant = monitorVariant;
	const long requestedAlerts = Env::Number("BRAIDCAST_SELFTEST_ALERTS", kMinAlerts);
	g_state.alertCount = int(std::clamp<long>(requestedAlerts, kMinAlerts, kMaxAlerts));
	if (requestedAlerts > kMaxAlerts) {
		HostLog(std::string(kLogTag) + " BRAIDCAST_SELFTEST_ALERTS=" + std::to_string(requestedAlerts) +
			" clamped to " + std::to_string(kMaxAlerts) + ", as many as " +
			std::to_string(kMaxCapturedSec) + " s of capture holds");
	}
	const long spacing = Env::Number("BRAIDCAST_SELFTEST_SPACING_MS", 0);
	g_state.spacingOverrideMs = spacing > 0 ? std::max<int>(kMinSpacingMs, int(spacing)) : 0;
	g_state.uiStallMs = std::max<int>(0, int(Env::Number("BRAIDCAST_SELFTEST_UI_STALL_MS", kDefaultUiStallMs)));
	g_state.phase = Phase::BootSettle;
	g_state.phaseStart = std::chrono::steady_clock::now();
	HostLog(std::string(kLogTag) + " armed: variant=" + (monitorVariant ? "monitor" : "output") +
		" alerts=" + std::to_string(g_state.alertCount) +
		" spacingMs=" + (g_state.spacingOverrideMs > 0 ? std::to_string(g_state.spacingOverrideMs) : "varied") +
		" uiStallMs=" + std::to_string(g_state.uiStallMs));
}

} // namespace

void ObsBootstrap::ArmOverlayAudioCaptureSelfTest(HWND)
{
	Arm(false);
}

void ObsBootstrap::ArmOverlayAudioMonitorSelfTest(HWND)
{
	Arm(true);
}

bool ObsBootstrap::RunOverlayAudioCaptureSelfTest()
{
	State &st = g_state;
	const auto elapsed = [&st] {
		return std::chrono::steady_clock::now() - st.phaseStart;
	};
	const auto enter = [&st](Phase next) {
		st.phase = next;
		st.phaseStart = std::chrono::steady_clock::now();
	};

	for (;;) {
		switch (st.phase) {
		case Phase::Idle:
			return true; // never armed

		case Phase::BootSettle:
			if (elapsed() < SelfTest::kBootSettle) {
				return false;
			}
			enter(Phase::Setup);
			continue;

		case Phase::Setup:
			if (SetUp(st)) {
				enter(Phase::AwaitPage);
			}
			continue;

		case Phase::AwaitPage: {
			// A named-channel test frame an alertbox ignores: it reports whether the page's
			// event socket is open without playing anything.
			if (st.probe && st.probe->done && Delivered(*st.probe) > 0) {
				HostLog(std::string(kLogTag) + " page listening");
				enter(Phase::PageSettle);
				return false;
			}
			if (elapsed() >= kPageWait) {
				Bail(st, 3, "the overlay page never opened its event socket");
				continue;
			}
			if (!st.probe || st.probe->done) {
				st.probe =
					CallAsync("overlays.test", json{{"id", st.widgetId}, {"channel", "viewers"}});
			}
			return false;
		}

		case Phase::PageSettle:
			if (elapsed() < kPageSettle) {
				return false;
			}
			// Warm-up ends here: jumps already logged belong to the page coming up, and the mix
			// is tapped from now on.
			st.jumpsBeforeFiring = SelfTest::CountSessionLogLines(kJumpNeedle);
			st.firingStartNs = os_gettime_ns();
			obs_add_raw_audio_callback(kTrack, nullptr, OnMixAudio, &g_mix);
			st.mixTapped = true;
			enter(Phase::Firing);
			continue;

		case Phase::Firing:
			if (st.fired < st.alertCount && GapElapsed(st)) {
				AlertResult alert;
				alert.fireNs = os_gettime_ns();
				g_output.recordUntilNs = alert.fireNs + kRecordNs;
				g_mix.recordUntilNs = alert.fireNs + kRecordNs;
				alert.call = CallAsync("overlays.test", json{{"id", st.widgetId}, {"type", "follow"}});
				st.alerts.push_back(std::move(alert));
				++st.fired;
				HostLog(std::string(kLogTag) + " alert " + std::to_string(st.fired) + " fired");
				if (st.uiStallMs > 0) {
					Sleep(DWORD(st.uiStallMs));
				}
			}
			if (st.fired == st.alertCount && GapElapsed(st)) {
				enter(Phase::Drain);
				continue;
			}
			return false;

		case Phase::Drain: {
			const bool answered = std::all_of(st.alerts.begin(), st.alerts.end(),
							  [](const AlertResult &a) { return a.call->done; });
			if (!answered && elapsed() < kPageWait) {
				return false;
			}
			enter(Phase::Verdict);
			for (size_t i = 0; i < st.alerts.size(); ++i) {
				const CallResult &call = *st.alerts[i].call;
				if (Delivered(call) != 1) {
					Bail(st, 3,
					     "alert " + std::to_string(i + 1) + " did not reach the page (" +
						     (call.error.empty() ? "delivered=0" : call.error) + ")");
					break;
				}
			}
			continue;
		}

		case Phase::Verdict:
			Judge(st);
			st.phase = Phase::Teardown;
			continue;

		case Phase::Teardown:
			Teardown(st);
			WriteSummary(st);
			st.phase = Phase::Finished;
			continue;

		case Phase::Finished:
			return true;
		}
	}
}

int ObsBootstrap::OverlayAudioCaptureSelfTestExitCode()
{
	return g_state.exitCode < 0 ? 3 : g_state.exitCode;
}
