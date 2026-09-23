#include "selftest_paths.hpp"

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

int CountSessionLogLines(const std::string &needle)
{
	std::ifstream in(SessionLog::CurrentPath());
	if (!in) {
		return -1;
	}
	int count = 0;
	std::string line;
	while (std::getline(in, line)) {
		if (line.find(needle) != std::string::npos) {
			++count;
		}
	}
	return count;
}

} // namespace SelfTest
