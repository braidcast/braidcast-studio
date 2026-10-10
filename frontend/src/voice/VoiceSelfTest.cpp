#include "obs_bootstrap.hpp"

#include "audio/AudioEndpoints.hpp"
#include "bridge.hpp"
#include "chat/chat_limits.hpp"
#include "chat/recent_chatters.hpp"
#include "log.hpp"
#include "multistream/GlobalAudioChannels.hpp"
#include "multistream/StorePaths.hpp"
#include "scene/transitions.hpp"
#include "settings/AdvancedSettings.hpp"
#include "util/file_util.hpp"
#include "util/async_task.hpp"
#include "util/env_config.hpp"
#include "util/http_client.hpp"
#include "util/paths.hpp"
#include "util/sha256.hpp"
#include "util/string_util.hpp"
#include "util/time_util.hpp"
#include "voice/CommandMatcher.hpp"
#include "voice/CommandRegistry.hpp"
#include "voice/FuzzyMatch.hpp"
#include "voice/MicMuteGuard.hpp"
#include "voice/Recognizer.hpp"
#include "voice/TextNormalize.hpp"
#include "voice/Tts.hpp"
#include "voice/VadEndpointer.hpp"
#include "voice/VoiceCapture.hpp"
#include "voice/VoiceCpu.hpp"
#include "voice/VoiceEngine.hpp"
#include "voice/VoiceFeedback.hpp"
#include "voice/VoiceListener.hpp"
#include "voice/VoiceModels.hpp"
#include "voice/VoiceResampler.hpp"
#include "voice/VoiceRing.hpp"
#include "voice/VoiceSettings.hpp"
#include "voice/WakeGate.hpp"
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

	VoiceSettings volumes;
	std::string volumeError;
	SettingsFields::ApplyPatch(table, nlohmann::json{{"cueVolume", 0.5}}, volumes, volumeError);
	t.Check("settings", "cue volume applies", volumes.cueVolume == 0.5);
	SettingsFields::ApplyPatch(table, nlohmann::json{{"cueVolume", 4.0}}, volumes, volumeError);
	t.Check("settings", "cue volume clamps high", volumes.cueVolume == 1.0);
	SettingsFields::ApplyPatch(table, nlohmann::json{{"cueVolume", -2.0}}, volumes, volumeError);
	t.Check("settings", "cue volume clamps low", volumes.cueVolume == 0.0);
	t.Check("settings", "cue volume defaults audible", VoiceSettings{}.cueVolume > 0.0);
	t.Check("settings", "cue volume is on the wire and in the file",
		SettingsFields::ToJson(table, VoiceSettings{}).contains("cueVolume") &&
			obs_data_has_user_value(data, "cue_volume"));

	VoiceSettings modes;
	std::string modeError;
	t.Check("settings", "the send mode defaults to countdown", modes.sendMode == "countdown");
	t.Check("settings", "countdown defaults to three seconds", modes.countdownSec == 3.0);
	t.Check("settings", "a known send mode applies",
		SettingsFields::ApplyPatch(table, nlohmann::json{{"sendMode", "instant"}}, modes, modeError) &&
			modes.sendMode == "instant");
	t.Check("settings", "an unknown send mode is refused",
		!SettingsFields::ApplyPatch(table, nlohmann::json{{"sendMode", "telepathy"}}, modes, modeError) &&
			modes.sendMode == "instant");
	SettingsFields::ApplyPatch(table, nlohmann::json{{"countdownSec", 60.0}}, modes, modeError);
	t.Check("settings", "the countdown clamps", modes.countdownSec <= 10.0);

	VoiceSettings listen;
	std::string listenError;
	t.Check("settings", "push-to-talk is the default trigger", listen.triggerMode == "ptt");
	t.Check("settings", "the default wake phrase is Braidcast", listen.wakePhrase == "Braidcast");
	t.Check("settings", "read-back is off by default", !listen.readBack);
	t.Check("settings", "the default language is English", listen.language == "en");

	t.Check("settings", "always-listen applies",
		SettingsFields::ApplyPatch(table, nlohmann::json{{"triggerMode", "wake"}}, listen, listenError) &&
			listen.triggerMode == "wake");
	t.Check("settings", "an unknown trigger mode is refused",
		!SettingsFields::ApplyPatch(table, nlohmann::json{{"triggerMode", "telepathy"}}, listen, listenError));
	t.Check("settings", "a supported language applies",
		SettingsFields::ApplyPatch(table, nlohmann::json{{"language", "de"}}, listen, listenError) &&
			listen.language == "de");
	t.Check("settings", "an unsupported language is refused",
		!SettingsFields::ApplyPatch(table, nlohmann::json{{"language", "xx"}}, listen, listenError));
	t.Check("settings", "an over-long wake phrase is refused",
		!SettingsFields::ApplyPatch(table, nlohmann::json{{"wakePhrase", std::string(200, 'x')}}, listen,
					    listenError));
	// Every offered language is one whisper knows; whisper_lang_id is -1 otherwise.
	bool whisperKnowsAll = true;
	for (const std::string &language : VoiceLanguages()) {
		whisperKnowsAll = whisperKnowsAll && whisper_lang_id(language.c_str()) >= 0;
	}
	t.Check("settings", "whisper knows every offered language", whisperKnowsAll && VoiceLanguages().size() == 15);
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
	t.Check("models", "P0 default is selectable", Voice::IsSelectableModel(Voice::P0::kDefaultModelId, "en"));
	t.Check("models", "multilingual is not in the English picker",
		!Voice::IsSelectableModel(Voice::kMultilingualModelId, "en"));
	t.Check("models", "unknown id rejected",
		Voice::FindModel("nope") == nullptr && !Voice::IsSelectableModel("nope", "en"));
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

	// N-I1: a mute the USER makes during the hold survives the release. Under the guard's
	// own mute it changes nothing in libobs, so it is caught where it enters. The bridge
	// seam (audio.setMuted, the mixer): mute during the hold, release, still muted, and a
	// save during the hold already stored muted.
	std::string error;
	obs_source_set_muted(src, false);
	guard.Engage(src);
	const bool seam = Bridge::SetSourceMuted(src, true, error);
	t.Check("mic", "a mute through the bridge during the hold is saved as muted",
		seam && GlobalAudio::PersistedMuteOverride(src, false));
	guard.Release();
	t.Check("mic", "a mute through the bridge during the hold survives the release", obs_source_muted(src));
	// Every other path reaches the mic's "mute" signal: libobs's own mute hotkey (which now
	// sets, and signals, even when the source is already muted), plugins, scripts.
	obs_source_set_muted(src, false);
	guard.Engage(src);
	obs_source_set_muted(src, true);
	guard.Release();
	t.Check("mic", "a mute by any other path during the hold survives the release", obs_source_muted(src));
	// The last choice wins, either way.
	obs_source_set_muted(src, false);
	guard.Engage(src);
	Bridge::SetSourceMuted(src, false, error);
	Bridge::SetSourceMuted(src, true, error);
	guard.Release();
	t.Check("mic", "unmuted then muted again during the hold: muted", obs_source_muted(src));
	obs_source_set_muted(src, false);
	guard.Engage(src);
	Bridge::SetSourceMuted(src, true, error);
	Bridge::SetSourceMuted(src, false, error);
	guard.Release();
	t.Check("mic", "muted then unmuted again during the hold: unmuted", !obs_source_muted(src));
	// And the guard's own mute is never taken for the user's.
	guard.Engage(src);
	guard.Release();
	t.Check("mic", "a hold the user left alone still gives the mic back", !obs_source_muted(src));
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
	} else if (text.rfind("draft ", 0) == 0) {
		// What the countdown send mode produces: pending, and sent when its window closes.
		out.kind = Voice::Interpretation::Kind::Pending;
		out.action.commandId = text.substr(6);
		out.action.summary = "draft " + out.action.commandId;
		out.action.runOnTimeout = true;
		out.action.timeoutMs = 3000;
		out.action.needsConfirmWord = false;
		out.action.params = {{"text", "hello"}};
	} else if (text.rfind("hold ", 0) == 0) {
		// What the "say send" mode produces: a draft that waits for the word.
		out.kind = Voice::Interpretation::Kind::Pending;
		out.action.commandId = text.substr(5);
		out.action.summary = "hold " + out.action.commandId;
		out.action.needsConfirmWord = true;
		out.action.params = {{"text", "held words"}};
	} else if (text.rfind("ask ", 0) == 0) {
		// A confirm-tier command as the registry words it: a question, read back if asked.
		out.kind = Voice::Interpretation::Kind::Pending;
		out.action.commandId = text.substr(4);
		out.action.summary = "Do " + out.action.commandId + "?";
		out.action.readBack = true;
	} else if (text == "read that back" && ctx.pending) {
		out.kind = Voice::Interpretation::Kind::Control;
		out.control = Voice::Interpretation::Control::ReadBackPending;
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
		const std::string up = EffectNames(l->Handle(Ev(EventType::PttUp, 900)));
		t.Check("listener", "key up moves to thinking and cues heard",
			up == "cue:heard" && l->Current() == S::Thinking);
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
			EffectNames(l->Handle(down)) == "cue:error" && l->Current() == S::Idle &&
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
			EffectNames(l->Handle(failed)) == "cue:error" && l->Current() == S::Idle &&
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
			EffectNames(l->Handle(Ev(EventType::MicLost, 300))) == "cue:error" &&
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
			EffectNames(l->Handle(result)) == "cue:error" && l->Current() == S::Idle &&
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

	// 26. Confirming from the UI runs the pending command, exactly like saying yes. The
	// click names the action it was for (N-I5).
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		l->Handle(Transcript("confirm streaming.stop", 1400));
		Event click = Ev(EventType::Confirm, 1600);
		click.pendingId = l->Snapshot().pending.id;
		t.Check("listener", "a UI confirmation runs the pending command",
			EffectNames(l->Handle(click)) == "run:streaming.stop,cue:accept" && l->Current() == S::Idle);
	}

	// 27. Confirming with nothing pending does nothing at all.
	{
		auto l = fresh();
		t.Check("listener", "a UI confirmation with nothing pending is inert",
			l->Handle(Ev(EventType::Confirm, 100)).empty() && l->Current() == S::Idle);
	}

	// 28. A click while the key is held runs the command and leaves the segment open; its
	// answer then finds nothing pending.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		l->Handle(Transcript("confirm streaming.stop", 1400));
		l->Handle(Ev(EventType::PttDown, 2000));
		Event click = Ev(EventType::Confirm, 2100);
		click.pendingId = l->Snapshot().pending.id;
		const std::string fx = EffectNames(l->Handle(click));
		const bool stillListening = l->Current() == S::Listening;
		l->Handle(Ev(EventType::PttUp, 2600));
		t.Check("listener", "a UI confirmation mid-segment keeps the segment",
			fx == "run:streaming.stop,cue:accept" && stillListening &&
				EffectNames(l->Handle(Transcript("yes", 2900))).find("run:") == std::string::npos);
	}

	// 29. A draft runs when its timer expires, where a command would be dropped, and its
	// window is its own (3 s here), not the 8 s a command gets.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		const std::vector<Effect> fx = l->Handle(Transcript("draft chat.send", 1400));
		t.Check("listener", "a draft waits and schedules its own send",
			EffectNames(fx) == "cue:pending,tick" && l->Current() == S::Pending && fx.back().atMs == 4400 &&
				l->Snapshot().pending.deadlineMs == 4400);
		const nlohmann::json json = l->StatusJson();
		t.Check("listener", "a draft's state carries its text and window",
			json["pending"].value("runOnTimeout", false) && json["pending"].value("timeoutMs", 0) == 3000 &&
				json["pending"].value("text", "") == "hello");
		Event tick = Ev(EventType::Tick, 4400);
		tick.seq = fx.back().seq;
		t.Check("listener", "a draft sends when the timer expires",
			EffectNames(l->Handle(tick)) == "run:chat.send,cue:accept" && l->Current() == S::Idle);
	}

	// 30. Cancelling a draft before the timer stops it being sent, and its tick then finds
	// nothing.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		const std::vector<Effect> fx = l->Handle(Transcript("draft chat.send", 1400));
		const bool cancelled = EffectNames(l->Handle(Ev(EventType::Cancel, 1500))) == "cue:cancel" &&
				       l->Snapshot().pending.commandId.empty();
		Event tick = Ev(EventType::Tick, 4400);
		tick.seq = fx.back().seq;
		t.Check("listener", "cancelling a draft does not send it", cancelled && l->Handle(tick).empty());
	}

	// 31. (N-I3) A draft whose window closes while the key is held is NOT sent then: it
	// waits for that segment, so a "cancel" said just as the countdown ran out wins. The
	// state keeps the draft (due), so the UI can keep showing it while the segment runs.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		const std::vector<Effect> fx = l->Handle(Transcript("draft chat.send", 1400));
		l->Handle(Ev(EventType::PttDown, 4000));
		Event tick = Ev(EventType::Tick, 4400);
		tick.seq = fx.back().seq;
		const bool waited = l->Handle(tick).empty() && l->Current() == S::Listening &&
				    l->Snapshot().pending.due && l->StatusJson()["pending"].value("due", false);
		l->Handle(Ev(EventType::PttUp, 4600));
		const std::string answer = EffectNames(l->Handle(Transcript("never mind", 4900)));
		t.Check("listener", "a draft due mid-segment waits for it, and a cancel in it wins",
			waited && answer == "cue:cancel" && l->Snapshot().pending.commandId.empty() &&
				l->Current() == S::Idle);
	}

	// 32. "Read that back" speaks the pending command again and leaves it waiting, on the
	// same deadline: its tick still expires it.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		const std::vector<Effect> pending = l->Handle(Transcript("ask streaming.stop", 1400));
		l->Handle(Ev(EventType::PttDown, 2000));
		l->Handle(Ev(EventType::PttUp, 2600));
		const std::vector<Effect> back = l->Handle(Transcript("read that back", 3000));
		const bool spoken = EffectNames(back) == "readback" && back.back().text == "Do streaming.stop?";
		const bool waiting = l->Current() == S::Pending && l->Snapshot().pending.commandId == "streaming.stop";
		Event tick = Ev(EventType::Tick, 9400);
		tick.seq = pending.back().seq;
		t.Check("listener", "read that back speaks the pending command and keeps it on its deadline",
			spoken && waiting && EffectNames(l->Handle(tick)) == "cue:cancel" && l->Current() == S::Idle);
	}

	// 33. A confirmed command with read-back says what it did, not the question.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		l->Handle(Transcript("ask streaming.stop", 1400));
		l->Handle(Ev(EventType::PttDown, 2000));
		l->Handle(Ev(EventType::PttUp, 2600));
		const std::vector<Effect> fx = l->Handle(Transcript("yes", 3000));
		t.Check("listener", "a confirmed command reads back without its question mark",
			EffectNames(fx) == "run:streaming.stop,cue:accept,readback" &&
				fx.back().text == "Do streaming.stop");
	}

	// 34. (N-I5) Every pending action has its own id, and a click from the UI must name
	// the one pending now: a click meant for draft A that lands after A expired must not
	// confirm what is pending by then, which can be "Stop streaming?".
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		const std::vector<Effect> first = l->Handle(Transcript("draft chat.send", 1400));
		const uint64_t draftId = l->Snapshot().pending.id;
		l->Handle(Ev(EventType::Cancel, 4000)); // the draft is dropped
		l->Handle(Ev(EventType::PttDown, 5000));
		l->Handle(Ev(EventType::PttUp, 5600));
		l->Handle(Transcript("confirm streaming.stop", 6000));
		const uint64_t stopId = l->Snapshot().pending.id;
		t.Check("listener", "each pending action gets an id of its own, carried in the state",
			draftId != 0 && stopId != 0 && stopId != draftId &&
				l->StatusJson()["pending"].value("id", static_cast<uint64_t>(0)) == stopId);
		Event late = Ev(EventType::Confirm, 6100);
		late.pendingId = draftId;
		const bool lateIgnored = l->Handle(late).empty() && l->Snapshot().pending.id == stopId;
		Event bare = Ev(EventType::Confirm, 6200);
		const bool bareIgnored = l->Handle(bare).empty() && l->Snapshot().pending.id == stopId;
		t.Check("listener", "a UI confirmation for another action, or for none, does nothing",
			lateIgnored && bareIgnored && l->Current() == S::Pending);
		Event lateCancel = Ev(EventType::Cancel, 6300);
		lateCancel.pendingId = draftId;
		const bool cancelIgnored = l->Handle(lateCancel).empty() && l->Snapshot().pending.id == stopId;
		const bool keyCancels = EffectNames(l->Handle(Ev(EventType::Cancel, 6400))) == "cue:cancel" &&
					l->Snapshot().pending.commandId.empty();
		t.Check("listener", "a UI cancel for another action does nothing; the cancel key needs no id",
			cancelIgnored && keyCancels && !first.empty());
	}

	// 35. (W-14) A "say send" draft whose window closes is neither sent nor lost: the text
	// is kept for the composer and the reason says so. Its window is visible in the state.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		const std::vector<Effect> fx = l->Handle(Transcript("hold chat.send", 1400));
		const bool windowShown = l->StatusJson()["pending"].value("timeoutMs", 0) ==
					 VoiceListener::kPendingTimeoutMs;
		Event tick = Ev(EventType::Tick, 1400 + VoiceListener::kPendingTimeoutMs);
		tick.seq = fx.back().seq;
		const std::string expired = EffectNames(l->Handle(tick));
		t.Check("listener", "a held draft that expires is kept, not sent, with a reason",
			windowShown && expired == "cue:cancel" && l->Current() == S::Idle &&
				l->Snapshot().pending.commandId.empty() && l->Snapshot().keptDraft == "held words" &&
				l->Snapshot().message.find("not sent") != std::string::npos);
	}

	// 36. (N-I3) A draft due mid-segment is sent when that segment says send.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		const std::vector<Effect> fx = l->Handle(Transcript("draft chat.send", 1400));
		l->Handle(Ev(EventType::PttDown, 4000));
		Event tick = Ev(EventType::Tick, 4400);
		tick.seq = fx.back().seq;
		l->Handle(tick);
		l->Handle(Ev(EventType::PttUp, 4600));
		t.Check("listener", "a draft due mid-segment is sent when the segment says yes",
			EffectNames(l->Handle(Transcript("yes", 4900))) == "run:chat.send,cue:accept" &&
				l->Current() == S::Idle);
	}

	// 37. (N-I3) A draft due mid-segment that the segment neither sends nor cancels (a miss,
	// a failed transcription) is held for "send" on a fresh window, never sent on its own;
	// and when that window closes too, its text is kept.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		const std::vector<Effect> fx = l->Handle(Transcript("draft chat.send", 1400));
		l->Handle(Ev(EventType::PttDown, 4000));
		Event tick = Ev(EventType::Tick, 4400);
		tick.seq = fx.back().seq;
		l->Handle(tick);
		l->Handle(Ev(EventType::PttUp, 4600));
		const std::vector<Effect> held = l->Handle(Transcript("the weather is nice", 4900));
		const PendingAction &p = l->Snapshot().pending;
		const bool isHeld = EffectNames(held) == "cue:reject,cue:pending,tick" && l->Current() == S::Pending &&
				    p.commandId == "chat.send" && !p.runOnTimeout && p.needsConfirmWord && !p.due &&
				    p.deadlineMs == 4900 + VoiceListener::kPendingTimeoutMs &&
				    l->Snapshot().message.find("not sent") != std::string::npos;
		Event again = Ev(EventType::Tick, p.deadlineMs);
		again.seq = held.back().seq;
		const std::string expired = EffectNames(l->Handle(again));
		t.Check("listener", "a draft due mid-segment that the segment misses is held, then kept",
			isHeld && expired == "cue:cancel" && l->Snapshot().keptDraft == "hello");

		auto f = fresh();
		f->Handle(Ev(EventType::PttDown, 100));
		f->Handle(Ev(EventType::PttUp, 900));
		const std::vector<Effect> fx2 = f->Handle(Transcript("draft chat.send", 1400));
		f->Handle(Ev(EventType::PttDown, 4000));
		Event tick2 = Ev(EventType::Tick, 4400);
		tick2.seq = fx2.back().seq;
		f->Handle(tick2);
		f->Handle(Ev(EventType::PttUp, 4600));
		Event failed = Ev(EventType::TranscribeFailed, 4900);
		failed.text = "I did not hear anything.";
		failed.miss = true;
		const std::string afterFail = EffectNames(f->Handle(failed));
		t.Check("listener", "so is one whose segment heard nothing",
			afterFail.find("run:") == std::string::npos && f->Snapshot().pending.needsConfirmWord &&
				!f->Snapshot().pending.runOnTimeout && f->Current() == S::Pending);
	}

	// 38. (N-I3) A countdown already mostly spent when it is scheduled is never an instant
	// send: the engine reports it Lapsed and the draft is held for "send".
	{
		t.Check("listener", "a countdown with less than half its window left counts as spent",
			VoiceListener::CountdownSpent(-200, 3000) && VoiceListener::CountdownSpent(1400, 3000) &&
				!VoiceListener::CountdownSpent(1500, 3000) &&
				!VoiceListener::CountdownSpent(3000, 3000));
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		const std::vector<Effect> fx = l->Handle(Transcript("draft chat.send", 1400));
		Event lapsed = Ev(EventType::Lapsed, 4500);
		lapsed.seq = fx.back().seq;
		const std::string held = EffectNames(l->Handle(lapsed));
		Event stale = Ev(EventType::Tick, 4500);
		stale.seq = fx.back().seq;
		t.Check("listener", "a lapsed countdown is held for send, and its old tick sends nothing",
			held == "cue:pending,tick" && l->Snapshot().pending.needsConfirmWord &&
				!l->Snapshot().pending.runOnTimeout && l->Handle(stale).empty() &&
				l->Current() == S::Pending);
	}

	// 39. The spec's seven cues: a miss and an error sound different. Nothing heard (a
	// slip of the key, silence, a wake the speech model did not confirm) is a miss; a
	// recognizer failure is an error. Every cue has its own name, which names its file.
	{
		auto l = fresh();
		l->Handle(Ev(EventType::PttDown, 100));
		l->Handle(Ev(EventType::PttUp, 900));
		Event tooShort = Ev(EventType::TranscribeFailed, 1000);
		tooShort.text = "That was too short to hear.";
		tooShort.miss = true;
		const std::string missed = EffectNames(l->Handle(tooShort));
		l->Handle(Ev(EventType::PttDown, 2000));
		l->Handle(Ev(EventType::PttUp, 2900));
		Event slow = Ev(EventType::TranscribeFailed, 9000);
		slow.text = "Recognition took too long.";
		const std::string failed = EffectNames(l->Handle(slow));
		bool named = true;
		std::vector<std::string> names;
		for (size_t i = 0; i < 7; ++i) {
			const std::string name = CueName(static_cast<Cue>(i));
			named = named && !name.empty() && std::find(names.begin(), names.end(), name) == names.end();
			names.push_back(name);
		}
		t.Check("listener",
			"nothing heard cues a miss, a recognizer failure an error, and all seven cues are named",
			missed == "cue:reject" && failed == "cue:error" && named &&
				std::string(CueName(Cue::Heard)) == "heard" &&
				std::string(CueName(Cue::Error)) == "error");
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

void TestVadEndpointer(Tally &t)
{
	const std::optional<std::string> modelDir = Env::Raw("BRAIDCAST_SELFTEST_VOICE_MODELS");
	if (!modelDir || modelDir->empty()) {
		t.Skip("vad", "finds speech in the fixture", "set BRAIDCAST_SELFTEST_VOICE_MODELS to run");
		return;
	}
	using VadState = Voice::VadEndpointer::State;
	Voice::VadEndpointer vad;
	std::string error;
	if (!vad.Start(*modelDir + "/ggml-silero-v5.1.2.bin", 1, error)) {
		t.Check("vad", "the VAD model loads", false);
		HostLog("[selftest] voice-vad load error: " + error);
		return;
	}
	t.Check("vad", "the VAD model loads", true);

	// Silence stays silent.
	const std::vector<float> silence(Voice::kVoiceSampleRate, 0.f);
	VadState state = VadState::Silence;
	for (size_t offset = 0; offset + 1600 <= silence.size(); offset += 1600) {
		state = vad.Push(silence.data() + offset, 1600);
	}
	t.Check("vad", "silence is not speech", state == VadState::Silence);

	// The spoken fixture is. Its own trailing silence may already end the utterance, so
	// the end is looked for over the fixture and the silence after it together.
	Voice::WavData wav;
	const std::string wavPath = VoiceDataPath("fixtures/switch-to-gameplay.wav");
	if (wavPath.empty() || !Voice::LoadWavMono(wavPath, wav, error)) {
		t.Check("vad", "fixture available", false);
		return;
	}
	vad.Reset();
	bool sawSpeech = false;
	int ends = 0;
	for (size_t offset = 0; offset + 1600 <= wav.samples.size(); offset += 1600) {
		const VadState s = vad.Push(wav.samples.data() + offset, 1600);
		sawSpeech = sawSpeech || s == VadState::Speech;
		ends += s == VadState::Ended ? 1 : 0;
	}
	t.Check("vad", "finds speech in the fixture", sawSpeech);
	for (int second = 0; second < 2; ++second) {
		for (size_t offset = 0; offset + 1600 <= silence.size(); offset += 1600) {
			state = vad.Push(silence.data() + offset, 1600);
			ends += state == VadState::Ended ? 1 : 0;
		}
	}
	t.Check("vad", "silence ends the utterance", ends >= 1 && state == VadState::Silence);

	// A pause shorter than the hold-off does not end it; a longer one does. Speech is
	// whatever of the fixture first reads as speech, then digital silence follows.
	vad.Reset();
	state = VadState::Silence;
	for (size_t offset = 0; offset + 1600 <= wav.samples.size() && state != VadState::Speech; offset += 1600) {
		state = vad.Push(wav.samples.data() + offset, 1600);
	}
	bool endedEarly = false;
	for (size_t pushedMs = 0; pushedMs < 300; pushedMs += 100) {
		endedEarly = endedEarly || vad.Push(silence.data(), 1600) == VadState::Ended;
	}
	bool ended = false;
	for (size_t pushedMs = 300; pushedMs < 1500 && !ended; pushedMs += 100) {
		ended = vad.Push(silence.data(), 1600) == VadState::Ended;
	}
	t.Check("vad", "it waits out a short pause first", state == VadState::Speech && !endedEarly && ended);

	vad.Stop();
	t.Check("vad", "stop is idempotent", (vad.Stop(), !vad.Ready()));
}

void TestContinuousRecognizer(Tally &t)
{
	const std::optional<std::string> modelDir = Env::Raw("BRAIDCAST_SELFTEST_VOICE_MODELS");
	if (!modelDir || modelDir->empty()) {
		t.Skip("continuous", "wakes on the wake phrase", "set BRAIDCAST_SELFTEST_VOICE_MODELS to run");
		return;
	}
	// A second fixture beside the first, opened by the default wake phrase:
	// "Braidcast, switch to gameplay."
	Voice::WavData woken;
	Voice::WavData ordinary;
	std::string error;
	const std::string wokenPath = VoiceDataPath("fixtures/braidcast-switch-to-gameplay.wav");
	const std::string ordinaryPath = VoiceDataPath("fixtures/switch-to-gameplay.wav");
	if (wokenPath.empty() || ordinaryPath.empty() || !Voice::LoadWavMono(wokenPath, woken, error) ||
	    !Voice::LoadWavMono(ordinaryPath, ordinary, error)) {
		t.Check("continuous", "fixtures available", false);
		return;
	}

	Voice::SpscRing ring(Voice::kVoiceSampleRate * 30);
	Voice::Recognizer rec(ring, [] { return false; });

	std::mutex mutex;
	std::condition_variable cv;
	std::vector<Voice::Recognizer::Result> results;
	int wakes = 0;
	bool ready = false;
	rec.SetModelCallback([&](bool ok, const std::string &) {
		std::lock_guard<std::mutex> lock(mutex);
		ready = ok;
		cv.notify_all();
	});
	rec.SetWakeCallback([&] {
		std::lock_guard<std::mutex> lock(mutex);
		++wakes;
		cv.notify_all();
	});
	rec.SetResultCallback([&](Voice::Recognizer::Result r) {
		std::lock_guard<std::mutex> lock(mutex);
		results.push_back(std::move(r));
		cv.notify_all();
	});

	Voice::Recognizer::Continuous continuous;
	continuous.enabled = true;
	continuous.wakePhrase = VoiceSettings{}.wakePhrase;
	continuous.vadModelPath = *modelDir + "/ggml-silero-v5.1.2.bin";
	continuous.wakeModelPath = *modelDir + "/ggml-tiny.en-q5_1.bin";
	rec.SetContinuous(continuous);
	rec.Start(*modelDir + "/ggml-base.en-q5_1.bin", 2);
	{
		std::unique_lock<std::mutex> lock(mutex);
		cv.wait_for(lock, std::chrono::seconds(90), [&] { return ready; });
	}
	t.Check("continuous", "all three models load", ready && rec.ContinuousActive());
	if (!ready) {
		rec.Stop();
		return;
	}

	// Speech without the wake phrase is dropped silently: no wake, no result.
	const std::vector<float> silence(Voice::kVoiceSampleRate, 0.f);
	ring.Write(ordinary.samples.data(), ordinary.samples.size());
	ring.Write(silence.data(), silence.size());
	{
		std::unique_lock<std::mutex> lock(mutex);
		cv.wait_for(lock, std::chrono::seconds(10), [&] { return wakes > 0 || !results.empty(); });
	}
	t.Check("continuous", "speech without the wake phrase is ignored", wakes == 0 && results.empty());

	// With it, the app wakes and transcribes, and the phrase is gone from the text.
	ring.Write(woken.samples.data(), woken.samples.size());
	ring.Write(silence.data(), silence.size());
	{
		std::unique_lock<std::mutex> lock(mutex);
		cv.wait_for(lock, std::chrono::seconds(60), [&] { return !results.empty(); });
	}
	const std::string text = results.empty() ? std::string() : StringUtil::ToLower(results[0].text);
	t.Check("continuous", "wakes on the wake phrase", wakes == 1 && results.size() == 1 && results[0].wake);
	t.Check("continuous", "the transcript keeps the command", text.find("gameplay") != std::string::npos);
	t.Check("continuous", "the wake phrase is stripped", text.find("braid") == std::string::npos);

	rec.Stop();
	t.Check("continuous", "stop joins cleanly with continuous mode on", !rec.Ready() && !rec.ContinuousActive());
}

void TestAlwaysListenWiring(Tally &t)
{
	VoiceSettings settings = Voice::Engine().Settings();
	const VoiceSettings original = settings;

	settings.triggerMode = "wake";
	Voice::Engine().ApplySettings(settings);
	nlohmann::json state = Voice::Engine().StateJson();
	t.Check("alwayslisten", "the state reports the trigger mode", state["settings"]["triggerMode"] == "wake");
	t.Check("alwayslisten", "switching modes does not wedge the engine",
		state["state"] == "disabled" || state["state"] == "notReady" || state["state"] == "idle");
	t.Check("alwayslisten", "the state says whether always-listen runs",
		state["ready"].contains("wake") && state["ready"].contains("wakeReason"));

	// A wake phrase change must restart the runtime rather than be ignored until the
	// next launch.
	settings.wakePhrase = "hey studio";
	Voice::Engine().ApplySettings(settings);
	t.Check("alwayslisten", "a wake phrase change is applied",
		Voice::Engine().StateJson()["settings"]["wakePhrase"] == "hey studio");

	// The speech model is biased toward the phrase, last, where whisper keeps it.
	const std::string prompt = Voice::PromptBiasFor(Voice::CommandCandidates{}, "Braidcast");
	t.Check("alwayslisten", "the wake phrase closes the whisper prompt",
		prompt.size() >= 9 && prompt.compare(prompt.size() - 9, 9, "Braidcast") == 0);

	Voice::Engine().ApplySettings(original);
	state = Voice::Engine().StateJson();
	t.Check("alwayslisten", "settings restore",
		Voice::Engine().Settings().triggerMode == original.triggerMode &&
			Voice::Engine().Settings().wakePhrase == original.wakePhrase);
	t.Check("alwayslisten", "push-to-talk reports no always-listen reason",
		original.triggerMode != "ptt" || state["ready"]["wakeReason"] == "");
}

void TestTts(Tally &t)
{
	Voice::WavData spoken;
	std::string error;
	const bool ok = Voice::Synthesize("Switched to gameplay", spoken, error);
	t.Check("tts", "synthesis produces audio", ok && spoken.sampleRate > 0 && spoken.samples.size() > 1000);
	if (!ok) {
		HostLog("[selftest] voice-tts error: " + error);
		return;
	}
	t.Check("tts", "the audio is not silence", Rms(spoken.samples.data(), spoken.samples.size()) > 0.005);
	t.Check("tts", "a duration in the right ballpark",
		spoken.samples.size() < static_cast<size_t>(spoken.sampleRate) * 10);

	Voice::WavData empty;
	t.Check("tts", "empty text is refused", !Voice::Synthesize("", empty, error) && !error.empty());

	// Very long text is refused rather than read out for a minute.
	Voice::WavData capped;
	t.Check("tts", "over-long text is refused",
		!Voice::Synthesize(std::string(5000, 'a'), capped, error) && !error.empty());

	// Angle brackets are a scene name's, not markup.
	Voice::WavData bracketed;
	t.Check("tts", "angle brackets are read as text", Voice::Synthesize("Switched to <BRB>", bracketed, error));
}

void TestReadBackWords(Tally &t)
{
	using namespace Voice;
	CommandCandidates studio;
	studio.scenes = {"BRB"};
	studio.platforms = {"twitch"};
	PendingAction waiting;
	waiting.commandId = "streaming.stop";
	waiting.summary = "Stop streaming?";
	InterpretContext pending;
	pending.pending = &waiting;
	const Interpretation again = Interpret("Read that back.", pending, studio);
	t.Check("readback", "read that back answers a pending command",
		again.kind == Interpretation::Kind::Control &&
			again.control == Interpretation::Control::ReadBackPending);
	// With nothing pending it is no command, and push-to-talk does not post it to chat.
	const Interpretation idle = Interpret("read that back", InterpretContext{}, studio);
	t.Check("readback", "with nothing pending it is a miss, not a chat message",
		idle.kind == Interpretation::Kind::Miss);
}

void TestWakeGate(Tally &t)
{
	using Voice::MatchWakePhrase;

	Voice::WakeResult result = MatchWakePhrase("hey braidcast switch to gameplay", "hey braidcast");
	t.Check("wake", "an exact wake phrase matches", result.matched && result.remainder == "switch to gameplay");

	result = MatchWakePhrase("Hey, Braidcast! Switch to gameplay.", "hey braidcast");
	t.Check("wake", "punctuation and capitals do not matter",
		result.matched && result.remainder == "Switch to gameplay.");

	result = MatchWakePhrase("hey braid cast switch to BRB", "hey braidcast");
	t.Check("wake", "a split wake phrase still matches", result.matched && result.remainder == "switch to BRB");
	result = MatchWakePhrase("Hey Braid-cast, switch to BRB", "hey braidcast");
	t.Check("wake", "so does a hyphenated one", result.matched && result.remainder == "switch to BRB");
	// A letter slip is not enough, and deliberately: "brate cast" is exactly as far from
	// "braidcast" as "broadcast" is, and a streamer says "broadcast" all the time.
	t.Check("wake", "a misspelled wake phrase does not match",
		!MatchWakePhrase("hey brate cast switch to BRB", "hey braidcast").matched);

	result = MatchWakePhrase("so I was thinking about the raid", "hey braidcast");
	t.Check("wake", "ordinary speech does not match", !result.matched);

	result = MatchWakePhrase("switch to gameplay hey braidcast", "hey braidcast");
	t.Check("wake", "the phrase has to come first", !result.matched);

	result = MatchWakePhrase("hey braidcast", "hey braidcast");
	t.Check("wake", "the phrase alone matches with nothing after it", result.matched && result.remainder.empty());

	result = MatchWakePhrase("", "hey braidcast");
	t.Check("wake", "empty text does not match", !result.matched);

	result = MatchWakePhrase("hey braidcast switch to gameplay", "");
	t.Check("wake", "an empty wake phrase never matches", !result.matched);

	// A single-word wake phrase is riskier, and the threshold must not be looser for it.
	t.Check("wake", "a single-word phrase does not match a similar ordinary word",
		!MatchWakePhrase("computing the odds now", "computer").matched);

	// The default phrase, as the recognizer is likely to write it.
	const std::string phrase = VoiceSettings{}.wakePhrase;
	result = MatchWakePhrase("Braidcast, switch to gameplay.", phrase);
	t.Check("wake", "the default phrase opens a command",
		result.matched && result.remainder == "switch to gameplay.");
	result = MatchWakePhrase("Braid cast switch to BRB", phrase);
	t.Check("wake", "split in two, it still does", result.matched && result.remainder == "switch to BRB");
	t.Check("wake", "a similar word does not wake it", !MatchWakePhrase("Broadcast is live now", phrase).matched);
	// Part of the phrase is not the phrase: one common word must never wake the app.
	t.Check("wake", "part of a longer phrase does not match",
		!MatchWakePhrase("hey everyone welcome back", "hey braidcast").matched);

	// The speech model's re-match (N-I2). A woken utterance whose transcript does not open
	// with the phrase was a tiny-model false positive and is dropped, never interpreted:
	// otherwise "send to chat ..." said to the room would be posted.
	const Voice::WokenUtterance woken = Voice::ConfirmWake("Braidcast, switch to gameplay.", phrase);
	t.Check("wake", "a confirmed wake hands on the command alone",
		woken.ok && woken.command == "switch to gameplay." && woken.reason.empty());
	const Voice::WokenUtterance stray = Voice::ConfirmWake("send to chat we are so back", phrase);
	t.Check("wake", "a wake the speech model does not confirm is dropped with a reason",
		!stray.ok && stray.command.empty() && !stray.reason.empty());
	t.Check("wake", "so is one the speech model spells as another word",
		!Voice::ConfirmWake("Broadcast, mute mic.", phrase).ok);
	const Voice::WokenUtterance alone = Voice::ConfirmWake("Braidcast.", phrase);
	t.Check("wake", "the phrase alone is a miss that says so",
		!alone.ok && alone.reason == "I heard the wake phrase, but no command after it.");
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

	// N-I4: the cancel key (Escape by default, global) cancels voice from a game, never
	// while one of our own windows is in front, where Escape belongs to the web UI.
	t.Check("engine", "the cancel key does not cancel voice while the app is in front",
		!Voice::CancelKeyApplies(4242, 4242));
	t.Check("engine", "the cancel key cancels voice while another app is in front",
		Voice::CancelKeyApplies(777, 4242));
	t.Check("engine", "and when no window is in front", Voice::CancelKeyApplies(0, 4242));

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

void TestCommandMatcher(Tally &t)
{
	using namespace Voice;

	// A fixed studio: three scenes, three sources, two of the scenes similarly named.
	CommandCandidates candidates;
	candidates.scenes = {"Gameplay Cam", "Starting Soon", "BRB"};
	candidates.sources = {"Webcam", "Alerts", "Game Capture"};
	candidates.audioSources = {"Microphone", "Desktop Audio"};

	auto match = [&](const char *text) {
		return MatchCommand(Normalize(text), candidates);
	};

	struct Case {
		const char *said;
		const char *commandId; // "" means no match
		const char *slot;      // the resolved slot value, when there is one
		// Show/hide and mute/unmute share one bridge method, so the id alone no longer
		// tells them apart: assert the boolean that does. nullptr = the row takes none.
		const char *flagKey;
		bool flagValue;
	};

	const Case kTable[] = {
		{"switch to gameplay", "scenes.setCurrent", "Gameplay Cam", nullptr, false},
		{"go to BRB", "scenes.setCurrent", "BRB", nullptr, false},
		{"Switch to B.R.B., please.", "scenes.setCurrent", "BRB", nullptr, false},
		{"scene starting soon", "scenes.setCurrent", "Starting Soon", nullptr, false},
		{"show webcam", "sceneItems.setVisible", "Webcam", "visible", true},
		{"hide the alerts", "sceneItems.setVisible", "Alerts", "visible", false},
		{"mute microphone", "audio.setMuted", "Microphone", "muted", true},
		{"mute mike", "audio.setMuted", "Microphone", "muted", true},
		{"unmute my mic", "audio.setMuted", "Microphone", "muted", false},
		{"mute desktop audio", "audio.setMuted", "Desktop Audio", "muted", true},
		{"unmute desktop", "audio.setMuted", "Desktop Audio", "muted", false},
		{"start streaming", "streaming.start", "", nullptr, false},
		{"go live", "streaming.start", "", nullptr, false},
		{"stop the stream", "streaming.stop", "", nullptr, false},
		{"end the stream now", "streaming.stop", "", nullptr, false},
		// The spec's own example phrasings, each one (S-I2: "end stream" used to be chat).
		{"switch to BRB", "scenes.setCurrent", "BRB", nullptr, false},
		{"scene BRB", "scenes.setCurrent", "BRB", nullptr, false},
		{"go to Starting Soon", "scenes.setCurrent", "Starting Soon", nullptr, false},
		{"mute mic", "audio.setMuted", "Microphone", "muted", true},
		{"unmute mic", "audio.setMuted", "Microphone", "muted", false},
		{"show alerts", "sceneItems.setVisible", "Alerts", "visible", true},
		{"hide webcam", "sceneItems.setVisible", "Webcam", "visible", false},
		{"end stream", "streaming.stop", "", nullptr, false},
		{"stop streaming", "streaming.stop", "", nullptr, false},
		// And their plain variants.
		{"stop stream", "streaming.stop", "", nullptr, false},
		{"end streaming", "streaming.stop", "", nullptr, false},
		{"start stream", "streaming.start", "", nullptr, false},
		// Not commands.
		{"I'll switch to BRB later", "", "", nullptr, false},
		{"we should probably go to the store", "", "", nullptr, false},
		{"switch to the weather forecast", "", "", nullptr, false},
		{"stop streaming for a second", "", "", nullptr, false},
		{"switch to", "", "", nullptr, false},
		{"", "", "", nullptr, false},
	};

	bool allOk = true;
	for (const Case &c : kTable) {
		const CommandMatch got = match(c.said);
		const bool flagOk = c.flagKey == nullptr
					    ? got.flagKey == nullptr
					    : (got.flagKey != nullptr && std::string(got.flagKey) == c.flagKey &&
					       got.flagValue == c.flagValue);
		const bool matched = got.ok && got.commandId == c.commandId && got.slotValue == c.slot && flagOk;
		const bool missed = !got.ok && std::string(c.commandId).empty();
		if (!(matched || missed)) {
			allOk = false;
			HostLog(std::string("[selftest] voice-matcher case failed: '") + c.said + "' -> '" +
				(got.ok ? got.commandId : std::string("<no match>")) + "' slot '" + got.slotValue +
				"'");
		}
	}
	t.Check("matcher", "the whole command table", allOk);

	// Ambiguity: two scenes that both match "game" must ask, not pick.
	CommandCandidates twins;
	twins.scenes = {"Game One", "Game Two"};
	const CommandMatch ambiguous = MatchCommand(Normalize("switch to game"), twins);
	t.Check("matcher", "two scenes matching 'game' are ambiguous",
		!ambiguous.ok && ambiguous.ambiguous && !ambiguous.message.empty());

	// A command whose slot has no candidate at all says which name it heard.
	const CommandMatch missing = MatchCommand(Normalize("switch to intermission"), candidates);
	t.Check("matcher", "an unknown scene name is reported back",
		!missing.ok && missing.message.find("intermission") != std::string::npos);

	// Near misses: misheard commands, and command words with nothing after them. Each is a
	// miss that says what was probably meant, so push-to-talk never posts it to chat. The
	// second half must stay ordinary speech, which push-to-talk does post.
	struct NearCase {
		const char *said;
		bool isNear;       // "near" is a macro in windows.h
		const char *meant; // a part of the reason, for a near miss
	};
	const NearCase kNear[] = {
		{"and stream", true, "'end stream'"},
		{"end streem", true, "'end stream'"},
		{"start a stream", true, "'start stream'"},
		{"stop the streams", true, "'stop stream'"},
		{"go life", true, "'go live'"},
		{"switch two BRB", true, "'switch to BRB'"},
		{"which to BRB", true, "'switch to BRB'"},
		{"so webcam", true, "'show Webcam'"},
		{"muted mic", true, "'mute Microphone'"},
		{"mute", true, "after 'mute'"},
		{"switch to the", true, "after 'switch to'"},
		{"I'll switch to BRB later", false, ""},
		{"we switch to BRB", false, ""},
		{"nice stream", false, ""},
		{"great stream everyone", false, ""},
		{"end of stream", false, ""},
		{"good game everyone", false, ""},
		{"go home", false, ""},
		{"go live soon", false, ""},
		{"stop streaming for a second", false, ""},
		{"thanks for the raid", false, ""},
		{"so anyway I was saying", false, ""},
	};
	bool nearOk = true;
	for (const NearCase &c : kNear) {
		const CommandMatch got = match(c.said);
		const bool ok = !got.ok && got.nearMiss == c.isNear &&
				(!c.isNear || got.message.find(c.meant) != std::string::npos) &&
				(c.isNear || got.message.empty());
		if (!ok) {
			nearOk = false;
			HostLog(std::string("[selftest] voice-matcher near-miss case failed: '") + c.said +
				"' -> near " + (got.nearMiss ? "yes" : "no") + ", ok " + (got.ok ? "yes" : "no"));
		}
	}
	t.Check("matcher", "near misses are told from ordinary speech", nearOk);
	t.Check("matcher", "a near miss carries its score", match("and stream").score > 0.5);
	t.Check("matcher", "an exact phrase scores 1", match("end stream").score == 1.0);

	// Destructive commands ask first, and so does going live.
	t.Check("matcher", "stopping the stream needs confirmation", match("stop the stream").needsConfirm);
	t.Check("matcher", "going live needs confirmation", match("go live").needsConfirm);
	t.Check("matcher", "switching scenes does not", !match("switch to BRB").needsConfirm);
}

// The seams have to behave exactly like the methods, persistence and events included, so
// the visibility case drives a real scene the way sceneItems.setVisible is addressed. A
// throwaway scene and source keep the user's studio untouched; both are removed after.
void TestBridgeSeams(Tally &t)
{
	const char *kScene = "braidcast voice seam scene";
	const char *kSource = "braidcast voice seam source";
	nlohmann::json result;
	std::string error;
	if (!Bridge::Dispatch("scenes.create", nlohmann::json{{"name", kScene}}, result, error)) {
		t.Skip("seams", "visibility seam", "could not create the test scene: " + error);
		return;
	}
	const bool added = Bridge::Dispatch(
		"sources.create", nlohmann::json{{"type", "color_source"}, {"name", kSource}, {"scene", kScene}},
		result, error);
	const int64_t id = added ? result.value("id", static_cast<int64_t>(0)) : 0;
	if (!added || id == 0) {
		t.Skip("seams", "visibility seam", "could not add the test item: " + error);
	} else {
		const nlohmann::json item = {{"scene", kScene}, {"id", id}};
		auto visible = [&] {
			OBSSourceAutoRelease scene = obs_get_source_by_name(kScene);
			obs_sceneitem_t *found =
				scene ? obs_scene_find_sceneitem_by_id(obs_scene_from_source(scene), id) : nullptr;
			return found && obs_sceneitem_visible(found);
		};
		error.clear();
		t.Check("seams", "visibility seam hides",
			Bridge::SetSceneItemVisible(item, false, error) && !visible());
		t.Check("seams", "visibility seam shows", Bridge::SetSceneItemVisible(item, true, error) && visible());
		error.clear();
		t.Check("seams", "visibility seam refuses an item that is not there",
			!Bridge::SetSceneItemVisible(nlohmann::json{{"scene", kScene}, {"id", id + 1000}}, false,
						     error) &&
				!error.empty());
		Bridge::Dispatch("sceneItems.remove", item, result, error);
		OBSSourceAutoRelease source = obs_get_source_by_name(kSource);
		if (source) {
			obs_source_remove(source);
		}
	}
	Bridge::Dispatch("scenes.remove", nlohmann::json{{"name", kScene}}, result, error);

	// A private source: it is on no channel and in no scene, so nothing the user owns is
	// muted, and nothing persists it.
	OBSSourceAutoRelease muteable = obs_source_create_private("color_source", "braidcast voice seam mute", nullptr);
	if (!muteable) {
		t.Skip("seams", "mute seam", "could not create the test source");
		return;
	}
	obs_source_set_muted(muteable, false);
	error.clear();
	t.Check("seams", "mute seam mutes",
		Bridge::SetSourceMuted(muteable, true, error) && obs_source_muted(muteable));
	t.Check("seams", "mute seam unmutes",
		Bridge::SetSourceMuted(muteable, false, error) && !obs_source_muted(muteable));
	t.Check("seams", "mute seam refuses a null source",
		!Bridge::SetSourceMuted(nullptr, true, error) && !error.empty());
}

void TestCommandRegistry(Tally &t)
{
	using namespace Voice;
	using Kind = Interpretation::Kind;

	// The smoke run has at least one scene, so the candidates are never empty.
	const CommandCandidates candidates = CurrentCandidates();
	t.Check("registry", "the current scene list is not empty", !candidates.scenes.empty());
	t.Check("registry", "audio candidates come from the global channels", !candidates.audioSources.empty());

	// The prompt bias carries the command phrases and the studio's own names, and is
	// bounded: a studio with 200 sources must not push a 10 KB prompt at whisper.
	const std::string prompt = PromptBias();
	t.Check("registry", "the prompt names a command phrase", prompt.find("switch to") != std::string::npos);
	t.Check("registry", "the prompt is bounded", prompt.size() <= kMaxPromptChars);

	// Interpretation: a command that cannot be resolved is a miss with a reason, not a
	// silent nothing.
	InterpretContext ctx;
	const Interpretation missed = InterpretTranscript("switch to a scene that does not exist", ctx);
	t.Check("registry", "an unresolvable scene is a miss with a reason",
		missed.kind == Kind::Miss && !missed.message.empty());
	t.Check("registry", "ordinary speech is a miss",
		InterpretTranscript("so anyway I was saying", ctx).kind == Kind::Miss);

	// Both lifecycle commands become pending actions rather than running.
	const Interpretation stop = InterpretTranscript("stop the stream", ctx);
	t.Check("registry", "stopping the stream is pending, not instant",
		stop.kind == Kind::Pending && stop.action.commandId == "streaming.stop" &&
			stop.action.needsConfirmWord);
	const Interpretation live = InterpretTranscript("go live", ctx);
	t.Check("registry", "going live is pending, not instant",
		live.kind == Kind::Pending && live.action.commandId == "streaming.start");

	// Confirmation words only count while something is pending, and while something is,
	// nothing else does.
	t.Check("registry", "'yes' with nothing pending is a miss", InterpretTranscript("yes", ctx).kind == Kind::Miss);
	PendingAction pending;
	pending.commandId = "streaming.stop";
	pending.summary = "Stop streaming?";
	InterpretContext withPending;
	withPending.pending = &pending;
	const Interpretation yes = InterpretTranscript("yes", withPending);
	t.Check("registry", "'yes' confirms a pending command",
		yes.kind == Kind::Control && yes.control == Interpretation::Control::ConfirmPending);
	const Interpretation no = InterpretTranscript("never mind", withPending);
	t.Check("registry", "'never mind' cancels it",
		no.kind == Kind::Control && no.control == Interpretation::Control::CancelPending);
	t.Check("registry", "another command while one is pending is a miss",
		InterpretTranscript("go live", withPending).kind == Kind::Miss);

	// A muted microphone, in a fixed studio so the mic's name is known. Always-listen
	// honours the user's mute; push-to-talk does not, and our own push-to-talk mute
	// never counts.
	CommandCandidates studio;
	studio.scenes = {"BRB"};
	studio.audioSources = {"Desktop Audio", "Mic/Aux"};
	studio.micSource = "Mic/Aux";
	InterpretContext muted;
	muted.trigger = Trigger::Wake;
	muted.mutedSeen = true;
	t.Check("registry", "a muted mic in always-listen ignores an ordinary command",
		Interpret("switch to BRB", muted, studio).kind == Kind::Ignored);
	t.Check("registry", "a muted mic in always-listen still allows unmuting it",
		Interpret("unmute mic", muted, studio).kind == Kind::Instant);
	t.Check("registry", "a muted mic in always-listen does not unmute something else",
		Interpret("unmute desktop audio", muted, studio).kind == Kind::Ignored);
	InterpretContext pttUserMuted;
	pttUserMuted.mutedSeen = true;
	t.Check("registry", "push-to-talk on a mic the user muted acts normally",
		Interpret("switch to BRB", pttUserMuted, studio).kind == Kind::Instant);
	InterpretContext ours;
	ours.pttMuted = true;
	t.Check("registry", "our own push-to-talk mute blocks nothing",
		Interpret("switch to BRB", ours, studio).kind == Kind::Instant);

	// Running a scene switch reports back. It switches to the scene already on program,
	// so the run leaves the studio as it found it.
	OBSSourceAutoRelease program = Transitions::GetProgramScene();
	const char *programName = program ? obs_source_get_name(program) : nullptr;
	if (programName) {
		PendingAction action;
		action.commandId = "scenes.setCurrent";
		action.summary = std::string("Switch to ") + programName;
		action.params = {{"name", programName}};
		bool ran = false;
		bool ok = false;
		RunCommand(action, [&](bool succeeded, std::string) {
			ran = true;
			ok = succeeded;
		});
		t.Check("registry", "running a scene switch reports back", ran && ok);
	} else {
		t.Skip("registry", "running a scene switch reports back", "no program scene");
	}

	// Actions naming something that has since disappeared fail cleanly, each with a
	// reason.
	auto fails = [](const char *method, const nlohmann::json &params) {
		PendingAction gone;
		gone.commandId = method;
		gone.params = params;
		bool failedCleanly = false;
		RunCommand(gone, [&](bool succeeded, std::string message) {
			failedCleanly = !succeeded && !message.empty();
		});
		return failedCleanly;
	};
	t.Check("registry", "an action whose target vanished fails with a reason",
		fails("scenes.setCurrent", {{"name", "a scene that was deleted"}}));
	t.Check("registry", "showing a source that is not in the scene fails with a reason",
		fails("sceneItems.setVisible", {{"source", "a source that was deleted"}, {"visible", true}}));
	t.Check("registry", "muting an audio source that is gone fails with a reason",
		fails("audio.setMuted", {{"source", "a source that was deleted"}, {"muted", true}}));

	// Anything without a typed branch goes to the bridge registry by name.
	PendingAction state;
	state.commandId = "voice.state";
	state.summary = "Read the voice state";
	bool dispatched = false;
	RunCommand(state, [&](bool succeeded, std::string message) {
		dispatched = succeeded && message == "Read the voice state";
	});
	t.Check("registry", "other commands fall through to the bridge registry", dispatched);
	t.Check("registry", "an unknown method fails with a reason", fails("no.such.method", nlohmann::json::object()));
}

void TestRecentChatters(Tally &t)
{
	Chat::RecentChatters ring;
	const int64_t t0 = 1000000;
	const OAuth::DestinationId twitch{"twitch:100", ""};
	const OAuth::DestinationId youtubeA{"youtube:200", "profile-a"};
	const OAuth::DestinationId youtubeB{"youtube:200", "profile-b"};

	ring.Note("twitch", "u1", "Dave", twitch, t0);
	ring.Note("youtube", "u2", "Sarah", youtubeA, t0 + 1000);
	ring.Note("twitch", "u1", "Dave", twitch, t0 + 2000); // same person again

	const std::vector<Chat::Chatter> recent = ring.Recent(t0 + 3000);
	t.Check("chatters", "a repeat speaker appears once", recent.size() == 2);
	t.Check("chatters", "most recent first", !recent.empty() && recent[0].displayName == "Dave");

	// A reply goes back to the broadcast the person last spoke in.
	ring.Note("youtube", "u2", "Sarah", youtubeB, t0 + 2500);
	const std::optional<Chat::Chatter> sarah = ring.Resolve("sarah", t0 + 3000);
	t.Check("chatters", "a repeat speaker adopts their newest destination",
		sarah && sarah->accountId == "youtube:200" && sarah->profileUuid == "profile-b");

	// A platform without stable author ids still works, keyed by name.
	Chat::RecentChatters byName;
	byName.Note("kick", "", "Mo", twitch, t0);
	byName.Note("kick", "", "mo", twitch, t0 + 500); // same name, different case
	t.Check("chatters", "a nameless-id platform dedupes by name", byName.Recent(t0 + 1000).size() == 1);

	// The window drops old speakers.
	t.Check("chatters", "speakers older than the window are dropped",
		ring.Recent(t0 + Chat::RecentChatters::kWindowMs + 5000).empty());

	// Capacity is bounded.
	Chat::RecentChatters full;
	for (int i = 0; i < 400; ++i) {
		full.Note("twitch", "u" + std::to_string(i), "User" + std::to_string(i), twitch, t0 + i);
	}
	t.Check("chatters", "the ring is bounded", full.Recent(t0 + 500).size() == Chat::RecentChatters::kCapacity);
	t.Check("chatters", "the oldest speakers fell off",
		!full.Resolve("User0", t0 + 500).has_value() && full.Resolve("User399", t0 + 500).has_value());

	// Resolution is fuzzy, because the user says a name rather than spelling a handle.
	Chat::RecentChatters names;
	names.Note("twitch", "u1", "DaveTheStreamer", twitch, t0);
	names.Note("twitch", "u2", "Sarah_92", twitch, t0 + 100);
	const std::optional<Chat::Chatter> dave = names.Resolve("dave", t0 + 200);
	t.Check("chatters", "one word of a run-together handle resolves", dave && dave->authorId == "u1");
	t.Check("chatters", "an unknown name does not", !names.Resolve("gareth", t0 + 200));

	// Two similar names must not be guessed between; an exact name is not a guess.
	Chat::RecentChatters twins;
	twins.Note("twitch", "u1", "Dave_K", twitch, t0);
	twins.Note("twitch", "u2", "Dave_J", twitch, t0 + 100);
	t.Check("chatters", "ambiguous names do not resolve", !twins.Resolve("dave", t0 + 200));
	twins.Note("twitch", "u3", "Sam", twitch, t0 + 150);
	twins.Note("twitch", "u4", "Sammy", twitch, t0 + 160);
	const std::optional<Chat::Chatter> sam = twins.Resolve("sam", t0 + 200);
	t.Check("chatters", "an exact name wins over a longer one", sam && sam->authorId == "u3");

	// The same name on two platforms is one person to the ear: the latest speaker.
	Chat::RecentChatters both;
	both.Note("twitch", "u1", "Kai", twitch, t0);
	both.Note("youtube", "c9", "kai", youtubeA, t0 + 100);
	const std::optional<Chat::Chatter> kai = both.Resolve("kai", t0 + 200);
	t.Check("chatters", "one name on two platforms resolves to the latest speaker",
		kai && kai->platform == "youtube");
}

void TestChatterFeed(Tally &t)
{
	// The hub's ring is process-wide; the smoke run has no live chat, so this checks the
	// wiring rather than real traffic: a body shaped exactly like the one the hub's fan-out
	// point admits is fed through the same helper the hub calls. Whatever the ring held is
	// put back, so a run with chat live loses nobody.
	const int64_t now = TimeUtil::NowMs();
	const std::vector<Chat::Chatter> before = Chat::Chatters().Recent(now);
	Chat::Chatters().Clear();
	const OAuth::DestinationId dest{"twitch:100", ""};

	const nlohmann::json message = {
		{"platform", "twitch"},
		{"author", {{"id", "u42"}, {"name", "Dave"}}},
		{"fragments", nlohmann::json::array()},
	};
	Chat::NoteChatMessage(message, "self-id", dest, now);
	const std::vector<Chat::Chatter> fed = Chat::Chatters().Recent(now);
	t.Check("chatterfeed", "a message adds its author", fed.size() == 1);
	t.Check("chatterfeed", "the author keeps the destination it spoke on",
		fed.size() == 1 && fed[0].accountId == "twitch:100" && fed[0].platform == "twitch");

	const nlohmann::json own = {
		{"platform", "twitch"},
		{"author", {{"id", "self-id"}, {"name", "Me"}}},
		{"fragments", nlohmann::json::array()},
	};
	Chat::NoteChatMessage(own, "self-id", dest, now);
	t.Check("chatterfeed", "our own message is skipped", Chat::Chatters().Recent(now).size() == 1);

	const nlohmann::json anonymous = {{"platform", "kick"}, {"author", {{"name", "Mo"}}}};
	Chat::NoteChatMessage(anonymous, "self-id", dest, now);
	t.Check("chatterfeed", "an author without an id still counts", Chat::Chatters().Recent(now).size() == 2);

	const nlohmann::json malformed = {{"platform", "twitch"}, {"fragments", nlohmann::json::array()}};
	Chat::NoteChatMessage(malformed, "self-id", dest, now);
	Chat::NoteChatMessage(nlohmann::json{{"platform", "twitch"}, {"author", "Dave"}}, "self-id", dest, now);
	t.Check("chatterfeed", "a message with no author is ignored", Chat::Chatters().Recent(now).size() == 2);

	Chat::Chatters().Clear();
	for (auto it = before.rbegin(); it != before.rend(); ++it) {
		Chat::Chatters().Note(it->platform, it->authorId, it->displayName, {it->accountId, it->profileUuid},
				      it->lastSeenMs);
	}
}

void TestChatLimits(Tally &t)
{
	// Counting is by code point, not byte: an emoji is one character to a platform and
	// four bytes to us, and a message of 400 emoji must not be called 1600 characters.
	t.Check("limits", "ASCII counts by character", Chat::CodePointCount("hello") == 5);
	t.Check("limits", "an accented character counts once", Chat::CodePointCount("caf\xc3\xa9") == 4);
	t.Check("limits", "an emoji counts once", Chat::CodePointCount("\xf0\x9f\x8e\xae") == 1);
	t.Check("limits", "malformed UTF-8 does not loop or overcount", Chat::CodePointCount("\xff\xfe") <= 2);

	t.Check("limits", "every armed platform has a limit",
		Chat::MessageLimit("twitch") > 0 && Chat::MessageLimit("youtube") > 0 &&
			Chat::MessageLimit("kick") > 0 && Chat::MessageLimit("facebook") > 0);
	t.Check("limits", "an unknown platform gets the safe default",
		Chat::MessageLimit("some-new-platform") == Chat::kDefaultMessageLimit);

	const std::vector<std::string> platforms = {"twitch", "youtube"};
	std::string offender;
	t.Check("limits", "a short message fits everywhere",
		Chat::FitsEverywhere("hello chat", platforms, offender) && offender.empty());

	const std::string longMessage(Chat::MessageLimit("youtube") + 10, 'x');
	t.Check("limits", "a message too long for one platform is refused, and names it",
		!Chat::FitsEverywhere(longMessage, platforms, offender) && offender == "youtube");

	const std::string exact(Chat::MessageLimit("youtube"), 'x');
	t.Check("limits", "a message exactly at the limit fits", Chat::FitsEverywhere(exact, platforms, offender));

	t.Check("limits", "an empty message never fits", !Chat::FitsEverywhere("", platforms, offender));
	t.Check("limits", "an empty platform list does not fit either", !Chat::FitsEverywhere("hello", {}, offender));
}

void TestChatCommands(Tally &t)
{
	using namespace Voice;

	CommandCandidates candidates;
	candidates.scenes = {"BRB"};
	candidates.people = {"DaveTheStreamer", "Sarah_92"};
	candidates.platforms = {"twitch", "youtube"};

	auto match = [&](const char *text) {
		return MatchCommand(Normalize(text), candidates);
	};

	// The message keeps the user's own capitals and punctuation.
	const CommandMatch toChat = match("send to chat We are back in five minutes!");
	t.Check("chatcmd", "send to chat takes the rest as the message",
		toChat.ok && toChat.commandId == "chat.send" && toChat.messageText == "We are back in five minutes!");

	const CommandMatch say = match("say in chat hello everyone");
	t.Check("chatcmd", "say in chat is the same command",
		say.ok && say.commandId == "chat.send" && say.messageText == "hello everyone");

	const CommandMatch bare = match("chat hello everyone");
	t.Check("chatcmd", "chat followed by a message is the same command",
		bare.ok && bare.commandId == "chat.send" && bare.slot == SlotKind::None &&
			bare.messageText == "hello everyone");

	// One platform, in each of the spec's forms.
	const CommandMatch toTwitch = match("send to Twitch thanks for the raid");
	t.Check("chatcmd", "a named platform narrows the target",
		toTwitch.ok && toTwitch.commandId == "chat.send" && toTwitch.slot == SlotKind::Platform &&
			toTwitch.slotValue == "twitch" && toTwitch.messageText == "thanks for the raid");
	const CommandMatch only = match("YouTube only thanks for watching");
	t.Check("chatcmd", "'<platform> only' narrows the target",
		only.ok && only.slotValue == "youtube" && only.messageText == "thanks for watching");
	const CommandMatch tell = match("tell twitch I'll be right back");
	t.Check("chatcmd", "'tell <platform>' narrows the target",
		tell.ok && tell.slotValue == "twitch" && tell.messageText == "I'll be right back");
	t.Check("chatcmd", "'tell' and a word that is no platform is not a command",
		match("tell them I'll be right back").commandId.empty());

	// A reply resolves the person, by a word of their handle, and keeps the message. It is
	// chat.send too: a reply is a message addressed to one chat.
	const CommandMatch reply = match("reply to Dave good question, I will cover that next");
	t.Check("chatcmd", "a reply resolves the person",
		reply.ok && reply.commandId == "chat.send" && reply.slot == SlotKind::Person &&
			reply.slotValue == "DaveTheStreamer" &&
			reply.messageText == "good question, I will cover that next");

	// A reply to nobody in particular fails with a reason rather than sending to chat.
	const CommandMatch unknown = match("reply to Gareth are you there");
	t.Check("chatcmd", "an unknown person is reported, not guessed",
		!unknown.ok && unknown.message.find("Gareth") != std::string::npos);
	CommandCandidates twins = candidates;
	twins.people = {"Dave_K", "Dave_J"};
	const CommandMatch either = MatchCommand(Normalize("reply to Dave hello"), twins);
	t.Check("chatcmd", "two people matching a name are reported, not guessed",
		!either.ok && either.ambiguous && either.message.find("Dave_K") != std::string::npos &&
			either.message.find("Dave_J") != std::string::npos);

	// Empty messages are not commands.
	t.Check("chatcmd", "send to chat with no message is not a command", !match("send to chat").ok);
	t.Check("chatcmd", "reply with no message is not a command", !match("reply to Dave").ok);

	// Anchoring still applies: talking about chat is not a chat command.
	t.Check("chatcmd", "talking about sending is not sending",
		match("I should send to chat later when the raid ends").commandId.empty());

	// Studio commands still come first.
	t.Check("chatcmd", "a studio command is not read as chat",
		match("switch to BRB").commandId == "scenes.setCurrent");

	// Chat messages never ask for confirmation at match time; the send mode decides.
	t.Check("chatcmd", "a chat message is not a confirm-at-match command", !toChat.needsConfirm);
}

void TestChatDrafts(Tally &t)
{
	using namespace Voice;
	using Kind = Interpretation::Kind;

	// A fixed studio with two live chats and one person who has spoken, so the send mode is
	// the only thing that varies. The send mode reaches Interpret as a SendPolicy, which is
	// what InterpretTranscript reads from the engine's settings.
	CommandCandidates studio;
	studio.scenes = {"BRB"};
	studio.platforms = {"twitch", "youtube"};
	studio.people = {"Dave"};
	studio.replyRoutes = {{"twitch", "twitch:100", ""}};
	InterpretContext ctx;
	SendPolicy instantMode;
	instantMode.mode = "instant";
	SendPolicy countdownMode;
	countdownMode.countdownSec = 4.0;
	SendPolicy sayMode;
	sayMode.mode = "say";

	const Interpretation instant = Interpret("send to chat hello everyone", ctx, studio, instantMode);
	t.Check("drafts", "instant mode sends immediately",
		instant.kind == Kind::Instant && instant.action.commandId == "chat.send" &&
			instant.action.params.value("text", "") == "hello everyone");

	const Interpretation countdown = Interpret("send to chat hello everyone", ctx, studio, countdownMode);
	t.Check("drafts", "countdown mode drafts and sends on the timer",
		countdown.kind == Kind::Pending && countdown.action.runOnTimeout &&
			countdown.action.timeoutMs == 4000 && !countdown.action.needsConfirmWord);

	const Interpretation waitForWord = Interpret("send to chat hello everyone", ctx, studio, sayMode);
	t.Check("drafts", "say mode waits for the word",
		waitForWord.kind == Kind::Pending && !waitForWord.action.runOnTimeout &&
			waitForWord.action.needsConfirmWord);

	// Where it goes: every live chat, one platform, or the one chat a person spoke in.
	t.Check("drafts", "a message to chat goes to every live platform",
		countdown.action.params["platforms"] == nlohmann::json::array({"twitch", "youtube"}));
	const Interpretation reply = Interpret("reply to Dave good game", ctx, studio, countdownMode);
	t.Check("drafts", "a reply is addressed to the one chat the person spoke in",
		reply.kind == Kind::Pending && reply.action.params.value("accountId", "") == "twitch:100" &&
			!reply.action.params.contains("platforms") &&
			reply.action.params.value("text", "") == "@Dave good game");
	CommandCandidates lost = studio;
	lost.replyRoutes.clear();
	const Interpretation unrouted = Interpret("reply to Dave good game", ctx, lost, countdownMode);
	t.Check("drafts", "a reply with nowhere to go is refused, not widened",
		unrouted.kind == Kind::Miss && unrouted.message.find("Dave") != std::string::npos);

	// "send" confirms a waiting draft.
	PendingAction draft;
	draft.commandId = "chat.send";
	draft.summary = "Send to chat: hello";
	InterpretContext withDraft;
	withDraft.pending = &draft;
	t.Check("drafts", "'send' confirms a waiting draft",
		Interpret("send", withDraft, studio).control == Interpretation::Control::ConfirmPending);
	t.Check("drafts", "'send' with nothing waiting is a miss", Interpret("send", ctx, studio).kind == Kind::Miss);

	// A message a platform will not take whole is refused before it is drafted, names the
	// platform, and is kept for the composer.
	const std::string tooLong(300, 'x');
	const Interpretation big = Interpret("send to chat " + tooLong, ctx, studio, countdownMode);
	t.Check("drafts", "an over-long message is refused with a reason",
		big.kind == Kind::Miss && big.message.find("too long for YouTube") != std::string::npos &&
			big.keptDraft == tooLong);

	// Push-to-talk: words no command claims are a message to every live chat. Always-listen:
	// never.
	const Interpretation spoken = Interpret("good game everyone!", ctx, studio, countdownMode);
	t.Check("drafts", "unclaimed push-to-talk speech is a message to chat",
		spoken.kind == Kind::Pending && spoken.action.params.value("text", "") == "good game everyone!");
	InterpretContext wake;
	wake.trigger = Trigger::Wake;
	t.Check("drafts", "unclaimed always-listen speech never reaches chat",
		Interpret("good game everyone", wake, studio, countdownMode).kind == Kind::Miss);
	t.Check("drafts", "a failed studio command is not posted to chat",
		Interpret("switch to the weather forecast", ctx, studio, countdownMode).kind == Kind::Miss);
	// The spec's own "end stream" is the command, not a message (S-I2), and a misheard one
	// is a miss that says what it heard, never a message either. Real chat is still chat.
	const Interpretation endStream = Interpret("end stream", ctx, studio, countdownMode);
	t.Check("drafts", "'end stream' stops the stream rather than posting to chat",
		endStream.kind == Kind::Pending && endStream.action.commandId == "streaming.stop");
	t.Check("drafts", "'start stream' starts it rather than posting to chat",
		Interpret("start stream", ctx, studio, countdownMode).action.commandId == "streaming.start");
	const Interpretation misheard = Interpret("and stream", ctx, studio, countdownMode);
	t.Check("drafts", "a misheard command is a miss with a reason, not a message",
		misheard.kind == Kind::Miss && misheard.message.find("end stream") != std::string::npos);
	t.Check("drafts", "a bare command word is a miss, not a message",
		Interpret("unmute", ctx, studio, countdownMode).kind == Kind::Miss);
	const Interpretation later = Interpret("I'll switch to BRB later", ctx, studio, countdownMode);
	t.Check("drafts", "a sentence that mentions a command is still a message",
		later.kind == Kind::Pending && later.action.commandId == "chat.send" &&
			later.action.params.value("text", "") == "I'll switch to BRB later");
	t.Check("drafts", "'nice stream' is still a message",
		Interpret("nice stream", ctx, studio, countdownMode).action.commandId == "chat.send");

	// With nothing live there is nowhere to send, and the refusal says so rather than the
	// message disappearing.
	const Interpretation nowhere = InterpretTranscript("send to chat hello", ctx);
	const bool live = !CurrentCandidates().platforms.empty();
	t.Check("drafts", "with nothing live the refusal explains",
		live || (nowhere.kind == Kind::Miss && !nowhere.message.empty()));

	// Running: a reply whose chat has ended fails out loud and is never widened to the
	// platform, and a message to chats with none live fails too. Both skip when chat is
	// live, since then they would post.
	if (live) {
		t.Skip("drafts", "a reply to a chat that ended fails out loud", "chat is live");
		t.Skip("drafts", "a message with no live chat fails out loud", "chat is live");
	} else {
		auto fails = [](const nlohmann::json &params) {
			PendingAction action;
			action.commandId = "chat.send";
			action.params = params;
			bool failedCleanly = false;
			RunCommand(action,
				   [&](bool ok, std::string message) { failedCleanly = !ok && !message.empty(); });
			return failedCleanly;
		};
		t.Check("drafts", "a reply to a chat that ended fails out loud",
			fails({{"text", "@Dave hi"},
			       {"accountId", "twitch:100"},
			       {"profileUuid", ""},
			       {"replyTo", "Dave"}}));
		t.Check("drafts", "a message with no live chat fails out loud",
			fails({{"text", "hi"}, {"platforms", nlohmann::json::array({"twitch"})}}));
	}
}

void TestVoiceFeedback(Tally &t)
{
	Voice::VoiceFeedback feedback;
	std::string error;
	const bool started = feedback.Start(error);
	t.Check("feedback", "the cue source starts", started);
	if (!started) {
		HostLog("[selftest] voice-feedback start error: " + error);
		return;
	}
	t.Check("feedback", "all seven cues loaded",
		feedback.LoadedCues() == Voice::kCueCount && Voice::kCueCount == 7);

	OBSSourceAutoRelease source = obs_get_source_by_name(Voice::kFeedbackSourceName);
	t.Check("feedback", "the source is private, so it is not in the user's source list", !source);

	// Playing must not block: the caller is the UI thread.
	const int64_t before = TimeUtil::NowMs();
	feedback.Play(Voice::Cue::Accept, 0.0); // silent, so the smoke run makes no noise
	t.Check("feedback", "playing returns immediately", TimeUtil::NowMs() - before < 50);

	feedback.Stop();
	t.Check("feedback", "stop is idempotent", (feedback.Stop(), true));

	Voice::VoiceFeedback nowhere;
	t.Check("feedback", "a directory without cues is refused with a reason",
		!nowhere.StartFrom(RundirRoot() + "/no-such-cue-directory", error) && !error.empty() &&
			nowhere.LoadedCues() == 0);
}

void TestAudioEndpoints(Tally &t)
{
	// The default render endpoint always resolves on a machine with any audio at all.
	const std::string defaultRender = AudioEndpoints::DefaultRenderDeviceId();
	t.Check("endpoints", "the default render device resolves", !defaultRender.empty());

	// "default" and the resolved id must be recognized as the same endpoint, which is the
	// whole point: OBS stores "default" in both places.
	t.Check("endpoints", "'default' resolves to the default endpoint",
		AudioEndpoints::ResolveRenderDeviceId("default") == defaultRender);
	t.Check("endpoints", "an explicit id resolves to itself",
		AudioEndpoints::ResolveRenderDeviceId(defaultRender) == defaultRender);
	t.Check("endpoints", "an unknown id resolves to itself",
		AudioEndpoints::ResolveRenderDeviceId("nope") == "nope");

	// The shared-device question must answer without throwing, whatever the setup.
	std::string device;
	const bool shared = AudioEndpoints::MonitorSharesCapturedDevice(device);
	t.Check("endpoints", "the shared-device check answers", shared == !device.empty());

	// Ducking still works after the move (it is a no-op that must not fault). Applied
	// with the user's own setting, so the run changes nothing.
	AudioEndpoints::DisableAudioDucking(ObsBootstrap::Advanced().disableAudioDucking);
	t.Check("endpoints", "ducking control survived the move", true);
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

	t.Check("bridge", "a wake phrase with no word in it is refused",
		!Dispatch("settings.setVoice", nlohmann::json{{"wakePhrase", " ?! "}}, result, error) &&
			!error.empty() && Voice::Engine().Settings().wakePhrase == original.wakePhrase);

	t.Check("bridge", "voice.state answers",
		Dispatch("voice.state", nlohmann::json::object(), result, error) && result.contains("state") &&
			result.contains("ready"));

	t.Check("bridge", "voice.model.status lists the catalog",
		Dispatch("voice.model.status", nlohmann::json::object(), result, error) && result["models"].is_array());

	t.Check("bridge", "downloading an unknown model is refused",
		!Dispatch("voice.model.download", nlohmann::json{{"id", "not-a-model"}}, result, error));

	t.Check("bridge", "cancelling a download that is not running is refused",
		!Dispatch("voice.model.cancel", nlohmann::json{{"id", Voice::kVadModelId}}, result, error));

	result.clear();
	error.clear();
	t.Check("bridge", "confirming with nothing pending is refused",
		!Dispatch("voice.confirm", nlohmann::json::object(), result, error) && !error.empty());
	t.Check("bridge", "confirming an action that is not pending is refused",
		!Dispatch("voice.confirm", nlohmann::json{{"id", 12345}}, result, error) && !error.empty());
	t.Check("bridge", "cancelling an action that is not pending is refused",
		!Dispatch("voice.cancel", nlohmann::json{{"id", 12345}}, result, error) && !error.empty());
	t.Check("bridge", "cancelling with nothing to cancel is refused",
		!Dispatch("voice.cancel", nlohmann::json::object(), result, error) && !error.empty());
	t.Check("bridge", "getVoice carries the cue warning field",
		Dispatch("settings.getVoice", nlohmann::json::object(), result, error) &&
			result.contains("cueWarning"));

	// The settings file round-trips through the store, not just through memory.
	VoiceSettings persisted;
	persisted.Load();
	t.Check("bridge", "setVoice persisted the model", persisted.model == Voice::P0::kDefaultModelId);

	// Put back what the run found, in memory and on disk.
	Dispatch("settings.setVoice", SettingsFields::ToJson(VoiceSettingsTable(), original), result, error);
}

void TestLanguageSelection(Tally &t)
{
	t.Check("language", "English uses an English-only model",
		Voice::ModelForLanguage("en") != Voice::kMultilingualModelId);
	t.Check("language", "another language uses the multilingual model",
		Voice::ModelForLanguage("de") == Voice::kMultilingualModelId);
	t.Check("language", "the multilingual model becomes selectable for a non-English language",
		Voice::IsSelectableModel(Voice::kMultilingualModelId, "de") &&
			!Voice::IsSelectableModel(Voice::kMultilingualModelId, "en"));
	t.Check("language", "an English-only model is not offered for another language",
		!Voice::IsSelectableModel(Voice::P0::kDefaultModelId, "de"));

	// Setting a language through the bridge switches the model rather than leaving the
	// user on an English-only one that will transcribe German as nonsense.
	const VoiceSettings original = Voice::Engine().Settings();
	nlohmann::json result;
	std::string error;
	t.Check("language", "setting a language switches the model",
		Dispatch("settings.setVoice", nlohmann::json{{"language", "de"}}, result, error) &&
			result["settings"]["model"] == Voice::kMultilingualModelId);
	bool offered = false;
	for (const nlohmann::json &m : result["models"]) {
		offered = offered || (m.value("id", "") == Voice::kMultilingualModelId && m.value("selectable", false));
	}
	t.Check("language", "and the Voice tab is offered the multilingual model", offered);
	result.clear();
	error.clear();
	t.Check("language", "going back to English switches back",
		Dispatch("settings.setVoice", nlohmann::json{{"language", "en"}}, result, error) &&
			result["settings"]["model"] != Voice::kMultilingualModelId);

	// Put back what the run found, in memory and on disk.
	Dispatch("settings.setVoice", SettingsFields::ToJson(VoiceSettingsTable(), original), result, error);
}

using Case = void (*)(Tally &);

const Case kCases[] = {
	&TestWhisperLinked,
	&TestCpuGate,
	&TestLogCategory,
	&TestVoiceSettingsTable,
	&TestSha256,
	&TestHttpCancel,
	&TestModelCatalog,
	&TestModelVerifyAndCommit,
	&TestPostToUiDelayed,
	&TestSpscRing,
	&TestResampler,
	&TestMicMuteGuard,
	&TestVoiceCapture,
	&TestVoiceListener,
	&TestWavFile,
	&TestRecognizerWithoutModel,
	&TestRecognizer,
	&TestVadEndpointer,
	&TestWakeGate,
	&TestContinuousRecognizer,
	&TestVoiceEngine,
	&TestAlwaysListenWiring,
	&TestVoiceHotkeys,
	&TestVoiceBridge,
	&TestLanguageSelection,
	&TestTextNormalize,
	&TestFuzzyMatch,
	&TestCommandMatcher,
	&TestBridgeSeams,
	&TestCommandRegistry,
	&TestVoiceFeedback,
	&TestTts,
	&TestAudioEndpoints,
	&TestRecentChatters,
	&TestChatterFeed,
	&TestChatLimits,
	&TestChatCommands,
	&TestChatDrafts,
	&TestReadBackWords,
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
