#include "obs_bootstrap.hpp"

#include "log.hpp"
#include "util/file_util.hpp"
#include "util/async_task.hpp"
#include "util/http_client.hpp"
#include "util/sha256.hpp"
#include "util/time_util.hpp"
#include "voice/VoiceCpu.hpp"
#include "voice/VoiceModels.hpp"
#include "voice/VoiceRing.hpp"
#include "voice/VoiceSettings.hpp"

#include <obs.hpp>
#include <whisper.h>

#include <atomic>
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

using Case = void (*)(Tally &);

const Case kCases[] = {
	&TestWhisperLinked, &TestCpuGate,      &TestLogCategory,          &TestVoiceSettingsTable, &TestSha256,
	&TestHttpCancel,    &TestModelCatalog, &TestModelVerifyAndCommit, &TestPostToUiDelayed,    &TestSpscRing,
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
