#include "env_config.hpp"

#include <cstdlib>
#include <fstream>

#include "string_util.hpp"

namespace Env {

namespace {

// Read `key`'s value from a KEY=VALUE .env file (CRLF- and whitespace-tolerant).
// Yields the raw value string; nullopt when the file is missing or has no such
// key, so the caller falls through to the next source.
std::optional<std::string> FromEnvFile(const char *path, const char *key)
{
	std::ifstream f(path);
	if (!f) {
		return std::nullopt;
	}
	std::string line;
	while (std::getline(f, line)) {
		const size_t eq = line.find('=');
		if (eq == std::string::npos) {
			continue;
		}
		if (StringUtil::Trim(line.substr(0, eq)) != key) {
			continue;
		}
		return StringUtil::Trim(line.substr(eq + 1));
	}
	return std::nullopt;
}

} // namespace

std::optional<std::string> Raw(const char *key)
{
	if (const char *env = getenv(key)) {
		return std::string(env);
	}
#ifdef BRAIDCAST_ENV_FILE
	if (const std::optional<std::string> v = FromEnvFile(BRAIDCAST_ENV_FILE, key)) {
		return v;
	}
#endif
	return std::nullopt;
}

bool Flag(const char *key, bool fallback)
{
	const std::optional<std::string> raw = Raw(key);
	if (!raw) {
		return fallback;
	}
	const std::string v = StringUtil::ToLower(*raw);
	return !(v.empty() || v == "0" || v == "false" || v == "no" || v == "off");
}

long Number(const char *key, long fallback)
{
	const std::optional<std::string> raw = Raw(key);
	if (!raw) {
		return fallback;
	}
	char *end = nullptr;
	const long n = strtol(raw->c_str(), &end, 10);
	return (end && end != raw->c_str()) ? n : fallback;
}

double Double(const char *key, double fallback)
{
	const std::optional<std::string> raw = Raw(key);
	if (!raw) {
		return fallback;
	}
	char *end = nullptr;
	const double d = strtod(raw->c_str(), &end);
	return (end && end != raw->c_str()) ? d : fallback;
}

bool IsSet(const char *key)
{
	return Raw(key).has_value();
}

bool IsSelfTestRun()
{
	return IsSet("FE_SMOKE_QUIT_SECONDS") || IsSet("BRAIDCAST_SELFTEST_STREAM");
}

std::string Value(const char *key, const std::string &fallback)
{
	const std::optional<std::string> raw = Raw(key);
	return raw ? *raw : fallback;
}

} // namespace Env
