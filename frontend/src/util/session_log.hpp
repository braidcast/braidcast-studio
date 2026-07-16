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

// Register the libobs crash sink (base_set_crash_handler). When bcrash() fires
// -- notably from the Win32 unhandled-exception filter -- the crash report is
// written to <config base>/crashes/Crash <ts>.txt (rotated like session logs)
// and the process exits non-zero so a crash can never score as success. Must
// run BEFORE obs_init_win32_crash_handler() installs the filter, so the filter
// can never fire while libobs' default sink (stderr + exit 0) is still live.
void InstallCrashHandler();

} // namespace SessionLog

#endif // OBS_MULTISTREAM_FRONTEND_SESSION_LOG_HPP_
