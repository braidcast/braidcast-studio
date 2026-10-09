#include "voice/VoiceCpu.hpp"

#include <immintrin.h>
#include <intrin.h>

namespace Voice {

namespace {

// One required feature bit. `reg` indexes __cpuidex's output: 1 = EBX, 2 = ECX.
struct CpuBit {
	int leaf;
	int reg;
	int bit;
	const char *name;
};

constexpr int kEbx = 1;
constexpr int kEcx = 2;

// OSXSAVE first: _xgetbv below is only legal once it is known to be present.
constexpr CpuBit kRequired[] = {
	{1, kEcx, 27, "OSXSAVE"}, {1, kEcx, 28, "AVX"}, {1, kEcx, 12, "FMA"},
	{1, kEcx, 29, "F16C"},    {7, kEbx, 5, "AVX2"}, {7, kEbx, 8, "BMI2"},
};

// XCR0 bits 1 (SSE state) and 2 (AVX state): the OS saves the YMM registers.
constexpr unsigned long long kXcr0AvxState = 0x6;

} // namespace

bool CpuSupportsVoice(std::string &reason)
{
	int info[4] = {};
	__cpuid(info, 0);
	const int maxLeaf = info[0];
	for (const CpuBit &b : kRequired) {
		int regs[4] = {};
		if (b.leaf <= maxLeaf) {
			__cpuidex(regs, b.leaf, 0);
		}
		if (b.leaf > maxLeaf || (regs[b.reg] & (1 << b.bit)) == 0) {
			reason = std::string("Voice control needs a CPU with ") + b.name +
				 " support (Intel Haswell / AMD Excavator, 2013, or newer).";
			return false;
		}
	}
	if ((_xgetbv(0) & kXcr0AvxState) != kXcr0AvxState) {
		reason = "Windows has not enabled AVX on this CPU, which voice control needs.";
		return false;
	}
	reason.clear();
	return true;
}

} // namespace Voice
