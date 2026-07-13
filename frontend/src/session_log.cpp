#include "session_log.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include <util/base.h>
#include <util/platform.h>

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

} // namespace

void Init()
{
	if (g_initialized) {
		return;
	}
	g_initialized = true;

	char dir[512];
	if (os_get_config_path(dir, sizeof(dir), "braidcast/logs") <= 0) {
		// Can't resolve the dir: still chain so logging keeps working.
		base_get_log_handler(&g_prevHandler, &g_prevParam);
		base_set_log_handler(SessionLogHandler, nullptr);
		return;
	}
	os_mkdirs(dir);

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

} // namespace SessionLog
