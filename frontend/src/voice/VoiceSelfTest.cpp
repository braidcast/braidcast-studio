#include "obs_bootstrap.hpp"

#include "log.hpp"
#include "multistream/GlobalAudioChannels.hpp"
#include "util/file_util.hpp"
#include "util/async_task.hpp"
#include "util/http_client.hpp"
#include "util/sha256.hpp"
#include "util/time_util.hpp"
#include "voice/MicMuteGuard.hpp"
#include "voice/VoiceCapture.hpp"
#include "voice/VoiceCpu.hpp"
#include "voice/VoiceModels.hpp"
#include "voice/VoiceResampler.hpp"
#include "voice/VoiceRing.hpp"
#include "voice/VoiceSettings.hpp"

#include <obs.hpp>
#include <whisper.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
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
	guard.Engage(src);
	t.Check("mic", "engage mutes an unmuted mic", obs_source_muted(src));
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
	guard.Engage(src);
	t.Check("mic", "engage on an already muted mic is a no-op", obs_source_muted(src));
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

using Case = void (*)(Tally &);

const Case kCases[] = {
	&TestWhisperLinked, &TestCpuGate,      &TestLogCategory,          &TestVoiceSettingsTable, &TestSha256,
	&TestHttpCancel,    &TestModelCatalog, &TestModelVerifyAndCommit, &TestPostToUiDelayed,    &TestSpscRing,
	&TestResampler,     &TestMicMuteGuard, &TestVoiceCapture,
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
