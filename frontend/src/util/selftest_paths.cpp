#include "selftest_paths.hpp"

#include <windows.h>

#include <cstdio>
#include <fstream>

#include <util/platform.h>

#include "../multistream/StorePaths.hpp"
#include "session_log.hpp"

namespace SelfTest {

std::string ConfigPath(const std::string &file)
{
	const std::string base = BraidcastConfigPath("selftest");
	if (base.empty()) {
		return std::string();
	}
	os_mkdirs(base.c_str());
	return base + "/" + file;
}

std::string TimestampFromSessionLog()
{
	const std::string logPath = SessionLog::CurrentPath();
	if (logPath.empty()) {
		return "unknown";
	}
	const size_t slash = logPath.find_last_of("/\\");
	const std::string base = slash == std::string::npos ? logPath : logPath.substr(slash + 1);
	const size_t dot = base.rfind(".txt");
	return dot == std::string::npos ? base : base.substr(0, dot);
}

std::string WriteSummaryFile(const std::string &prefix, const std::string &body)
{
	std::string path = ConfigPath(prefix + "-" + TimestampFromSessionLog() + ".txt");
	if (path.empty()) {
		return path;
	}

	std::ofstream out(path, std::ios::out | std::ios::trunc);
	if (!out) {
		// A read-only config directory or a full disk. Reported as nothing written, since
		// that is what happened.
		path.clear();
		return path;
	}
	out << body;
	return path;
}

const char *ResultName(int exitCode)
{
	return exitCode == 0 ? "PASS" : exitCode == 1 ? "FAIL" : exitCode == 2 ? "SKIP" : "NOT RUN";
}

namespace {

// `accept`, when given, filters the matching lines further.
int CountLines(const std::string &path, const std::string &needle, bool (*accept)(const std::string &) = nullptr)
{
	std::ifstream in(path);
	if (!in) {
		return -1;
	}
	int count = 0;
	std::string line;
	while (std::getline(in, line)) {
		if (line.find(needle) != std::string::npos && (!accept || accept(line))) {
			++count;
		}
	}
	return count;
}

// A local wall-clock time as one comparable number: MMDDhhmmssmmm.
long long Stamp(unsigned monthDay, unsigned hms, unsigned ms)
{
	return (monthDay * 1000000LL + hms) * 1000 + ms;
}

// When this process started, in local time as CEF stamps its lines.
long long LaunchStamp()
{
	static const long long stamp = [] {
		FILETIME created, exited, kernel, user;
		FILETIME local;
		SYSTEMTIME t;
		if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) ||
		    !FileTimeToLocalFileTime(&created, &local) || !FileTimeToSystemTime(&local, &t)) {
			return 0LL;
		}
		return Stamp(t.wMonth * 100u + t.wDay, t.wHour * 10000u + t.wMinute * 100u + t.wSecond,
			     t.wMilliseconds);
	}();
	return stamp;
}

// Whether a CEF log line ("[MMDD/hhmmss.mmm:...") was written by this launch. The log
// is wiped with the self-test config base, but a leftover subprocess from a killed run
// can hold it open, and what an earlier launch wrote must not count. Lines crossing a new
// year compare wrongly; a self-test spanning midnight on 31 December is not worth code.
bool WrittenThisLaunch(const std::string &line)
{
	unsigned monthDay = 0;
	unsigned hms = 0;
	unsigned ms = 0;
	if (sscanf(line.c_str(), "[%4u/%6u.%3u:", &monthDay, &hms, &ms) != 3) {
		return false;
	}
	return Stamp(monthDay, hms, ms) >= LaunchStamp();
}

} // namespace

int CountSessionLogLines(const std::string &needle)
{
	return CountLines(SessionLog::CurrentPath(), needle);
}

int CountCefLogLines(const std::string &needle)
{
	return CountLines(BraidcastConfigPath(kCefDebugLogFile), needle, WrittenThisLaunch);
}

} // namespace SelfTest
