#include "update_version.hpp"

#include <cctype>

namespace Update {

std::optional<std::array<int, 3>> ParseVersion(const std::string &s)
{
	size_t i = 0;
	if (i < s.size() && (s[i] == 'v' || s[i] == 'V')) {
		++i;
	}
	std::array<int, 3> parts{0, 0, 0};
	for (size_t part = 0; part < parts.size(); ++part) {
		if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) {
			if (part == 0) {
				return std::nullopt;
			}
			break;
		}
		int n = 0;
		while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
			if (n > 99999) {
				return std::nullopt; // not a version anyone tags
			}
			n = n * 10 + (s[i] - '0');
			++i;
		}
		parts[part] = n;
		if (i < s.size() && s[i] == '.') {
			++i;
		} else {
			break;
		}
	}
	return parts;
}

bool IsNewer(const std::string &candidate, const std::string &running)
{
	const std::optional<std::array<int, 3>> a = ParseVersion(candidate);
	const std::optional<std::array<int, 3>> b = ParseVersion(running);
	return a && b && *a > *b;
}

} // namespace Update
