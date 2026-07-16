#include "session_log.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include <util/base.h>
#include <util/platform.h>

#include "../multistream/StorePaths.hpp"

namespace SessionLog {

namespace {

// How many session log files to keep. Older ones are deleted on Init().
constexpr unsigned kMaxLogs = 10;
// Cap on a single formatted line. Matches libobs' own do_log buffer.
constexpr size_t kLineBuf = 8192;

std::mutex g_mutex; // guards g_file (blog is called from many threads)
std::ofstream g_file;
std::string g_path; // absolute path to this session's log, "" if unopened
bool g_initialized = false;

log_handler_t g_prevHandler = nullptr;
void *g_prevParam = nullptr;

// Filenames are "YYYY-MM-DD HH-MM-SS.txt", so lexicographic order == chronological.
std::string GenerateTimestampName()
{
	const time_t now = time(nullptr);
	struct tm lt;
	localtime_s(&lt, &now);
	char buf[32];
	strftime(buf, sizeof(buf), "%Y-%m-%d %H-%M-%S.txt", &lt);
	return std::string(buf);
}

// Delete the oldest .txt files until at most kMaxLogs remain. Called after the
// new file exists, so the newest (this session) is never a deletion candidate.
void RotateOldLogs(const std::filesystem::path &dir)
{
	std::error_code ec;
	std::vector<std::filesystem::path> logs;
	for (auto &entry : std::filesystem::directory_iterator(dir, ec)) {
		if (ec) {
			break;
		}
		if (entry.is_regular_file(ec) && entry.path().extension() == ".txt") {
			logs.push_back(entry.path());
		}
	}
	if (logs.size() <= kMaxLogs) {
		return;
	}
	std::sort(logs.begin(), logs.end()); // ascending == oldest first
	const size_t remove = logs.size() - kMaxLogs;
	for (size_t i = 0; i < remove; ++i) {
		std::filesystem::remove(logs[i], ec);
	}
}

// The installed handler: format once, write to the file, then forward the SAME
// message to the previous handler so stderr/HostLog/debugger output is preserved.
void SessionLogHandler(int level, const char *format, va_list args, void *)
{
	// va_copy BEFORE consuming args: vsnprintf below exhausts `args`, so the
	// previous handler needs its own untouched copy.
	va_list argsForPrev;
	va_copy(argsForPrev, args);

	char line[kLineBuf];
	vsnprintf(line, sizeof(line), format, args);

	if (g_prevHandler) {
		g_prevHandler(level, format, argsForPrev, g_prevParam);
	}
	va_end(argsForPrev);

	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_file.is_open()) {
		g_file << line << '\n';
		g_file.flush();
	}
}

// Cap on a formatted crash report. Matches the legacy frontend's crash writer.
constexpr size_t kMaxCrashReport = 200 * 1024;

// Resolved once at install so the handler does zero path resolution while the
// process is crashed.
std::string g_crashDir;
// Headless smoke/selftest runs must never block on a modal message box.
bool g_crashQuiet = false;
// Off-stack: the crashing thread's stack may be nearly exhausted (e.g. a
// stack-overflow fault), so the report is formatted into static storage.
char g_crashText[kMaxCrashReport];
volatile LONG g_crashEntered = 0;

// bcrash() sink. Runs inside a crashed process: work is limited to formatting
// into a static buffer and one file write on a precomputed path. Never returns.
void CrashSinkHandler(const char *format, va_list args, void *)
{
	// A crash inside this handler -- or a second thread crashing
	// concurrently -- must not re-enter the file write; die immediately,
	// still reporting failure.
	if (InterlockedExchange(&g_crashEntered, 1)) {
		TerminateProcess(GetCurrentProcess(), static_cast<UINT>(-1));
	}

	vsnprintf(g_crashText, sizeof(g_crashText), format, args);

	std::string path;
	if (!g_crashDir.empty()) {
		path = g_crashDir + "/Crash " + GenerateTimestampName();
		std::ofstream file(std::filesystem::u8path(path), std::ios::out | std::ios::trunc | std::ios::binary);
		if (file.is_open()) {
			file << g_crashText;
		}
	}

	if (!g_crashQuiet) {
		std::replace(path.begin(), path.end(), '/', '\\');
		char message[1024];
		snprintf(message, sizeof(message),
			 "Woops, Braidcast has crashed!\n\nWould you like to copy the crash log "
			 "to the clipboard? The crash log will still be saved to:\n\n%s",
			 path.empty() ? "(unavailable)" : path.c_str());
		const int ret =
			MessageBoxA(nullptr, message, "Braidcast has crashed!", MB_YESNO | MB_ICONERROR | MB_TASKMODAL);
		if (ret == IDYES) {
			const size_t len = strlen(g_crashText) + 1;
			if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, len)) {
				if (void *dst = GlobalLock(mem)) {
					memcpy(dst, g_crashText, len);
					GlobalUnlock(mem);
					if (OpenClipboard(nullptr)) {
						EmptyClipboard();
						SetClipboardData(CF_TEXT, mem);
						CloseClipboard();
					}
				}
			}
		}
	}

	// _exit, not exit: atexit handlers and static destructors must not run in
	// a crashed process, and the exit code must report failure -- never 0.
	_exit(-1);
}

} // namespace

void Init()
{
	if (g_initialized) {
		return;
	}
	g_initialized = true;

	const std::string dir = BraidcastConfigPath("logs");
	if (dir.empty()) {
		// Can't resolve the dir: still chain so logging keeps working.
		base_get_log_handler(&g_prevHandler, &g_prevParam);
		base_set_log_handler(SessionLogHandler, nullptr);
		return;
	}
	os_mkdirs(dir.c_str());

	const std::filesystem::path logsDir = std::filesystem::u8path(dir);
	const std::filesystem::path full = logsDir / std::filesystem::u8path(GenerateTimestampName());

	g_file.open(full, std::ios::out | std::ios::trunc);
	if (g_file.is_open()) {
		g_path = full.u8string();
		RotateOldLogs(logsDir);
	}

	// Capture the existing handler (e.g. the stderr/HostLog one) and chain to it.
	base_get_log_handler(&g_prevHandler, &g_prevParam);
	base_set_log_handler(SessionLogHandler, nullptr);
}

std::string CurrentPath()
{
	return g_path;
}

void Shutdown()
{
	if (!g_initialized) {
		return;
	}
	base_set_log_handler(g_prevHandler, g_prevParam);
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_file.is_open()) {
		g_file.close();
	}
	g_initialized = false;
}

void InstallCrashHandler()
{
	g_crashDir = BraidcastConfigPath("crashes");
	if (!g_crashDir.empty()) {
		os_mkdirs(g_crashDir.c_str());
		// Rotate at install, not at crash time, so the handler itself never
		// iterates a directory inside a crashed process.
		RotateOldLogs(std::filesystem::u8path(g_crashDir));
	}
	g_crashQuiet = getenv("FE_SMOKE_QUIT_SECONDS") != nullptr || getenv("BRAIDCAST_SELFTEST_STREAM") != nullptr;
	base_set_crash_handler(CrashSinkHandler, nullptr);
}

} // namespace SessionLog
