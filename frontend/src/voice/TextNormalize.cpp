#include "voice/TextNormalize.hpp"

#include <algorithm>
#include <cctype>

namespace Voice {

namespace {

struct NumberWord {
	const char *word;
	int value;
};

// 0-19 and the tens. Anything above 99 is rare in a spoken command and stays as words.
constexpr NumberWord kUnits[] = {
	{"zero", 0},     {"one", 1},      {"two", 2},        {"three", 3},     {"four", 4},
	{"five", 5},     {"six", 6},      {"seven", 7},      {"eight", 8},     {"nine", 9},
	{"ten", 10},     {"eleven", 11},  {"twelve", 12},    {"thirteen", 13}, {"fourteen", 14},
	{"fifteen", 15}, {"sixteen", 16}, {"seventeen", 17}, {"eighteen", 18}, {"nineteen", 19},
};

constexpr NumberWord kTens[] = {
	{"twenty", 20}, {"thirty", 30},  {"forty", 40},  {"fifty", 50},
	{"sixty", 60},  {"seventy", 70}, {"eighty", 80}, {"ninety", 90},
};

int UnitValue(const std::string &word)
{
	for (const NumberWord &n : kUnits) {
		if (word == n.word) {
			return n.value;
		}
	}
	return -1;
}

int TensValue(const std::string &word)
{
	for (const NumberWord &n : kTens) {
		if (word == n.word) {
			return n.value;
		}
	}
	return -1;
}

// whisper often writes a compound number with a hyphen, as one token: "twenty-one".
// -1 when `word` is not exactly a tens word, a hyphen and a unit 1-9.
int HyphenatedValue(const std::string &word)
{
	const size_t dash = word.find('-');
	if (dash == std::string::npos) {
		return -1;
	}
	const int tens = TensValue(word.substr(0, dash));
	const int unit = UnitValue(word.substr(dash + 1));
	return tens >= 0 && unit > 0 && unit < 10 ? tens + unit : -1;
}

bool IsWordChar(unsigned char c)
{
	return std::isalnum(c) != 0;
}

bool IsSpace(char c)
{
	return std::isspace(static_cast<unsigned char>(c)) != 0;
}

} // namespace

std::string Normalized::Original(size_t first, size_t last) const
{
	if (spans.empty() || first >= spans.size() || first > last) {
		return std::string();
	}
	last = std::min(last, spans.size() - 1);
	// Punctuation touching a token belongs to it ("World!", "\"hi\""), so the range is
	// widened over non-space bytes, but never into a neighbouring token's own range.
	size_t begin = spans[first].first;
	const size_t floor = first > 0 ? spans[first - 1].second : 0;
	while (begin > floor && !IsSpace(source[begin - 1])) {
		--begin;
	}
	size_t end = spans[last].second;
	const size_t ceiling = last + 1 < spans.size() ? spans[last + 1].first : source.size();
	while (end < ceiling && !IsSpace(source[end])) {
		++end;
	}
	return source.substr(begin, end - begin);
}

Normalized Normalize(const std::string &input)
{
	Normalized out;
	out.source = input;

	// 1. Split into tokens, remembering where each came from. An apostrophe or a
	// hyphen between two word characters stays; everything else is a separator.
	std::vector<std::string> raw;
	std::vector<std::pair<size_t, size_t>> spans;
	std::string current;
	size_t start = 0;
	for (size_t i = 0; i <= input.size(); ++i) {
		const unsigned char c = i < input.size() ? static_cast<unsigned char>(input[i]) : ' ';
		const bool inWord = IsWordChar(c) || c >= 0x80 ||
				    ((c == '\'' || c == '-') && !current.empty() && i + 1 < input.size() &&
				     IsWordChar(static_cast<unsigned char>(input[i + 1])));
		if (inWord) {
			if (current.empty()) {
				start = i;
			}
			current.push_back(static_cast<char>(c < 0x80 ? std::tolower(c) : c));
			continue;
		}
		if (!current.empty()) {
			raw.push_back(current);
			spans.emplace_back(start, i);
			current.clear();
		}
	}

	// 2. Collapse a run of single letters into one token: "b r b" -> "brb". Two letters
	// in a row are left alone ("a b" could be a scene name), three or more collapse.
	std::vector<std::string> merged;
	std::vector<std::pair<size_t, size_t>> mergedSpans;
	for (size_t i = 0; i < raw.size();) {
		size_t run = 0;
		while (i + run < raw.size() && raw[i + run].size() == 1 &&
		       std::isalpha(static_cast<unsigned char>(raw[i + run][0]))) {
			++run;
		}
		if (run >= 3) {
			std::string joined;
			for (size_t k = 0; k < run; ++k) {
				joined += raw[i + k];
			}
			merged.push_back(joined);
			mergedSpans.emplace_back(spans[i].first, spans[i + run - 1].second);
			i += run;
			continue;
		}
		merged.push_back(raw[i]);
		mergedSpans.push_back(spans[i]);
		++i;
	}

	// 3. Number words to digits, joining a tens word with a following unit.
	for (size_t i = 0; i < merged.size(); ++i) {
		const int tens = TensValue(merged[i]);
		if (tens >= 0) {
			int value = tens;
			size_t last = i;
			if (i + 1 < merged.size()) {
				const int unit = UnitValue(merged[i + 1]);
				if (unit > 0 && unit < 10) {
					value += unit;
					last = i + 1;
				}
			}
			out.tokens.push_back(std::to_string(value));
			out.spans.emplace_back(mergedSpans[i].first, mergedSpans[last].second);
			i = last;
			continue;
		}
		const int hyphenated = HyphenatedValue(merged[i]);
		const int unit = hyphenated >= 0 ? hyphenated : UnitValue(merged[i]);
		out.tokens.push_back(unit >= 0 ? std::to_string(unit) : merged[i]);
		out.spans.push_back(mergedSpans[i]);
	}

	for (const std::string &token : out.tokens) {
		if (!out.text.empty()) {
			out.text.push_back(' ');
		}
		out.text += token;
	}
	return out;
}

} // namespace Voice
