#ifndef OBS_MULTISTREAM_FRONTEND_LOG_HPP_
#define OBS_MULTISTREAM_FRONTEND_LOG_HPP_

#include <windows.h>

#include <util/base.h>

#include <cstdint>
#include <string>

// The curated category table: the SINGLE source for the LogCat enum, the wire
// name each gated line is tagged with, and the name lookup that the
// BRAIDCAST_DEBUG filter spec and the CEF console parser share. Adding a
// category is one line here.
// keep in sync with frontend/web/src/lib/utils/logCategories.ts
//
// Render and RenderGpu are GATE-ONLY categories: nothing emits a "[render]" or
// "[rendergpu]" line. Render toggles libobs's per-frame render-thread timing
// (obs_set_render_debug, tagged "[render-debug]"), a firehose that would
// otherwise flood the log. RenderGpu separately toggles the GPU timer queries
// behind those diagnostics (obs_set_render_gpu_debug) -- split out because the
// per-frame query readback happens on the graphics thread and is itself a
// suspect for the stalls the timing exists to find, so the timing has to be
// observable without it. Both are excluded from kDefaultCats, so neither the
// binary debug toggle nor the "basic" spec token enables either; opt in with the
// two-var scheme, BRAIDCAST_DEBUG=1 BRAIDCAST_DEBUG_COMPONENTS=render (optionally
// ",rendergpu"), or take the whole table with "all". The master is mandatory: a
// falsy BRAIDCAST_DEBUG yields an empty category set before components are parsed
// at all.
#define BRAIDCAST_LOG_CATEGORIES(X) \
	X(Lifecycle, "lifecycle")   \
	X(Bridge, "bridge")         \
	X(Preview, "preview")       \
	X(Canvas, "canvas")         \
	X(Stream, "stream")         \
	X(Encode, "encode")         \
	X(Audio, "audio")           \
	X(OAuth, "oauth")           \
	X(Chat, "chat")             \
	X(Events, "events")         \
	X(Overlay, "overlay")       \
	X(Import, "import")         \
	X(Scene, "scene")           \
	X(Mcp, "mcp")               \
	X(Net, "net")               \
	X(Cef, "cef")               \
	X(Render, "render")         \
	X(RenderGpu, "rendergpu")

enum class LogCat {
#define BRAIDCAST_LOG_CAT_ENUM(sym, name) sym,
	BRAIDCAST_LOG_CATEGORIES(BRAIDCAST_LOG_CAT_ENUM)
#undef BRAIDCAST_LOG_CAT_ENUM
};

inline constexpr int kLogCatCount = 0
#define BRAIDCAST_LOG_CAT_COUNT(sym, name) +1
	BRAIDCAST_LOG_CATEGORIES(BRAIDCAST_LOG_CAT_COUNT)
#undef BRAIDCAST_LOG_CAT_COUNT
	;

// One bit per category in Log::CatMask; kAllCats shifts by the count, so the
// ceiling is one below the mask type's width.
static_assert(kLogCatCount < 32, "LogCat no longer fits Log::CatMask's shift -- widen it");

// Lowercase, stable name for a category: the tag written into the log line and
// the wire name shared with the web logger.
inline const char *LogCatName(LogCat cat)
{
	switch (cat) {
#define BRAIDCAST_LOG_CAT_NAME(sym, name) \
	case LogCat::sym:                 \
		return name;
		BRAIDCAST_LOG_CATEGORIES(BRAIDCAST_LOG_CAT_NAME)
#undef BRAIDCAST_LOG_CAT_NAME
	}
	return "?";
}

// Reverse of LogCatName. False (leaving `out` untouched) when no category owns
// the name, so callers can tell a curated tag from arbitrary text.
inline bool LogCatFromName(const std::string &name, LogCat &out)
{
#define BRAIDCAST_LOG_CAT_LOOKUP(sym, cname) \
	if (name == cname) {                 \
		out = LogCat::sym;           \
		return true;                 \
	}
	BRAIDCAST_LOG_CATEGORIES(BRAIDCAST_LOG_CAT_LOOKUP)
#undef BRAIDCAST_LOG_CAT_LOOKUP
	return false;
}

// printf-format checking on GCC/Clang (CI builds those with -Werror); a no-op on
// MSVC, matching libobs' own blog() attribute (util/base.h #undefs its macro).
#if defined(__GNUC__) || defined(__clang__)
#define BRAIDCAST_PRINTF_ATTR(fmtIdx, argIdx) __attribute__((__format__(__printf__, fmtIdx, argIdx)))
#else
#define BRAIDCAST_PRINTF_ATTR(fmtIdx, argIdx)
#endif

namespace Log {

// The set of categories whose DEBUG lines emit; one bit per LogCat.
using CatMask = uint32_t;

constexpr CatMask CatBit(LogCat cat)
{
	return CatMask{1} << static_cast<int>(cat);
}

inline constexpr CatMask kNoCats = 0;
inline constexpr CatMask kAllCats = (CatMask{1} << kLogCatCount) - 1;
// Every category EXCEPT the two render-thread gates. Three routes resolve to it:
// the binary debug toggle (SetDebug / diagnostics.setDebug), the "basic"
// component token, and a truthy BRAIDCAST_DEBUG left with no
// BRAIDCAST_DEBUG_COMPONENTS. So enabling debug never floods the per-frame
// [render-debug] timing nor arms GPU timer queries -- opt into those with
// BRAIDCAST_DEBUG=1 BRAIDCAST_DEBUG_COMPONENTS=render / rendergpu, or take every
// category with "all". The master variable is mandatory either way: without it no
// component is parsed at all.
inline constexpr CatMask kDefaultCats = kAllCats & ~CatBit(LogCat::Render) & ~CatBit(LogCat::RenderGpu);

// What kDefaultCats holds back, which it holds back because those categories cost
// something to run. Derived rather than restated, so excluding a future category
// from the default set also enrolls it in the cost notice SetDebugMask emits.
inline constexpr CatMask kCostlyCats = kAllCats & ~kDefaultCats;

// Whether any category is on. The coarse gate: what diagnostics.get reports and
// what the web logger mirrors, since the per-category filter is applied
// host-side in Client::OnConsoleMessage.
bool DebugEnabled();

// Whether this specific category is on. A relaxed atomic read plus a bit test;
// DBG() calls this BEFORE evaluating its arguments so gated logging is
// zero-cost off.
bool DebugEnabled(LogCat cat);

// The live category mask.
CatMask DebugMask();

// Flip every category on or off. The binary toggle that the diagnostics.setDebug
// bridge method and the persisted DiagnosticsSettings default drive.
void SetDebug(bool enabled);

// Set the exact category mask. The boot resolver uses this so
// BRAIDCAST_DEBUG_COMPONENTS can name a subset.
void SetDebugMask(CatMask mask);

// The components resolved from a BRAIDCAST_DEBUG_COMPONENTS list: the categories
// to enable plus whether the non-category gpudiag sampler subsystem is requested.
struct DebugComponents {
	CatMask logMask = kNoCats;
	bool gpuDiag = false;
};

// Parse a BRAIDCAST_DEBUG_COMPONENTS list (comma- or space-separated, case-
// insensitive) into its components, accumulatively:
//   "basic"        -> |= kDefaultCats (every category except the render gates)
//   "all"          -> |= kAllCats (the whole table, render gates included)
//   a category name -> |= that category's bit (e.g. "render" opts the firehose in)
//   "gpudiag"      -> gpuDiag = true
//   anything else  -> ignored, so a stale token still yields the rest
DebugComponents ParseComponents(const std::string &spec);

// Emit one gated DEBUG line: builds "[<cat>] " + formatted fmt via vsnprintf and
// hands it to blog(LOG_DEBUG, ...). Prefer the DBG() macro so arguments aren't
// evaluated when the gate is off.
void Debug(LogCat cat, const char *fmt, ...) BRAIDCAST_PRINTF_ATTR(2, 3);

} // namespace Log

// Gated DEBUG log. Evaluates its arguments ONLY when that category is on, so
// calls left in hot paths cost a single relaxed atomic read plus a bit test
// while DEBUG is off.
#define DBG(cat, ...)                                     \
	do {                                              \
		if (::Log::DebugEnabled((cat))) {         \
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
