#include "log.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>

// Forward-declared rather than pulling in <obs.h>: this lean logging TU would
// otherwise drag in the graphics headers, whose nameless-union warnings are
// -Werror in the frontend target. Links against libobs's C export.
extern "C" void obs_set_render_debug(bool enabled);

namespace {
// The single process-wide DEBUG gate. Default OFF: gated DBG() calls cost one
// relaxed atomic read until the boot seed or diagnostics.setDebug flips it.
std::atomic<bool> g_debugEnabled{false};
} // namespace

namespace Log {

bool DebugEnabled()
{
	return g_debugEnabled.load(std::memory_order_relaxed);
}

void SetDebug(bool enabled)
{
	g_debugEnabled.store(enabled, std::memory_order_relaxed);
	// Flip render-thread composite timing alongside the DEBUG gate. This is the
	// single seam both the boot seed and the live diagnostics.setDebug toggle
	// funnel through; obs_set_render_debug no-ops safely before obs startup.
	obs_set_render_debug(enabled);
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
