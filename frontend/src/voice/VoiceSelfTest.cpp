#include "obs_bootstrap.hpp"

#include "log.hpp"
#include "voice/VoiceCpu.hpp"
#include "voice/VoiceSettings.hpp"

#include <obs.hpp>
#include <whisper.h>

#include <string>

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

using Case = void (*)(Tally &);

const Case kCases[] = {
	&TestWhisperLinked,
	&TestCpuGate,
	&TestLogCategory,
	&TestVoiceSettingsTable,
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
