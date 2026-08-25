#ifndef OBS_MULTISTREAM_FRONTEND_STRING_UTIL_HPP_
#define OBS_MULTISTREAM_FRONTEND_STRING_UTIL_HPP_

#include <algorithm>
#include <cctype>
#include <string>

// Small string helpers shared across subsystems. Kept in one place so the same
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

// Case-insensitive ordering, the strict-weak counterpart to EqualsCI. What a list of
// names shown to a user is sorted by: byte order files "Arial" and "arial" apart, which
// reads as two entries for one thing.
inline bool LessCI(const std::string &a, const std::string &b)
{
	return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
		return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
	});
}

// Lowercase every character, byte by byte. The unsigned-char cast is the whole point
// of having this in one place: std::tolower takes an int and is undefined for a
// negative one, which is exactly what a signed char holding UTF-8 continuation bytes
// gives you.
inline std::string ToLower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(),
		       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

// The strict boolean grammar the master debug flags are written in: 1/true/on/yes
// (case-insensitive, whitespace ignored) are on; everything else -- 0/false/off/no,
// empty, a typo -- is off. Deliberately stricter than Env::Flag, whose "set to
// anything counts" convention exists for the diag flags; a switch that widens the
// attack surface should only open on a word that says so.
inline bool ParseBool(const std::string &raw)
{
	std::string v;
	for (const char c : raw) {
		if (!std::isspace(static_cast<unsigned char>(c))) {
			v += c;
		}
	}
	v = ToLower(v);
	return v == "1" || v == "true" || v == "on" || v == "yes";
}

// `s` without leading or trailing whitespace, over the whole ASCII whitespace set. The
// general-purpose one, for wherever "do these two strings say the same thing" is asked -- a
// .env value, a log-filter token, a value a platform echoed back.
//
// Not the only trim in the tree, and deliberately not: mcp/HttpServer.cpp keeps its own,
// whose leading and trailing sets differ FROM EACH OTHER on purpose for HTTP OWS parsing.
// Folding that one onto this would change header parsing, so leave it where it is.
inline std::string Trim(const std::string &s)
{
	static constexpr char kSpace[] = " \t\n\v\f\r";
	const size_t begin = s.find_first_not_of(kSpace);
	if (begin == std::string::npos) {
		return std::string();
	}
	return s.substr(begin, s.find_last_not_of(kSpace) - begin + 1);
}

// Append `part` to `out` so the concatenation can only be read one way: the part's byte
// length, a ':', then the bytes verbatim. For building an identity string out of several
// values -- a key and its value, a provider id and its bag -- where joining on a delimiter
// would be forgeable. The values are free text a streamer typed, so whatever byte was
// chosen as the delimiter is a byte some value may carry, and a value carrying one would
// move a boundary and let two different value sets build the same string. A length prefix
// leaves nothing to forge: a reader is told how far the part runs and never looks for a
// terminator, so the byte sequence decodes back to exactly one list of parts.
inline void AppendLengthPrefixed(std::string &out, const std::string &part)
{
	out += std::to_string(part.size());
	out += ':';
	out += part;
}

// Case-sensitive suffix test. An empty suffix matches anything, matching
// std::string::compare over a zero-length range.
inline bool EndsWith(const std::string &s, const std::string &suffix)
{
	return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// `s` without a trailing `suffix`, or `s` unchanged when it doesn't end in one.
inline std::string StripSuffix(const std::string &s, const std::string &suffix)
{
	return EndsWith(s, suffix) ? s.substr(0, s.size() - suffix.size()) : s;
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

// Whether the unsuffixed `base` is worth offering. Skip is for callers that already
// know it collides, so probing it would be a wasted lookup.
enum class BareName { Try, Skip };

// The auto-suffix loop behind the "that name is already taken" resolutions that number
// plainly -- scenes, sources, filters, canvases, destination labels. Hands back the first of
// `base`, "base 2", "base 3", ... that `taken` reports free. What differs per caller
// is only what "taken" means and which namespace it consults, so that stays a
// callable; the numbering, which the user sees, does not. Scene collections are the one
// holdout: their "(Imported N)" candidate shape is a different sequence, so obs_importer
// keeps its own loop.
template<typename TakenFn> std::string UniqueName(const std::string &base, BareName bare, const TakenFn &taken)
{
	if (bare == BareName::Try && !taken(base)) {
		return base;
	}
	for (int n = 2;; ++n) {
		const std::string candidate = base + " " + std::to_string(n);
		if (!taken(candidate)) {
			return candidate;
		}
	}
}

} // namespace StringUtil

#endif // OBS_MULTISTREAM_FRONTEND_STRING_UTIL_HPP_
