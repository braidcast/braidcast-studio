#include "env_config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

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
	const auto trim = [](std::string s) {
		const size_t b = s.find_first_not_of(" \t\r");
		if (b == std::string::npos) {
			return std::string();
		}
		return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
	};
	std::string line;
	while (std::getline(f, line)) {
		const size_t eq = line.find('=');
		if (eq == std::string::npos) {
			continue;
		}
		if (trim(line.substr(0, eq)) != key) {
			continue;
		}
		return trim(line.substr(eq + 1));
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
	std::string v = *raw;
	std::transform(v.begin(), v.end(), v.begin(),
		       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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

std::string Value(const char *key, const std::string &fallback)
{
	const std::optional<std::string> raw = Raw(key);
	return raw ? *raw : fallback;
}

} // namespace Env
