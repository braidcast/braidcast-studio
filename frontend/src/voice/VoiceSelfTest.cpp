#include "obs_bootstrap.hpp"

#include "bridge.hpp"
#include "log.hpp"
#include "multistream/GlobalAudioChannels.hpp"
#include "multistream/StorePaths.hpp"
#include "util/file_util.hpp"
#include "util/async_task.hpp"
#include "util/env_config.hpp"
#include "util/http_client.hpp"
#include "util/paths.hpp"
#include "util/sha256.hpp"
#include "util/string_util.hpp"
#include "util/time_util.hpp"
#include "voice/FuzzyMatch.hpp"
#include "voice/MicMuteGuard.hpp"
#include "voice/Recognizer.hpp"
#include "voice/TextNormalize.hpp"
#include "voice/VoiceCapture.hpp"
#include "voice/VoiceCpu.hpp"
#include "voice/VoiceEngine.hpp"
#include "voice/VoiceListener.hpp"
#include "voice/VoiceModels.hpp"
#include "voice/VoiceResampler.hpp"
#include "voice/VoiceRing.hpp"
#include "voice/VoiceSettings.hpp"
#include "voice/WavFile.hpp"

#include <obs.hpp>
#include <whisper.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

// Voice control self-tests for the headless smoke path (main.cpp). One function per
// case, listed in kCases; each logs "[selftest] voice-<area> <case> -> OK|MISMATCH"
// and the run ends with one "voice overall" verdict. Transcripts are never logged.
namespace {

struct Tally {
	int failures = 0;

	void Check(const char *area, const std::string &name, bool ok)
	{
		HostLog(std::string("[selftest] voice-") + area + " " + name + " -> " + (ok ? "OK" : "MISMATCH"));
		if (!ok) {
			++failures;
		}
	}

	void Skip(const char *area, const std::string &name, const std::string &why)
	{
		HostLog(std::string("[selftest] voice-") + area + " " + name + " -> SKIPPED (" + why + ")");
	}
};

void TestWhisperLinked(Tally &t)
{
	t.Check("whisper", "version 1.9.4", std::string(whisper_version()) == "1.9.4");
}

void TestCpuGate(Tally &t)
{
	std::string reason;
	const bool ok = Voice::CpuSupportsVoice(reason);
	// PF_AVX2_INSTRUCTIONS_AVAILABLE (40) is missing from older SDK headers.
	const bool osAvx2 = IsProcessorFeaturePresent(40) != 0;
	t.Check("cpu", "gate agrees with the OS AVX2 report", ok == osAvx2);
	t.Check("cpu", "reason set exactly when unsupported", ok == reason.empty());
}

void TestLogCategory(Tally &t)
{
	const Log::DebugComponents c = Log::ParseComponents("voice");
	t.Check("log", "'voice' token selects only LogCat::Voice", c.logMask == Log::CatBit(LogCat::Voice));
	t.Check("log", "'basic' includes voice", (Log::kDefaultCats & Log::CatBit(LogCat::Voice)) != 0);
}

void TestVoiceSettingsTable(Tally &t)
{
	const auto &table = VoiceSettingsTable();

	VoiceSettings s;
	t.Check("settings", "defaults: disabled, P0 model", !s.enabled && s.model == Voice::P0::kDefaultModelId);

	std::string error;
	const bool ok = SettingsFields::ApplyPatch(
		table, nlohmann::json{{"enabled", true}, {"model", "tiny.en-q5_1"}, {"unknownKey", 1}}, s, error);
	t.Check("settings", "patch applies present keys, ignores unknown",
		ok && s.enabled && s.model == "tiny.en-q5_1");

	VoiceSettings before = s;
	const bool tooLong =
		SettingsFields::ApplyPatch(table, nlohmann::json{{"model", std::string(65, 'x')}}, s, error);
	t.Check("settings", "over-long string rejected, struct untouched",
		!tooLong && !error.empty() && s.model == before.model);

	const bool wrongType = SettingsFields::ApplyPatch(table, nlohmann::json{{"enabled", "yes"}}, s, error);
	t.Check("settings", "wrong JSON type ignored", wrongType && s.enabled);

	const nlohmann::json wire = SettingsFields::ToJson(table, s);
	t.Check("settings", "wire has every P1 key",
		wire.contains("enabled") && wire.contains("model") && wire.contains("logTranscripts"));

	OBSDataAutoRelease data = obs_data_create();
	SettingsFields::Save(table, data, s);
	VoiceSettings loaded;
	SettingsFields::Load(table, data, loaded);
	t.Check("settings", "obs_data round trip",
		loaded.enabled == s.enabled && loaded.model == s.model && loaded.logTranscripts == s.logTranscripts);
	t.Check("settings", "file key is snake_case", obs_data_has_user_value(data, "log_transcripts"));
}

void TestSha256(Tally &t)
{
	unsigned char d[32];
	t.Check("sha256", "abc vector",
		Sha256::Digest("abc", d) &&
			Sha256::ToHex(d) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	t.Check("sha256", "empty vector",
		Sha256::Digest("", d) &&
			Sha256::ToHex(d) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

	Sha256::Hasher h;
	h.Update("a", 1);
	h.Update("bc", 2);
	t.Check("sha256", "incremental equals one-shot",
		h.Ok() && h.Final(d) &&
			Sha256::ToHex(d) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

	const std::filesystem::path p = std::filesystem::temp_directory_path() / "braidcast-voice-sha.bin";
	{
		std::ofstream f(p, std::ios::binary);
		f << "abc";
	}
	std::string hex;
	t.Check("sha256", "file hash",
		Sha256::FileHex(p.u8string(), hex, nullptr) &&
			hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	std::atomic<bool> cancelled{true};
	t.Check("sha256", "file hash honours cancel", !Sha256::FileHex(p.u8string(), hex, &cancelled));
	std::error_code ec;
	std::filesystem::remove(p, ec);
}

void TestHttpCancel(Tally &t)
{
	// 10.255.255.1 is non-routable, so the connect hangs until the timeout. With the
	// cancel flag already set, the transfer-info poll must end it in about a second.
	std::atomic<bool> cancel{true};
	Http::HttpReq req;
	req.method = "GET";
	req.url = "http://10.255.255.1/voice-selftest";
	req.timeoutSec = 30;
	req.followRedirects = true;
	req.cancel = &cancel;
	std::string errorBody, error;
	const int64_t t0 = TimeUtil::NowMs();
	const long status = Http::HttpRequestStreaming(req, [](std::string_view) { return true; }, errorBody, error);
	const int64_t elapsed = TimeUtil::NowMs() - t0;
	t.Check("http", "cancel ends a stalled connect within 3 s", status == 0 && elapsed < 3000);
}

void TestModelCatalog(Tally &t)
{
	size_t n = 0;
	const Voice::ModelInfo *const *all = Voice::ModelCatalog(n);
	bool shapes = n >= 5;
	for (size_t i = 0; i < n; ++i) {
		const std::string sha = all[i]->sha256;
		shapes = shapes && sha.size() == 64 && sha.find_first_not_of("0123456789abcdef") == std::string::npos &&
			 all[i]->bytes > 0 && std::string(all[i]->url).rfind("https://", 0) == 0;
		for (size_t j = i + 1; j < n; ++j) {
			shapes = shapes && std::string(all[i]->id) != all[j]->id;
		}
	}
	t.Check("models", "catalog ids unique, hashes and urls well formed", shapes);
	const Voice::ModelInfo *vad = Voice::FindModel(Voice::kVadModelId);
	t.Check("models", "VAD model present and kind Vad", vad && vad->kind == Voice::ModelKind::Vad);
	t.Check("models", "P0 default is selectable", Voice::IsSelectableModel(Voice::P0::kDefaultModelId));
	t.Check("models", "multilingual is not in the English picker",
		!Voice::IsSelectableModel(Voice::kMultilingualModelId));
	t.Check("models", "unknown id rejected",
		Voice::FindModel("nope") == nullptr && !Voice::IsSelectableModel("nope"));
}

void TestModelVerifyAndCommit(Tally &t)
{
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / "braidcast-voice-models-selftest";
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::create_directories(dir, ec);
	const std::string abcSha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

	const fs::path part = dir / "m.bin.part";
	{
		std::ofstream f(part, std::ios::binary);
		f << "abc";
	}
	std::string error;
	t.Check("models", "verify accepts size and hash match",
		Voice::VerifyFile(part.u8string(), 3, abcSha.c_str(), error, nullptr));
	t.Check("models", "verify rejects size mismatch",
		!Voice::VerifyFile(part.u8string(), 4, abcSha.c_str(), error, nullptr) && !error.empty());
	t.Check("models", "verify rejects hash mismatch",
		!Voice::VerifyFile(part.u8string(), 3, std::string(64, '0').c_str(), error, nullptr));

	const fs::path final = dir / "m.bin";
	const bool committed = Voice::CommitDownload(part.u8string(), final.u8string(), abcSha, error);
	std::string marker;
	FileUtil::ReadUtf8File((final.u8string() + ".verified"), marker);
	t.Check("models", "commit renames .part and writes the marker",
		committed && fs::exists(final) && !fs::exists(part) && marker == abcSha);
	fs::remove_all(dir, ec);
}

// Ordering note: the delayed callback logs its own line after this whole self-test
// run has finished, so it appears in the log below the "voice overall" verdict.
void TestPostToUiDelayed(Tally &t)
{
	static std::atomic<bool> ran{false};
	ran.store(false, std::memory_order_release);
	AsyncTask::PostToUiDelayed(
		[] {
			ran.store(true, std::memory_order_release);
			HostLog("[selftest] voice-async delayed callback ran -> OK");
		},
		50);
	t.Check("async", "PostToUiDelayed defers even on the UI thread", !ran.load(std::memory_order_acquire));
}

void TestSpscRing(Tally &t)
{
	Voice::SpscRing ring(8);
	const float in[5] = {1.f, 2.f, 3.f, 4.f, 5.f};
	float out[8] = {};
	t.Check("ring", "write then read returns the same samples",
		ring.Write(in, 5) == 5 && ring.Available() == 5 && ring.Read(out, 5) == 5 && out[0] == 1.f &&
			out[4] == 5.f && ring.Available() == 0);

	// Capacity is 8, so the 9th sample of a 9-sample write is dropped and counted.
	const std::vector<float> big(9, 7.f);
	const size_t wrote = ring.Write(big.data(), big.size());
	t.Check("ring", "overflow drops the newest and counts it", wrote == 8 && ring.Dropped() == 1);
	ring.Reset();
	t.Check("ring", "reset empties the ring and the drop count", ring.Available() == 0 && ring.Dropped() == 0);

	// Wrap-around: write and read repeatedly across the buffer end.
	Voice::SpscRing wrapRing(4);
	bool wrapOk = true;
	float one = 0.f;
	for (int i = 0; i < 20; ++i) {
		const float v = static_cast<float>(i);
		wrapOk = wrapOk && wrapRing.Write(&v, 1) == 1 && wrapRing.Read(&one, 1) == 1 && one == v;
	}
	t.Check("ring", "wraps without losing order", wrapOk);

	// Stress: one producer thread, this thread consuming, checking the sequence.
	Voice::SpscRing stress(1024);
	constexpr int kTotal = 200000;
	std::thread producer([&] {
		int sent = 0;
		while (sent < kTotal) {
			const float v = static_cast<float>(sent % 1000);
			if (stress.Write(&v, 1) == 1) {
				++sent;
			}
		}
	});
	int got = 0;
	bool orderOk = true;
	while (got < kTotal) {
		float v = 0.f;
		if (stress.Read(&v, 1) == 1) {
			orderOk = orderOk && v == static_cast<float>(got % 1000);
			++got;
		}
	}
	producer.join();
	// A full ring refuses a write and counts it as dropped; the producer then retries, so
	// the count is not zero here. Order and completeness are what this case is about.
	t.Check("ring", "200k samples cross threads in order", orderOk);
}

// Fill `out` with `frames` samples of a sine at `hz`, sampled at `rate`.
void FillSine(std::vector<float> &out, size_t frames, double hz, double rate)
{
	out.resize(frames);
	for (size_t i = 0; i < frames; ++i) {
		out[i] = static_cast<float>(std::sin(6.283185307179586 * hz * static_cast<double>(i) / rate));
	}
}

double Rms(const float *p, size_t n)
{
	if (n == 0) {
		return 0.0;
	}
	double sum = 0.0;
	for (size_t i = 0; i < n; ++i) {
		sum += static_cast<double>(p[i]) * p[i];
	}
	return std::sqrt(sum / static_cast<double>(n));
}

// How much of a 16 kHz signal is a sine at `hz` (any phase), against everything else,
// in dB: a least-squares fit of sin and cos at that frequency over p[0..n).
double ToneSnrDb(const float *p, size_t n, double hz)
{
	double ss = 0.0, cc = 0.0, sc = 0.0, ys = 0.0, yc = 0.0, yy = 0.0;
	for (size_t i = 0; i < n; ++i) {
		const double phase = 6.283185307179586 * hz * static_cast<double>(i) / Voice::kVoiceSampleRate;
		const double s = std::sin(phase);
		const double c = std::cos(phase);
		ss += s * s;
		cc += c * c;
		sc += s * c;
		ys += p[i] * s;
		yc += p[i] * c;
		yy += static_cast<double>(p[i]) * p[i];
	}
	const double det = ss * cc - sc * sc;
	if (det <= 0.0) {
		return 0.0;
	}
	const double a = (ys * cc - yc * sc) / det;
	const double b = (yc * ss - ys * sc) / det;
	const double tone = a * ys + b * yc;
	return 10.0 * std::log10(tone / std::max(yy - tone, 1e-30));
}

// Run a whole buffer through a Downsampler in kMaxInFrames blocks.
std::vector<float> RunDownsampler(Voice::Downsampler &down, const std::vector<float> &in)
{
	std::vector<float> out(Voice::Downsampler::kMaxOutFrames);
	std::vector<float> all;
	size_t offset = 0;
	while (offset < in.size()) {
		const size_t chunk = std::min<size_t>(Voice::Downsampler::kMaxInFrames, in.size() - offset);
		const size_t n = down.Process(in.data() + offset, chunk, out.data());
		all.insert(all.end(), out.begin(), out.begin() + n);
		offset += chunk;
	}
	return all;
}

void TestResampler(Tally &t)
{
	std::vector<float> in;

	// A 1 kHz tone is inside the pass band, so it survives at about the same level.
	Voice::Downsampler down(48000);
	FillSine(in, 48000, 1000.0, 48000.0);
	const std::vector<float> got = RunDownsampler(down, in);
	t.Check("resample", "48k -> 16k frame count", got.size() >= 15980 && got.size() <= 16000);
	// Skip the filter's warm-up before measuring.
	t.Check("resample", "1 kHz passes at full level",
		std::fabs(Rms(got.data() + 128, got.size() - 128) - 0.7071) < 0.02);

	// 12 kHz is above the 8 kHz Nyquist of the output and must not alias back in.
	Voice::Downsampler alias(48000);
	FillSine(in, 48000, 12000.0, 48000.0);
	const std::vector<float> aliased = RunDownsampler(alias, in);
	t.Check("resample", "12 kHz is rejected, not aliased", Rms(aliased.data() + 128, aliased.size() - 128) < 0.01);

	// 44.1 kHz is the other common mix rate, and its ratio is not an integer.
	Voice::Downsampler odd(44100);
	FillSine(in, 44100, 1000.0, 44100.0);
	const std::vector<float> oddOut = RunDownsampler(odd, in);
	t.Check("resample", "44.1k -> 16k frame count", oddOut.size() >= 15980 && oddOut.size() <= 16010);
	// A fractional ratio is where a decimator that picks the nearest input sample jitters;
	// interpolating keeps the tone clean (measured 62 dB; the nearest pick gave 28 dB).
	t.Check("resample", "44.1k: 1 kHz stays a clean 1 kHz tone",
		oddOut.size() > 128 && ToneSnrDb(oddOut.data() + 128, oddOut.size() - 128, 1000.0) > 40.0);

	// 16 kHz in is a pass-through, sample for sample.
	Voice::Downsampler same(16000);
	FillSine(in, 1600, 1000.0, 16000.0);
	std::vector<float> out(Voice::Downsampler::kMaxOutFrames);
	const size_t n = same.Process(in.data(), 1600, out.data());
	t.Check("resample", "16k in is passed through unchanged", n == 1600 && out[10] == in[10]);

	// An over-long input block must be refused rather than overrun the output.
	Voice::Downsampler guard(48000);
	const std::vector<float> huge(Voice::Downsampler::kMaxInFrames + 1, 0.f);
	t.Check("resample", "over-long block refused", guard.Process(huge.data(), huge.size(), out.data()) == 0);
}

void TestMicMuteGuard(Tally &t)
{
	const uint32_t channel = GlobalAudio::PrimaryMicChannel();
	t.Check("mic", "primary mic channel is a mic/aux slot", channel >= 3 && channel <= 6);

	// A private source of our own, so the test never touches the user's devices.
	OBSSourceAutoRelease src =
		obs_source_create_private("wasapi_input_capture", "braidcast voice selftest mic", nullptr);
	if (!src) {
		t.Skip("mic", "guard restores the prior state", "no input capture source available");
		return;
	}
	obs_source_set_muted(src, false);

	Voice::MicMuteGuard guard;
	const bool ours = guard.Engage(src);
	t.Check("mic", "engage mutes an unmuted mic", obs_source_muted(src));
	t.Check("mic", "engage reports the mute as its own", ours);
	t.Check("mic", "persisted state stays unmuted while held", !GlobalAudio::PersistedMuteOverride(src, true));
	guard.Release();
	t.Check("mic", "release restores unmuted", !obs_source_muted(src));

	// The user unmutes mid-hold: release must leave their choice alone.
	obs_source_set_muted(src, false);
	guard.Engage(src);
	obs_source_set_muted(src, false);
	guard.Release();
	t.Check("mic", "release leaves a mic the user unmuted alone", !obs_source_muted(src));

	// Already muted before the guard: release must not unmute it.
	obs_source_set_muted(src, true);
	const bool userMute = !guard.Engage(src);
	t.Check("mic", "engage on an already muted mic is a no-op", obs_source_muted(src));
	t.Check("mic", "a mute the user made is not reported as the guard's", userMute);
	guard.Release();
	t.Check("mic", "release keeps a mic that was already muted", obs_source_muted(src));

	// Releasing without engaging, and double release, must both be harmless.
	guard.Release();
	t.Check("mic", "release without engage is harmless", obs_source_muted(src));
	t.Check("mic", "no override once released", GlobalAudio::PersistedMuteOverride(src, true));

	// A save that read "muted" during the hold but resolves it after the release (the
	// guard unmutes, then clears the override) must still store unmuted.
	obs_source_set_muted(src, false);
	guard.Engage(src);
	const uint64_t epoch = GlobalAudio::MuteOverrideEpoch();
	const bool snapshot = obs_source_muted(src);
	guard.Release();
	t.Check("mic", "a save straddling the release stores unmuted",
		!GlobalAudio::PersistedMuteForSnapshot(src, epoch, snapshot));
}

void TestVoiceCapture(Tally &t)
{
	Voice::SpscRing ring(Voice::kVoiceSampleRate * 4);
	Voice::VoiceCapture capture(ring);

	// Two planar channels of a 1 kHz sine at 48 kHz: 480 frames is one libobs block.
	std::vector<float> left;
	FillSine(left, 480, 1000.0, 48000.0);
	std::vector<float> right = left;
	const float *planes[2] = {left.data(), right.data()};

	capture.FeedForTest(48000, planes, 2, 480, false);
	t.Check("capture", "one 10 ms block yields about 160 frames at 16 kHz",
		ring.Available() >= 158 && ring.Available() <= 162);

	// The downmix is an average, so two identical channels keep the level.
	std::vector<float> got(ring.Available());
	ring.Read(got.data(), got.size());
	t.Check("capture", "identical channels keep their level", Rms(got.data(), got.size()) > 0.5);

	// A muted block still feeds audio (the mute is our own push-to-talk mute) but is
	// remembered, so the listener can refuse commands the user did not intend to speak.
	t.Check("capture", "muted starts false", !capture.TakeMutedSeen());
	capture.FeedForTest(48000, planes, 2, 480, true);
	t.Check("capture", "muted block sets the flag", capture.TakeMutedSeen());
	t.Check("capture", "taking the flag clears it", !capture.TakeMutedSeen());

	// A block longer than the resampler's limit must still be consumed whole.
	ring.Reset();
	std::vector<float> longBlock;
	FillSine(longBlock, 9000, 1000.0, 48000.0);
	const float *onePlane[1] = {longBlock.data()};
	capture.FeedForTest(48000, onePlane, 1, 9000, false);
	t.Check("capture", "a block over the resampler limit is chunked, not dropped",
		ring.Available() >= 2990 && ring.Available() <= 3010 && ring.Dropped() == 0);

	// A rate change rebuilds the resampler rather than resampling with stale state.
	ring.Reset();
	std::vector<float> at44;
	FillSine(at44, 441, 1000.0, 44100.0);
	const float *plane44[1] = {at44.data()};
	capture.FeedForTest(44100, plane44, 1, 441, false);
	t.Check("capture", "input rate change is picked up",
		capture.InputRateForTest() == 44100 && ring.Available() >= 158 && ring.Available() <= 162);

	// Binding a channel with no source is not an error; it simply stays unbound.
	t.Check("capture", "unbind without bind is harmless", !capture.Bound());
	capture.Unbind();
	t.Check("capture", "still unbound after a redundant unbind", !capture.Bound());
}

// Collect the effect types a Handle call produced, as a comma-joined string, so a
// case reads as one comparison.
std::string EffectNames(const std::vector<Voice::Effect> &effects)
{
	std::string out;
	for (const Voice::Effect &e : effects) {
		if (!out.empty()) {
			out += ",";
		}
		switch (e.type) {
		case Voice::EffectType::PlayCue:
			out += "cue:" + std::string(Voice::CueName(e.cue));
			break;
		case Voice::EffectType::Run:
			out += "run:" + e.action.commandId;
			break;
		case Voice::EffectType::ReadBack:
			out += "readback";
			break;
		case Voice::EffectType::ScheduleTick:
			out += "tick";
			break;
		}
	}
	return out;
}

Voice::Event Ev(Voice::EventType type, int64_t nowMs)
{
	Voice::Event e;
	e.type = type;
	e.nowMs = nowMs;
	return e;
}

Voice::Event Transcript(const std::string &text, int64_t nowMs)
{
	Voice::Event e = Ev(Voice::EventType::Transcript, nowMs);
	e.text = text;
	return e;
}

// An interpreter that reads the leading word of the transcript, so a case can ask for
// any outcome: "run ...", "confirm ...", "control cancel", "ignore ...", anything
// else is a miss.
Voice::Interpretation TestInterpret(const std::string &text, const Voice::InterpretContext &ctx)
{
	Voice::Interpretation out;
	if (text.rfind("run ", 0) == 0) {
		out.kind = Voice::Interpretation::Kind::Instant;
		out.action.commandId = text.substr(4);
		out.action.summary = "run " + out.action.commandId;
	} else if (text.rfind("confirm ", 0) == 0) {
		out.kind = Voice::Interpretation::Kind::Pending;
		out.action.commandId = text.substr(8);
		out.action.summary = "confirm " + out.action.commandId;
		out.action.needsConfirmWord = true;
	} else if (text == "yes" && ctx.pending) {
		out.kind = Voice::Interpretation::Kind::Control;
		out.control = Voice::Interpretation::Control::ConfirmPending;
	} else if (text == "never mind") {
		out.kind = Voice::Interpretation::Kind::Control;
		out.control = Voice::Interpretation::Control::CancelPending;
	} else if (text.rfind("ignore", 0) == 0) {
		out.kind = Voice::Interpretation::Kind::Ignored;
	} else {
		out.kind = Voice::Interpretation::Kind::Miss;
		out.message = "I did not catch a command.";
	}
	return out;
}

void TestVoiceListener(Tally &t)
{
	using namespace Voice;
	using S = Voice::State;

	auto fresh = [] {
		auto listener = std::make_unique<VoiceListener>(&TestInterpret);
		listener->Handle(Ev(EventType::Enable, 0));
		listener->Handle(Ev(EventType::ModelReady, 0));
		listener->Handle(Ev(EventType::MicBound, 0));
		return listener;
	};

	// 1. A fresh listener with no model is not ready, and the key does nothing.
	{
		VoiceListener l(&TestInterpret);
		l.Handle(Ev(EventType::Enable, 0));
		const std::string fx = EffectNames(l.Handle(Ev(EventType::PttDown, 10)));
		t.Check("listener", "push-to-talk before the model is ready does nothing",
			l.Current() == S::NotReady && fx.empty());
	}

	// 2. Disabled ignores everything, including a ready model.
	{
		VoiceListener l(&TestInterpret);
		l.Handle(Ev(EventType::ModelReady, 0));
		l.Handle(Ev(EventType::MicBound, 0));
		t.Check("listener", "disabled stays disabled with a ready model", l.Current() == S::Disabled);
	}

	// 3. The happy path: key down cues and listens, key up thinks, transcript runs.
	{
		auto l = fresh();
		const std::string down = EffectNames(l->Handle(Ev(EventType::PttDown, 100)));
		t.Check("listener", "key down plays the start cue and listens",
			down == "cue:start" && l->Current() == S::Listening);
		l->Handle(Ev(EventType::PttUp, 900));
		t.Check("listener", "key up moves to thinking", l->Current() == S::Thinking);
		const std::string done = EffectNames(l->Handle(Transcript("run scenes.setCurrent", 1400)));
		t.Check("listener", "a matched command runs and cues accept",
			done == "run:scenes.setCurrent,cue:accept" && l->Current() == S::Idle);
	}

	// 4. A miss cues reject and shows a message, and runs nothing.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		const std::string fx = EffectNames(l->Handle(Transcript("the weather is nice", 1400)));
		t.Check("listener", "an unmatched phrase cues reject and runs nothing",
			fx == "cue:reject" && l->Snapshot().message == "I did not catch a command." &&
				l->Current() == S::Idle);
	}

	// 5. An ignored phrase is silent: no cue, no message change.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		t.Check("listener", "an ignored phrase is silent",
			EffectNames(l->Handle(Transcript("ignore this", 1400))).empty() && l->Current() == S::Idle);
	}

	// 6. A dangerous command waits for confirmation, with a timeout scheduled.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		const std::string fx = EffectNames(l->Handle(Transcript("confirm streaming.stop", 1400)));
		t.Check("listener", "a confirmable command waits and schedules a timeout",
			fx == "cue:pending,tick" && l->Current() == S::Pending &&
				l->Snapshot().pending.commandId == "streaming.stop");
	}

	// 7. Confirming runs it.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		l->Handle(Transcript("confirm streaming.stop", 1400));
		l->Handle(Ev(EventType::PttDown, 2000));
		l->Handle(Ev(EventType::PttUp, 2400));
		t.Check("listener", "confirming runs the pending command",
			EffectNames(l->Handle(Transcript("yes", 2900))) == "run:streaming.stop,cue:accept" &&
				l->Current() == S::Idle);
	}

	// 8. Cancelling drops it without running.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		l->Handle(Transcript("confirm streaming.stop", 1400));
		l->Handle(Ev(EventType::PttDown, 2000));
		l->Handle(Ev(EventType::PttUp, 2400));
		t.Check("listener", "cancelling drops the pending command",
			EffectNames(l->Handle(Transcript("never mind", 2900))) == "cue:cancel" &&
				l->Current() == S::Idle && l->Snapshot().pending.commandId.empty());
	}

	// 9. The pending command expires on its own tick.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		const std::vector<Effect> fx = l->Handle(Transcript("confirm streaming.stop", 1400));
		Event tick = Ev(EventType::Tick, 1400 + VoiceListener::kPendingTimeoutMs);
		tick.seq = fx.back().seq;
		t.Check("listener", "a pending command expires",
			EffectNames(l->Handle(tick)) == "cue:cancel" && l->Current() == S::Idle);
	}

	// 10. A stale tick (from an older pending command) is ignored.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		const std::vector<Effect> fx = l->Handle(Transcript("confirm streaming.stop", 1400));
		Event stale = Ev(EventType::Tick, 9000);
		stale.seq = fx.back().seq - 1;
		t.Check("listener", "a stale tick is ignored", l->Handle(stale).empty() && l->Current() == S::Pending);
	}

	// 11. A second key press while thinking is refused, not queued.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		t.Check("listener", "a key press while thinking is refused",
			l->Handle(Ev(EventType::PttDown, 1000)).empty() && l->Current() == S::Thinking);
	}

	// 12. A key press the engine refused (PttDown with ok = false) does not listen.
	{
		auto l = fresh();
		Event down = Ev(EventType::PttDown, 100);
		down.ok = false;
		down.text = "The microphone is not available.";
		t.Check("listener", "a refused key press reports why and stays idle",
			EffectNames(l->Handle(down)) == "cue:reject" && l->Current() == S::Idle &&
				l->Snapshot().message == "The microphone is not available.");
	}

	// 13. Key up without key down is harmless.
	{
		auto l = fresh();
		t.Check("listener", "key up with no key down is harmless",
			l->Handle(Ev(EventType::PttUp, 100)).empty() && l->Current() == S::Idle);
	}

	// 14. A key held down and released with no transcript coming back still recovers.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		Event failed = Ev(EventType::TranscribeFailed, 1400);
		failed.text = "Recognition failed.";
		t.Check("listener", "a failed transcription returns to idle with a message",
			EffectNames(l->Handle(failed)) == "cue:reject" && l->Current() == S::Idle &&
				l->Snapshot().message == "Recognition failed.");
	}

	// 15. An empty transcript is a miss, not a crash.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		t.Check("listener", "an empty transcript is a miss",
			EffectNames(l->Handle(Transcript("", 1400))) == "cue:reject" && l->Current() == S::Idle);
	}

	// 16. Losing the microphone mid-listen ends the segment.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		t.Check("listener", "losing the mic while listening ends the segment",
			EffectNames(l->Handle(Ev(EventType::MicLost, 300))) == "cue:reject" &&
				l->Current() == S::NotReady);
	}

	// 17. Disabling mid-listen drops straight to disabled.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::Disable, 200));
		t.Check("listener", "disabling while listening stops everything",
			l->Current() == S::Disabled && l->Snapshot().pending.commandId.empty());
	}

	// 18. Disabling with a command pending drops the pending command too.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		l->Handle(Transcript("confirm streaming.stop", 1400));
		l->Handle(Ev(EventType::Disable, 1500));
		t.Check("listener", "disabling drops a pending command", l->Snapshot().pending.commandId.empty());
	}

	// 19. A model that fails to load reports why and is not ready.
	{
		VoiceListener l(&TestInterpret);
		l.Handle(Ev(EventType::Enable, 0));
		Event failed = Ev(EventType::ModelFailed, 10);
		failed.text = "Model file is missing.";
		l.Handle(failed);
		t.Check("listener", "a failed model load reports why",
			l.Current() == S::NotReady && l.Snapshot().message == "Model file is missing.");
	}

	// 20. An action result from a command that already finished updates the message
	// without changing state.
	{
		auto l = fresh();
		Event result = Ev(EventType::ActionResult, 3000);
		result.ok = false;
		result.text = "No scene called BRB.";
		t.Check("listener", "a failed action reports why without changing state",
			EffectNames(l->Handle(result)) == "cue:reject" && l->Current() == S::Idle &&
				l->Snapshot().message == "No scene called BRB.");
	}

	// 21. The cancel key drops a pending command, and is silent when idle.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		l->Handle(Transcript("confirm streaming.stop", 1400));
		t.Check("listener", "cancel drops a pending command",
			EffectNames(l->Handle(Ev(EventType::Cancel, 1600))) == "cue:cancel" &&
				l->Current() == S::Idle && l->Snapshot().pending.commandId.empty());
		t.Check("listener", "cancel with nothing in flight is silent",
			l->Handle(Ev(EventType::Cancel, 1700)).empty() && l->Current() == S::Idle);
	}

	// 22. The timeout still lands when the user has just pressed the key to answer: the
	// command expires, the segment carries on, and a late "yes" runs nothing.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		const std::vector<Effect> fx = l->Handle(Transcript("confirm streaming.stop", 1400));
		l->Handle(Ev(EventType::PttDown, 9300));
		Event tick = Ev(EventType::Tick, 1400 + VoiceListener::kPendingTimeoutMs);
		tick.seq = fx.back().seq;
		const std::string expired = EffectNames(l->Handle(tick));
		l->Handle(Ev(EventType::PttUp, 9600));
		t.Check("listener", "a timeout during the answering segment still expires the command",
			expired == "cue:cancel" && l->Snapshot().pending.commandId.empty() &&
				EffectNames(l->Handle(Transcript("yes", 9900))).find("run:") == std::string::npos);
	}

	// 23. Disabling forgets the model and the mic: the engine unloads and unbinds both,
	// so turning voice back on waits until they are reported again.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::Disable, 100));
		l->Handle(Ev(EventType::Enable, 200));
		t.Check("listener", "re-enabling waits for the model and the mic again",
			l->Current() == S::NotReady && l->Handle(Ev(EventType::PttDown, 300)).empty());
	}

	// 24. The engine refuses a press while the recognizer is still busy; that must not
	// abandon the segment whose transcript is on its way.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		Event busy = Ev(EventType::PttDown, 1000);
		busy.ok = false;
		busy.text = "Still working on the last command.";
		const bool quiet = l->Handle(busy).empty() && l->Current() == S::Thinking;
		t.Check("listener", "a refused press while thinking leaves the segment alone",
			quiet && EffectNames(l->Handle(Transcript("run scenes.setCurrent", 1400))) ==
					 "run:scenes.setCurrent,cue:accept");
	}

	// 25. The interpreter is told the user's mute and our push-to-talk mute apart.
	{
		InterpretContext seen;
		VoiceListener l([&seen](const std::string &, const InterpretContext &ctx) {
			seen = ctx;
			Interpretation out;
			out.kind = Interpretation::Kind::Shown;
			return out;
		});
		l.Handle(Ev(EventType::Enable, 0));
		l.Handle(Ev(EventType::ModelReady, 0));
		l.Handle(Ev(EventType::MicBound, 0));
		l.Handle(Ev(EventType::PttDown, 100));
		l.Handle(Ev(EventType::PttUp, 900));
		Event heard = Transcript("hello", 1400);
		heard.pttMuted = true;
		l.Handle(heard);
		const bool oursOnly = !seen.mutedSeen && seen.pttMuted;
		l.Handle(Ev(EventType::PttDown, 2000));
		l.Handle(Ev(EventType::PttUp, 2900));
		heard = Transcript("hello", 3400);
		heard.mutedSeen = true;
		l.Handle(heard);
		t.Check("listener", "the interpreter tells the user's mute from ours",
			oursOnly && seen.mutedSeen && !seen.pttMuted);
	}
}

std::string VoiceDataPath(const char *relative)
{
	// The rundir layout mirrors the installed one: <data>/braidcast/voice/<relative>.
	const std::string path = RundirRoot() + "/data/braidcast/voice/" + relative;
	std::error_code ec;
	return std::filesystem::exists(std::filesystem::u8path(path), ec) ? path : std::string();
}

void TestWavFile(Tally &t)
{
	const std::string path = VoiceDataPath("fixtures/switch-to-gameplay.wav");
	if (path.empty()) {
		t.Skip("wav", "fixture loads", "fixtures/switch-to-gameplay.wav not found in the data dir");
		return;
	}
	Voice::WavData wav;
	std::string error;
	const bool ok = Voice::LoadWavMono(path, wav, error);
	t.Check("wav", "fixture loads as 16 kHz mono", ok && wav.sampleRate == 16000 && wav.samples.size() > 12000);
	t.Check("wav", "fixture is not silence", ok && Rms(wav.samples.data(), wav.samples.size()) > 0.01);

	Voice::WavData missing;
	t.Check("wav", "a missing file reports an error",
		!Voice::LoadWavMono(path + ".nope", missing, error) && !error.empty());

	// A file that is not RIFF at all must be refused rather than read as audio.
	const std::filesystem::path junk = std::filesystem::temp_directory_path() / "braidcast-voice-junk.wav";
	{
		std::ofstream f(junk, std::ios::binary);
		f << "this is not a wav file at all, not even close";
	}
	t.Check("wav", "a non-RIFF file is refused", !Voice::LoadWavMono(junk.u8string(), missing, error));
	std::error_code ec;
	std::filesystem::remove(junk, ec);
}

// The parts of the recognizer that need no model, so they run in every smoke.
void TestRecognizerWithoutModel(Tally &t)
{
	t.Check("recognizer", "noise markers stripped",
		Voice::CleanTranscript(" [BLANK_AUDIO] switch to (wind blowing) gameplay\n") == "switch to gameplay");
	t.Check("recognizer", "nested and unbalanced markers stripped",
		Voice::CleanTranscript("[a [b] c] mute) mic (") == "mute mic");

	// One token per word, so the budget reads as a word count.
	const auto words = [](const std::string &text) {
		int n = 0;
		for (size_t i = 0; i < text.size(); ++i) {
			n += text[i] != ' ' && (i == 0 || text[i - 1] == ' ') ? 1 : 0;
		}
		return n;
	};
	t.Check("recognizer", "a prompt within budget is kept whole",
		Voice::TrimPromptFront("alpha beta gamma", 3, words) == "alpha beta gamma");
	t.Check("recognizer", "an over-long prompt keeps its tail",
		Voice::TrimPromptFront("alpha beta gamma delta", 2, words) == "gamma delta");

	// A model that is not there fails through the callback, and nothing else breaks.
	Voice::SpscRing ring(1024);
	Voice::Recognizer rec(ring, [] { return false; });
	std::mutex mutex;
	std::condition_variable cv;
	bool reported = false;
	bool loaded = true;
	std::string why;
	rec.SetModelCallback([&](bool ok, const std::string &error) {
		std::lock_guard<std::mutex> lock(mutex);
		reported = true;
		loaded = ok;
		why = error;
		cv.notify_all();
	});
	const std::filesystem::path missing = std::filesystem::temp_directory_path() / "braidcast-voice-no-model.bin";
	rec.Start(missing.u8string(), 1);
	{
		std::unique_lock<std::mutex> lock(mutex);
		cv.wait_for(lock, std::chrono::seconds(10), [&] { return reported; });
	}
	t.Check("recognizer", "a missing model file fails with a reason", reported && !loaded && !why.empty());
	t.Check("recognizer", "no segment opens without a model", !rec.Ready() && !rec.BeginSegment());
	rec.Stop();
	rec.Stop();
	t.Check("recognizer", "stop after a failed load is idempotent", !rec.Ready());
}

void TestRecognizer(Tally &t)
{
	// Loading a model costs seconds and hundreds of megabytes, so the recognizer runs
	// in the smoke test only when a model directory is named:
	//   BRAIDCAST_SELFTEST_VOICE_MODELS=D:/.../voice-p0/models
	const std::optional<std::string> modelDir = Env::Raw("BRAIDCAST_SELFTEST_VOICE_MODELS");
	if (!modelDir || modelDir->empty()) {
		t.Skip("recognizer", "transcribes the fixture", "set BRAIDCAST_SELFTEST_VOICE_MODELS to run");
		return;
	}
	const std::string modelPath = *modelDir + "/ggml-base.en-q5_1.bin";
	const std::string wavPath = VoiceDataPath("fixtures/switch-to-gameplay.wav");
	Voice::WavData wav;
	std::string error;
	if (wavPath.empty() || !Voice::LoadWavMono(wavPath, wav, error)) {
		t.Check("recognizer", "fixture available", false);
		return;
	}

	Voice::SpscRing ring(Voice::kVoiceSampleRate * 20);
	Voice::Recognizer rec(ring, [] { return false; });

	std::mutex mutex;
	std::condition_variable cv;
	std::vector<Voice::Recognizer::Result> results;
	bool ready = false;
	rec.SetModelCallback([&](bool ok, const std::string &why) {
		std::lock_guard<std::mutex> lock(mutex);
		ready = ok;
		error = why;
		cv.notify_all();
	});
	rec.SetResultCallback([&](Voice::Recognizer::Result r) {
		std::lock_guard<std::mutex> lock(mutex);
		results.push_back(std::move(r));
		cv.notify_all();
	});

	rec.Start(modelPath, 2);
	{
		std::unique_lock<std::mutex> lock(mutex);
		cv.wait_for(lock, std::chrono::seconds(60), [&] { return ready || !error.empty(); });
	}
	t.Check("recognizer", "model loads", ready);
	if (!ready) {
		HostLog("[selftest] voice-recognizer model load error: " + error);
		rec.Stop();
		return;
	}

	// Too short: under the minimum, the segment is refused without running whisper.
	rec.BeginSegment();
	ring.Write(wav.samples.data(), 1600); // 100 ms
	rec.EndSegment();
	{
		std::unique_lock<std::mutex> lock(mutex);
		cv.wait_for(lock, std::chrono::seconds(10), [&] { return !results.empty(); });
	}
	t.Check("recognizer", "a 100 ms segment is refused",
		results.size() == 1 && !results[0].ok && results[0].text.empty());
	results.clear();

	// The real fixture.
	const bool opened = rec.BeginSegment();
	ring.Write(wav.samples.data(), wav.samples.size());
	rec.EndSegment();
	{
		std::unique_lock<std::mutex> lock(mutex);
		cv.wait_for(lock, std::chrono::seconds(120), [&] { return !results.empty(); });
	}
	const std::string text = results.empty() ? std::string() : StringUtil::ToLower(results[0].text);
	t.Check("recognizer", "transcribes the fixture",
		opened && results.size() == 1 && results[0].ok && text.find("gameplay") != std::string::npos);
	t.Check("recognizer", "transcript has no bracketed noise markers",
		text.find("[") == std::string::npos && text.find("(") == std::string::npos);

	// A second BeginSegment while one is open is refused rather than interleaved.
	rec.BeginSegment();
	t.Check("recognizer", "overlapping segments are refused", !rec.BeginSegment());
	rec.EndSegment();

	rec.Stop();
	t.Check("recognizer", "stop is idempotent", (rec.Stop(), true));
}

void TestVoiceEngine(Tally &t)
{
	Voice::VoiceEngine &engine = Voice::Engine();
	const VoiceSettings original = engine.Settings();

	// The smoke run starts the engine with voice disabled (the shipping default), so
	// nothing is bound and the state is reportable.
	const nlohmann::json state = engine.StateJson();
	t.Check("engine", "state has the documented shape",
		state.contains("state") && state.contains("ready") && state["ready"].contains("cpu") &&
			state["ready"].contains("model") && state["ready"].contains("mic") &&
			state.contains("settings") && state.contains("pending") && state.contains("device"));
	t.Check("engine", "disabled by default", state["state"] == "disabled" && !state["settings"]["enabled"]);

	// A key press while disabled must be inert, from the hotkey thread as in real use:
	// no state change and no mute.
	OBSSourceAutoRelease mic = obs_get_output_source(GlobalAudio::PrimaryMicChannel());
	const bool mutedBefore = mic && obs_source_muted(mic);
	std::thread hotkey([&] {
		engine.OnPtt(true);
		engine.OnPtt(false);
		engine.OnCancelKey();
	});
	hotkey.join();
	t.Check("engine", "push-to-talk while disabled does nothing",
		engine.StateJson()["state"] == "disabled" && (!mic || obs_source_muted(mic) == mutedBefore));

	// CanEnable explains itself rather than failing silently.
	std::string reason;
	const bool can = engine.CanEnable(reason);
	t.Check("engine", "CanEnable gives a reason when it refuses", can == reason.empty());

	// Thread count comes from the P0 cap and the machine, and is at least 1.
	t.Check("engine", "thread count is capped by the P0 measurement",
		engine.ThreadCount() >= 1 && engine.ThreadCount() <= Voice::P0::kThreadCap);

	// Enabling without a downloaded model must not throw, and must report not-ready
	// rather than pretending to listen. A CPU that cannot run voice stays disabled.
	VoiceSettings enabled = original;
	enabled.enabled = true;
	engine.ApplySettings(enabled);
	const nlohmann::json after = engine.StateJson();
	t.Check("engine", "enabling with no model reports not ready",
		can ? (after["state"] == "notReady" || after["state"] == "idle") : after["state"] == "disabled");
	VoiceSettings off = original;
	off.enabled = false;
	engine.ApplySettings(off);
	t.Check("engine", "disabling returns to disabled",
		engine.StateJson()["state"] == "disabled" && !engine.StateJson()["ready"]["mic"]);

	engine.ApplySettings(original);
}

// Registration is verified through libobs's own registry rather than the hotkey store's
// globals, so a hotkey that was declared but never registered still fails.
obs_hotkey_id FindHotkey(const char *name)
{
	struct Search {
		const char *name;
		obs_hotkey_id id;
	} search{name, OBS_INVALID_HOTKEY_ID};
	obs_enum_hotkeys(
		[](void *param, obs_hotkey_id id, obs_hotkey_t *hotkey) {
			auto *s = static_cast<Search *>(param);
			const char *registered = obs_hotkey_get_name(hotkey);
			if (registered && std::string(registered) == s->name) {
				s->id = id;
				return false;
			}
			return true;
		},
		&search);
	return search.id;
}

void TestVoiceHotkeys(Tally &t)
{
	const obs_hotkey_id ptt = FindHotkey("Braidcast.Voice.PushToTalk");
	const obs_hotkey_id cancel = FindHotkey("Braidcast.Voice.Cancel");
	t.Check("hotkeys", "push-to-talk is registered", ptt != OBS_INVALID_HOTKEY_ID);
	t.Check("hotkeys", "cancel is registered", cancel != OBS_INVALID_HOTKEY_ID);
	// The existing four keep their OBSBasic.* names so saved bindings still match.
	t.Check("hotkeys", "the existing streaming hotkeys survived the refactor",
		FindHotkey("OBSBasic.StartStreaming") != OBS_INVALID_HOTKEY_ID &&
			FindHotkey("OBSBasic.StopStreaming") != OBS_INVALID_HOTKEY_ID);
	t.Check("hotkeys", "the virtual camera hotkeys survived the refactor",
		FindHotkey("OBSBasic.StartVirtualCam") != OBS_INVALID_HOTKEY_ID &&
			FindHotkey("OBSBasic.StopVirtualCam") != OBS_INVALID_HOTKEY_ID);

	// Cancel defaults to Escape. A binding saved in hotkeys.json wins over the default,
	// so the check only applies while there is none.
	OBSDataAutoRelease saved = LoadStoreData(MultistreamBasicPath("hotkeys.json"));
	if (saved && obs_data_has_user_value(saved, "Braidcast.Voice.Cancel")) {
		t.Skip("hotkeys", "cancel defaults to Escape", "hotkeys.json has a saved binding for it");
		return;
	}
	struct Bound {
		obs_hotkey_id id;
		bool escape;
	} bound{cancel, false};
	obs_enum_hotkey_bindings(
		[](void *param, size_t, obs_hotkey_binding_t *binding) {
			auto *b = static_cast<Bound *>(param);
			if (obs_hotkey_binding_get_hotkey_id(binding) == b->id &&
			    obs_hotkey_binding_get_key_combination(binding).key == OBS_KEY_ESCAPE) {
				b->escape = true;
			}
			return true;
		},
		&bound);
	t.Check("hotkeys", "cancel defaults to Escape", bound.escape);
}

void TestTextNormalize(Tally &t)
{
	using Voice::Normalize;

	t.Check("normalize", "lowercases and strips punctuation",
		Normalize("Switch to BRB, please!").text == "switch to brb please");
	t.Check("normalize", "collapses whitespace", Normalize("  switch\tto   brb \n").text == "switch to brb");
	t.Check("normalize", "keeps digits", Normalize("scene 2").text == "scene 2");
	t.Check("normalize", "number words become digits",
		Normalize("switch to scene twenty one").text == "switch to scene 21");
	t.Check("normalize", "a hyphenated number becomes digits", Normalize("scene twenty-one").text == "scene 21");
	t.Check("normalize", "zero through nine", Normalize("camera zero and nine").text == "camera 0 and 9");
	t.Check("normalize", "a spelled-out abbreviation collapses",
		Normalize("switch to B.R.B.").text == "switch to brb");
	t.Check("normalize", "apostrophes inside words survive", Normalize("don't stop").text == "don't stop");
	t.Check("normalize", "an empty string is empty", Normalize("").text.empty() && Normalize("...").text.empty());

	// The span map is what lets a chat message keep its original spelling.
	const Voice::Normalized message = Normalize("send Hello, World! to chat");
	t.Check("normalize", "tokens are recorded", message.tokens.size() == 5 && message.tokens[1] == "hello");
	t.Check("normalize", "a token range maps back to the original text", message.Original(1, 2) == "Hello, World!");
	t.Check("normalize", "the whole range maps back to the whole input",
		message.Original(0, 4) == "send Hello, World! to chat");
}

void TestFuzzyMatch(Tally &t)
{
	using Voice::Similarity;

	t.Check("fuzzy", "identical is 1", Similarity("gameplay", "gameplay") > 0.999);
	t.Check("fuzzy", "a contained phrase scores above the threshold",
		Similarity("gameplay", "gameplay cam") >= Voice::kSlotAcceptThreshold);
	t.Check("fuzzy", "a one-letter slip still matches",
		Similarity("gameplay", "game play") >= Voice::kSlotAcceptThreshold);
	t.Check("fuzzy", "an unrelated name does not match", Similarity("gameplay", "starting soon") < 0.4);
	t.Check("fuzzy", "an empty query matches nothing", Similarity("", "gameplay") == 0.0);
	t.Check("fuzzy", "a longer container scores lower than a tighter one",
		Similarity("brb", "brb screen") > Similarity("brb", "brb screen with a very long name"));
	t.Check("fuzzy", "case and spacing do not matter", Similarity("be right back", "Be Right  Back") > 0.999);
	// Containment counts whole words only: "art" is not "Starting Soon".
	t.Check("fuzzy", "a word inside another word is not containment",
		Similarity("art", "starting soon") < Voice::kSlotAcceptThreshold);

	// The best-of helper is what the matcher actually calls: it returns the winner only
	// when it is both good enough and clearly ahead of the runner-up.
	const std::vector<std::string> scenes = {"Gameplay Cam", "Starting Soon", "BRB"};
	Voice::SlotMatch match = Voice::BestMatch("gameplay", scenes);
	t.Check("fuzzy", "best match picks the right scene", match.ok && match.index == 0);

	match = Voice::BestMatch("weather forecast", scenes);
	t.Check("fuzzy", "no candidate above the threshold is a miss", !match.ok && !match.ambiguous);

	const std::vector<std::string> twins = {"Game One", "Game Two"};
	match = Voice::BestMatch("game", twins);
	t.Check("fuzzy", "two equally good candidates are ambiguous, not a coin flip", !match.ok && match.ambiguous);

	match = Voice::BestMatch("game one", twins);
	t.Check("fuzzy", "a clear winner among similar names is accepted", match.ok && match.index == 0);

	match = Voice::BestMatch("brb", {"BRB 2", "BRB"});
	t.Check("fuzzy", "an exact name beats a longer one containing it", match.ok && match.index == 1);

	match = Voice::BestMatch("anything", {});
	t.Check("fuzzy", "an empty candidate list is a miss", !match.ok && !match.ambiguous);
}

// Every voice method goes through Bridge::Dispatch, exactly as the web reaches it.
bool Dispatch(const char *method, const nlohmann::json &params, nlohmann::json &result, std::string &error)
{
	result = nlohmann::json();
	error.clear();
	return Bridge::Dispatch(method, params, result, error);
}

void TestVoiceBridge(Tally &t)
{
	const VoiceSettings original = Voice::Engine().Settings();
	nlohmann::json result;
	std::string error;

	t.Check("bridge", "settings.getVoice answers",
		Dispatch("settings.getVoice", nlohmann::json::object(), result, error) && result.contains("settings") &&
			result.contains("models") && result.contains("cpu"));
	const bool modelsListed = result["models"].is_array() && !result["models"].empty() &&
				  result["models"][0].contains("selectable");
	t.Check("bridge", "the catalog comes with the settings", modelsListed);

	// An unknown model id is refused rather than stored and failing later at load.
	const bool badModel = Dispatch("settings.setVoice", nlohmann::json{{"model", "not-a-model"}}, result, error);
	t.Check("bridge", "an unknown model id is refused", !badModel && !error.empty());

	// A known one is accepted and comes back in the response.
	const bool goodModel =
		Dispatch("settings.setVoice", nlohmann::json{{"model", Voice::P0::kDefaultModelId}}, result, error);
	t.Check("bridge", "a known model id is accepted",
		goodModel && result["settings"]["model"] == Voice::P0::kDefaultModelId);

	// Enabling on a CPU that cannot run voice is refused with the CPU's own reason, and
	// enabling before the model is on disk is refused too. Both apply to turning voice
	// ON, so start from off (a settings change while already on is not refused, or a CPU
	// that lost support could never be switched back off).
	Dispatch("settings.setVoice", nlohmann::json{{"enabled", false}}, result, error);
	std::string cpuReason;
	const Voice::ModelInfo *model = Voice::FindModel(Voice::P0::kDefaultModelId);
	if (!Voice::Engine().CanEnable(cpuReason)) {
		t.Check("bridge", "enabling on an unsupported CPU is refused",
			!Dispatch("settings.setVoice", nlohmann::json{{"enabled", true}}, result, error) &&
				error == cpuReason);
	} else {
		t.Skip("bridge", "enabling on an unsupported CPU is refused", "this CPU supports voice");
		if (model && !Voice::ModelFilePresent(*model)) {
			t.Check("bridge", "enabling before the model is downloaded is refused",
				!Dispatch("settings.setVoice", nlohmann::json{{"enabled", true}}, result, error) &&
					!error.empty() && !Voice::Engine().Settings().enabled);
		} else {
			t.Skip("bridge", "enabling before the model is downloaded is refused", "the model is present");
		}
	}

	t.Check("bridge", "voice.state answers",
		Dispatch("voice.state", nlohmann::json::object(), result, error) && result.contains("state") &&
			result.contains("ready"));

	t.Check("bridge", "voice.model.status lists the catalog",
		Dispatch("voice.model.status", nlohmann::json::object(), result, error) && result["models"].is_array());

	t.Check("bridge", "downloading an unknown model is refused",
		!Dispatch("voice.model.download", nlohmann::json{{"id", "not-a-model"}}, result, error));

	t.Check("bridge", "cancelling a download that is not running is refused",
		!Dispatch("voice.model.cancel", nlohmann::json{{"id", Voice::kVadModelId}}, result, error));

	// The settings file round-trips through the store, not just through memory.
	VoiceSettings persisted;
	persisted.Load();
	t.Check("bridge", "setVoice persisted the model", persisted.model == Voice::P0::kDefaultModelId);

	// Put back what the run found, in memory and on disk.
	Dispatch("settings.setVoice", SettingsFields::ToJson(VoiceSettingsTable(), original), result, error);
}

using Case = void (*)(Tally &);

const Case kCases[] = {
	&TestWhisperLinked,   &TestCpuGate,       &TestLogCategory,  &TestVoiceSettingsTable,
	&TestSha256,          &TestHttpCancel,    &TestModelCatalog, &TestModelVerifyAndCommit,
	&TestPostToUiDelayed, &TestSpscRing,      &TestResampler,    &TestMicMuteGuard,
	&TestVoiceCapture,    &TestVoiceListener, &TestWavFile,      &TestRecognizerWithoutModel,
	&TestRecognizer,      &TestVoiceEngine,   &TestVoiceHotkeys, &TestVoiceBridge,
	&TestTextNormalize,   &TestFuzzyMatch,
};

} // namespace

void ObsBootstrap::RunVoiceSelfTest()
{
	Tally t;
	for (Case c : kCases) {
		c(t);
	}
	HostLog(std::string("[selftest] voice overall -> ") + (t.failures == 0 ? "PASS" : "FAIL (BUG)"));
}
