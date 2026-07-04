#ifndef OBS_MULTISTREAM_FRONTEND_SESSION_LOG_HPP_
#define OBS_MULTISTREAM_FRONTEND_SESSION_LOG_HPP_

#include <string>

// Per-session libobs log file. Init() installs a base log handler that chains to
// whatever handler was already registered (so stderr/HostLog keep working) and
// additionally mirrors every blog() line to <config>/braidcast/logs/
// <YYYY-MM-DD HH-MM-SS>.txt, rotating older files. Must be called AFTER the
// existing handler is installed so it can capture and chain to it.
namespace SessionLog {

// Open this session's log file, rotate old ones, and chain the log handler.
// Idempotent: a second call is a no-op. Degrades gracefully (still chains, just
// skips the file) if the directory or file cannot be opened.
void Init();

// Absolute path to this session's log file, or "" if Init() never opened one.
std::string CurrentPath();

// Restore the previous log handler and close the file. Optional; the OS closes
// the file on exit if this is skipped.
void Shutdown();

} // namespace SessionLog

#endif // OBS_MULTISTREAM_FRONTEND_SESSION_LOG_HPP_
