#ifndef OBS_MULTISTREAM_FRONTEND_UTIL_SELFTEST_PATHS_HPP_
#define OBS_MULTISTREAM_FRONTEND_UTIL_SELFTEST_PATHS_HPP_

#include <chrono>
#include <string>

// Where a BRAIDCAST_SELFTEST_STREAM mode leaves its machine-readable summary, and how that
// file is named. Shared by every such mode so their artifacts land together and carry the
// same run identity; a second mode copying these two functions would drift from the first.
namespace SelfTest {

// <config base>/selftest/<file>, resolved through the shared BraidcastConfigPath seam
// (StorePaths.hpp) so the scratch files follow portable mode with every other store.
// Creates the directory. Returns "" if the config base cannot be resolved.
std::string ConfigPath(const std::string &file);

// This run's identity, taken from SessionLog's own per-session timestamp (its filename is
// already "YYYY-MM-DD HH-MM-SS.txt") rather than a fresh time() call, so a summary and the
// log that explains it carry the same stamp. "unknown" if no session log was opened.
std::string TimestampFromSessionLog();

// Writes `body` to ConfigPath("<prefix>-<run stamp>.txt"), replacing any existing file, and
// returns the path written. Returns "" if the config base could not be resolved OR the file
// could not be opened -- callers must report that as "(unwritten)" rather than as a path,
// because a mode's summary is the only record of a run nobody watched, and a log line naming a
// file that is not there is worse than one admitting nothing was written.
std::string WriteSummaryFile(const std::string &prefix, const std::string &body);

// The name every mode gives its exit code, in its log line and its summary alike: 0 PASS,
// 1 FAIL, 2 SKIP, anything else NOT RUN. One mapping, so two modes cannot disagree about what a
// number means.
const char *ResultName(int exitCode);

// How many lines of this session's log contain `needle`, or -1 if the log cannot be read. The
// session log handler flushes every message, so a line emitted moments ago is already counted.
int CountSessionLogLines(const std::string &needle);

// How long a BRAIDCAST_SELFTEST_STREAM mode waits after arming before touching anything, so
// startup -- module loads, the first scene, the UI's own first bridge calls -- has settled and
// does not land inside a measurement.
constexpr std::chrono::milliseconds kBootSettle{3000};

// Rate gate for a mode that counts an audio stream's samples against wall clock: how far the
// received count may fall behind, as a percentage of the rate it is counted in. The only gate
// that catches PARTIAL starvation -- many gaps each under any gap threshold, or short packets --
// which leaves a stream falling behind while no single gap looks wrong. Measured spread on a
// healthy run is under 0.01%, so 2% is ~200x margin against tick granularity and the partial
// packet at each end of the window, while still failing a stream that lost a second or more of
// audio over a run of a minute or two.
constexpr double kMaxRateDeficitPct = 2.0;

} // namespace SelfTest

#endif // OBS_MULTISTREAM_FRONTEND_UTIL_SELFTEST_PATHS_HPP_
