#ifndef OBS_MULTISTREAM_FRONTEND_UTIL_SELFTEST_PATHS_HPP_
#define OBS_MULTISTREAM_FRONTEND_UTIL_SELFTEST_PATHS_HPP_

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

} // namespace SelfTest

#endif // OBS_MULTISTREAM_FRONTEND_UTIL_SELFTEST_PATHS_HPP_
