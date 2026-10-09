#include "obs_bootstrap.hpp"

#include "log.hpp"
#include "voice/VoiceCpu.hpp"

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

using Case = void (*)(Tally &);

const Case kCases[] = {
	&TestWhisperLinked,
	&TestCpuGate,
	&TestLogCategory,
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
