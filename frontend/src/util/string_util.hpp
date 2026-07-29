#ifndef OBS_MULTISTREAM_FRONTEND_STRING_UTIL_HPP_
#define OBS_MULTISTREAM_FRONTEND_STRING_UTIL_HPP_

#include <algorithm>
#include <cctype>
#include <string>

// Small string predicates shared across subsystems. Kept in one place so the same
// question can't be answered two slightly different ways in two translation units.
namespace StringUtil {

// Case-insensitive substring test (an empty needle always matches). This is what a
// client-side lookup filter asks -- the provider descriptors' typeahead fields
// (YouTube's category list, Facebook's Page list) both narrow their options with it,
// and both mean "matches however the user cased it".
inline bool ContainsCI(const std::string &haystack, const std::string &needle)
{
	if (needle.empty()) {
		return true;
	}
	const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char a, char b) {
		return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
	});
	return it != haystack.end();
}

} // namespace StringUtil

#endif // OBS_MULTISTREAM_FRONTEND_STRING_UTIL_HPP_
