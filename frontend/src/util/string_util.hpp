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

// Case-insensitive whole-string equality. The protocol words a stream profile carries
// ("RTMPS", "HLS") are compared against fixed spellings, and neither side owns the casing.
inline bool EqualsCI(const std::string &a, const std::string &b)
{
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
		       return std::tolower(static_cast<unsigned char>(x)) ==
			      std::tolower(static_cast<unsigned char>(y));
	       });
}

// Whether `list` -- a delimited string, the shape libobs uses for its codec and protocol
// lists ("h264;hevc") -- carries `item` as a whole entry. A plain substring test would
// accept "h264" against a list holding only "h264_fallback", so the boundaries matter.
inline bool ListContains(const std::string &list, const std::string &item, char delimiter)
{
	if (item.empty()) {
		return false;
	}
	for (size_t start = 0; start <= list.size();) {
		const size_t end = list.find(delimiter, start);
		const size_t stop = end == std::string::npos ? list.size() : end;
		if (list.compare(start, stop - start, item) == 0) {
			return true;
		}
		if (end == std::string::npos) {
			break;
		}
		start = end + 1;
	}
	return false;
}

} // namespace StringUtil

#endif // OBS_MULTISTREAM_FRONTEND_STRING_UTIL_HPP_
