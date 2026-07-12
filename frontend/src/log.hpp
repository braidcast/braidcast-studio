#ifndef OBS_MULTISTREAM_FRONTEND_LOG_HPP_
#define OBS_MULTISTREAM_FRONTEND_LOG_HPP_

#include <windows.h>

#include <util/base.h>

#include <string>

// Curated category tags for the gated DEBUG channel. Each value maps to a stable
// lowercase name via LogCatName; the DBG() macro prefixes every gated line with
// "[<name>] ". Adding a category = one entry here + one in LogCatName.
// keep in sync with frontend/web/src/lib/logCategories.ts
enum class LogCat {
	Lifecycle,
	Bridge,
	Preview,
	Canvas,
	Stream,
	Encode,
	Audio,
	OAuth,
	Chat,
	Events,
	Overlay,
	Import,
	Scene,
	Mcp,
	Net,
	Cef,
};

// Lowercase, stable name for a category: the tag written into the log line and
// the wire name shared with the web logger.
// keep in sync with frontend/web/src/lib/logCategories.ts
inline const char *LogCatName(LogCat cat)
{
	switch (cat) {
	case LogCat::Lifecycle:
		return "lifecycle";
	case LogCat::Bridge:
		return "bridge";
	case LogCat::Preview:
		return "preview";
	case LogCat::Canvas:
		return "canvas";
	case LogCat::Stream:
		return "stream";
	case LogCat::Encode:
		return "encode";
	case LogCat::Audio:
		return "audio";
	case LogCat::OAuth:
		return "oauth";
	case LogCat::Chat:
		return "chat";
	case LogCat::Events:
		return "events";
	case LogCat::Overlay:
		return "overlay";
	case LogCat::Import:
		return "import";
	case LogCat::Scene:
		return "scene";
	case LogCat::Mcp:
		return "mcp";
	case LogCat::Net:
		return "net";
	case LogCat::Cef:
		return "cef";
	}
	return "?";
}

// printf-format checking on GCC/Clang (CI builds those with -Werror); a no-op on
// MSVC, matching libobs' own blog() attribute (util/base.h #undefs its macro).
#if defined(__GNUC__) || defined(__clang__)
#define BRAIDCAST_PRINTF_ATTR(fmtIdx, argIdx) __attribute__((__format__(__printf__, fmtIdx, argIdx)))
#else
#define BRAIDCAST_PRINTF_ATTR(fmtIdx, argIdx)
#endif

namespace Log {

// Whether the gated DEBUG channel is on. A cheap relaxed atomic read; DBG()
// checks this BEFORE evaluating its arguments so gated logging is zero-cost off.
bool DebugEnabled();

// Flip the gate. Called from the boot seed and the diagnostics.setDebug bridge
// method (the persisted default lives in DiagnosticsSettings).
void SetDebug(bool enabled);

// Emit one gated DEBUG line: builds "[<cat>] " + formatted fmt via vsnprintf and
// hands it to blog(LOG_DEBUG, ...). Prefer the DBG() macro so arguments aren't
// evaluated when the gate is off.
void Debug(LogCat cat, const char *fmt, ...) BRAIDCAST_PRINTF_ATTR(2, 3);

} // namespace Log

// Gated DEBUG log. Evaluates its arguments ONLY when the gate is on, so calls
// left in hot paths cost a single atomic read when DEBUG is off.
#define DBG(cat, ...)                                     \
	do {                                              \
		if (::Log::DebugEnabled()) {              \
			::Log::Debug((cat), __VA_ARGS__); \
		}                                         \
	} while (0)

// Mirror a line into the leveled logger at INFO so lifecycle logging lands in the
// session log file (SessionLog chains blog's handler) as well as stderr (blog's
// default handler). Used by the shell's startup/teardown/CEF callbacks; always on
// (NOT gated by the DEBUG flag).
inline void HostLog(const std::string &line)
{
	blog(LOG_INFO, "%s", line.c_str());
}

#endif // OBS_MULTISTREAM_FRONTEND_LOG_HPP_
