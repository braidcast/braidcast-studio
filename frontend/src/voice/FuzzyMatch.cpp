#include "voice/FuzzyMatch.hpp"

#include "voice/TextNormalize.hpp"

#include <algorithm>
#include <set>

namespace Voice {

namespace {

// The same letters with different spacing ("game play" for "Gameplay", "web cam" for
// "Webcam") is how speech most often misses a typed name. Just under an exact match,
// so a name spelled exactly as heard still wins over one that is only spaced alike.
constexpr double kSameLettersScore = 0.95;

// A candidate that contains the query's words in a row scores from this floor up to 1,
// by how much of the candidate the query covers.
constexpr double kContainedFloor = 0.8;

std::multiset<std::string> Bigrams(const std::string &text)
{
	std::multiset<std::string> out;
	for (size_t i = 0; i + 1 < text.size(); ++i) {
		out.insert(text.substr(i, 2));
	}
	return out;
}

// Sorensen-Dice over multisets: twice the overlap over the total size.
double Dice(const std::multiset<std::string> &a, const std::multiset<std::string> &b)
{
	if (a.empty() || b.empty()) {
		return a.empty() && b.empty() ? 1.0 : 0.0;
	}
	size_t overlap = 0;
	for (auto it = a.begin(); it != a.end(); it = a.upper_bound(*it)) {
		overlap += std::min(a.count(*it), b.count(*it));
	}
	return 2.0 * static_cast<double>(overlap) / static_cast<double>(a.size() + b.size());
}

std::string WithoutSpaces(const std::string &text)
{
	std::string out;
	out.reserve(text.size());
	for (char c : text) {
		if (c != ' ') {
			out.push_back(c);
		}
	}
	return out;
}

// True when `needle` appears in `haystack` as a run of whole tokens. Whole tokens, not
// a substring: "art" must not find "Starting Soon", which a byte search would.
bool ContainsTokenRun(const std::vector<std::string> &haystack, const std::vector<std::string> &needle)
{
	if (needle.empty() || needle.size() > haystack.size()) {
		return false;
	}
	return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

} // namespace

double Similarity(const std::string &query, const std::string &candidate)
{
	const Normalized nq = Normalize(query);
	const Normalized nc = Normalize(candidate);
	const std::string &q = nq.text;
	const std::string &c = nc.text;
	if (q.empty() || c.empty()) {
		return 0.0;
	}
	if (q == c) {
		return 1.0;
	}

	const double charSim = Dice(Bigrams(q), Bigrams(c));
	const double tokenSim = Dice(std::multiset<std::string>(nq.tokens.begin(), nq.tokens.end()),
				     std::multiset<std::string>(nc.tokens.begin(), nc.tokens.end()));
	double score = 0.7 * charSim + 0.3 * tokenSim;

	if (WithoutSpaces(q) == WithoutSpaces(c)) {
		score = std::max(score, kSameLettersScore);
	}

	// "gameplay" inside "gameplay cam" is a deliberate abbreviation, not a near miss.
	// Score it high, minus a penalty for how much of the candidate went unspoken.
	if (ContainsTokenRun(nc.tokens, nq.tokens)) {
		const double covered = static_cast<double>(q.size()) / static_cast<double>(c.size());
		score = std::max(score, kContainedFloor + (1.0 - kContainedFloor) * covered);
	}
	return std::min(1.0, score);
}

SlotMatch BestMatch(const std::string &query, const std::vector<std::string> &candidates)
{
	SlotMatch best;
	double second = 0.0;
	std::string secondName;
	for (size_t i = 0; i < candidates.size(); ++i) {
		const double score = Similarity(query, candidates[i]);
		if (score > best.score) {
			second = best.score;
			secondName = best.score > 0.0 ? candidates[best.index] : std::string();
			best.index = i;
			best.score = score;
		} else if (score > second) {
			second = score;
			secondName = candidates[i];
		}
	}
	if (best.score < kSlotAcceptThreshold) {
		return SlotMatch{};
	}
	// An exact match is not a guess, however close a longer name containing it comes
	// ("BRB" against "BRB 2"); only a second exact match makes it ambiguous.
	const bool exactWinner = best.score >= 1.0 && second < 1.0;
	if (!exactWinner && best.score - second < kSlotMargin) {
		SlotMatch ambiguous;
		ambiguous.ambiguous = true;
		ambiguous.index = best.index;
		ambiguous.score = best.score;
		ambiguous.runnerUp = secondName;
		return ambiguous;
	}
	best.ok = true;
	return best;
}

std::string SpokenHandle(const std::string &handle)
{
	auto lower = [](char c) {
		return c >= 'a' && c <= 'z';
	};
	auto upper = [](char c) {
		return c >= 'A' && c <= 'Z';
	};
	auto digit = [](char c) {
		return c >= '0' && c <= '9';
	};
	auto letter = [&](char c) {
		return lower(c) || upper(c);
	};

	std::string out;
	out.reserve(handle.size() + 8);
	for (size_t i = 0; i < handle.size(); ++i) {
		const char c = handle[i];
		if (c == '_' || c == '-' || c == '.') {
			out.push_back(' ');
			continue;
		}
		if (i > 0) {
			const char prev = handle[i - 1];
			const bool camel = (lower(prev) || digit(prev)) && upper(c);
			const bool numberEdge = (letter(prev) && digit(c)) || (digit(prev) && letter(c));
			if (camel || numberEdge) {
				out.push_back(' ');
			}
		}
		out.push_back(c);
	}
	return out;
}

} // namespace Voice
