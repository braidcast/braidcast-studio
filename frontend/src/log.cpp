#include "log.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>

#include "util/string_util.hpp"

// Forward-declared rather than pulling in <obs.h>: this lean logging TU would
// otherwise drag in the graphics headers, whose nameless-union warnings are
// -Werror in the frontend target. Links against libobs's C export.
extern "C" void obs_set_render_debug(bool enabled);
extern "C" void obs_set_render_gpu_debug(bool enabled);

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
	return StringUtil::ToLower(s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1));
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
	// Flip render-thread frame timing on ONLY when the render category is set.
	// This is the seam the live diagnostics.setDebug toggle and a filtered env spec
	// funnel through. It is not the only caller: obs_bootstrap.cpp calls
	// obs_set_render_debug directly after obs_startup, because the boot seed reaches
	// here while obs is still NULL and both calls below no-op in that state. Gated
	// on LogCat::Render (not the coarse any-category gate) so the
	// binary debug toggle doesn't flood the log with per-frame [render-debug] lines;
	// opt in with BRAIDCAST_DEBUG=1 BRAIDCAST_DEBUG_COMPONENTS=render (the master is
	// mandatory; a falsy BRAIDCAST_DEBUG yields an empty set before components parse).
	obs_set_render_debug((mask & CatBit(LogCat::Render)) != 0);

	// GPU timer queries are a separate opt-in on top: their per-frame readback runs
	// on the graphics thread, so leaving them off is what makes a control run
	// possible while the frame timing above is on. Must follow the call above --
	// source_profiler_gpu_enable ANDs against the CPU-side enable it sets.
	obs_set_render_gpu_debug((mask & CatBit(LogCat::RenderGpu)) != 0);
}

void SetDebug(bool enabled)
{
	SetDebugMask(enabled ? kDefaultCats : kNoCats);
}

DebugComponents ParseComponents(const std::string &spec)
{
	DebugComponents out;
	const std::string v = LowerTrim(spec);
	size_t pos = 0;
	while (pos <= v.size()) {
		size_t end = v.find_first_of(", ", pos);
		if (end == std::string::npos) {
			end = v.size();
		}
		const std::string name = LowerTrim(v.substr(pos, end - pos));
		if (!name.empty()) {
			LogCat cat{};
			if (name == "all") {
				// Every human-facing category, NOT the render-thread firehose;
				// name "render" explicitly to opt [render-debug] in.
				out.logMask |= kDefaultCats;
			} else if (name == "gpudiag") {
				out.gpuDiag = true;
			} else if (LogCatFromName(name, cat)) {
				out.logMask |= CatBit(cat);
			}
		}
		pos = end + 1;
	}
	return out;
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
