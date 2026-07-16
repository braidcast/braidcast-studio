#include "log.hpp"

#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>

// Forward-declared rather than pulling in <obs.h>: this lean logging TU would
// otherwise drag in the graphics headers, whose nameless-union warnings are
// -Werror in the frontend target. Links against libobs's C export.
extern "C" void obs_set_render_debug(bool enabled);

namespace {
// The single process-wide DEBUG gate, one bit per LogCat. Default OFF (no bits):
// gated DBG() calls cost one relaxed atomic read plus a bit test until the boot
// seed or diagnostics.setDebug sets it.
std::atomic<Log::CatMask> g_debugMask{Log::kNoCats};

std::string LowerTrim(const std::string &s)
{
	const size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) {
		return std::string();
	}
	std::string out = s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
	for (char &c : out) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}
} // namespace

namespace Log {

bool DebugEnabled()
{
	return g_debugMask.load(std::memory_order_relaxed) != kNoCats;
}

bool DebugEnabled(LogCat cat)
{
	return (g_debugMask.load(std::memory_order_relaxed) & CatBit(cat)) != 0;
}

CatMask DebugMask()
{
	return g_debugMask.load(std::memory_order_relaxed);
}

void SetDebugMask(CatMask mask)
{
	g_debugMask.store(mask, std::memory_order_relaxed);
	// Flip render-thread composite timing on ONLY when the render category is set.
	// This is the single seam the boot seed, the live diagnostics.setDebug toggle,
	// and a filtered env spec all funnel through; obs_set_render_debug no-ops safely
	// before obs startup. Gated on LogCat::Render (not the coarse any-category gate)
	// so the binary debug toggle doesn't flood the log with per-frame [render-debug]
	// lines; opt in with BRAIDCAST_DEBUG=render.
	obs_set_render_debug((mask & CatBit(LogCat::Render)) != 0);
}

void SetDebug(bool enabled)
{
	SetDebugMask(enabled ? kDefaultCats : kNoCats);
}

CatMask ParseDebugSpec(const std::string &spec)
{
	const std::string v = LowerTrim(spec);
	if (v.empty() || v == "0" || v == "false" || v == "off") {
		return kNoCats;
	}
	if (v == "1" || v == "true" || v == "on" || v == "all") {
		// "all" means every human-facing category, NOT the render-thread firehose;
		// name it explicitly (BRAIDCAST_DEBUG=render) to enable [render-debug].
		return kDefaultCats;
	}

	CatMask mask = kNoCats;
	size_t pos = 0;
	while (pos <= v.size()) {
		size_t end = v.find_first_of(", ", pos);
		if (end == std::string::npos) {
			end = v.size();
		}
		const std::string name = LowerTrim(v.substr(pos, end - pos));
		LogCat cat{};
		if (!name.empty() && LogCatFromName(name, cat)) {
			mask |= CatBit(cat);
		}
		pos = end + 1;
	}
	return mask;
}

void Debug(LogCat cat, const char *fmt, ...)
{
	char body[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(body, sizeof(body), fmt, args);
	va_end(args);

	// Build "[<cat>] <formatted>" then emit through blog with a "%s" format so no
	// stray '%' in the expanded message is reinterpreted.
	std::string line = "[";
	line += LogCatName(cat);
	line += "] ";
	line += body;
	blog(LOG_DEBUG, "%s", line.c_str());
}

} // namespace Log
