#include "voice/WakeGate.hpp"

#include "voice/FuzzyMatch.hpp"
#include "voice/TextNormalize.hpp"

#include <algorithm>
#include <vector>

namespace Voice {

namespace {

// True when `part` is a shorter run of the phrase's own words ("hey" or "braidcast" of
// "hey braidcast"). Similarity scores that high on purpose (a scene called by one word
// of its name), but for a wake phrase it would let one common word wake the app.
bool IsPartOfPhrase(const std::vector<std::string> &part, const std::vector<std::string> &phrase)
{
	return part.size() < phrase.size() &&
	       std::search(phrase.begin(), phrase.end(), part.begin(), part.end()) != phrase.end();
}

} // namespace

WakeResult MatchWakePhrase(const std::string &text, const std::string &wakePhrase)
{
	WakeResult result;
	const Normalized said = Normalize(text);
	const Normalized wake = Normalize(wakePhrase);
	if (said.tokens.empty() || wake.tokens.empty()) {
		return result;
	}

	// Compare the opening of the utterance against the phrase, trying the phrase's own
	// length and one token either side: the recognizer splits or merges words
	// ("braidcast" -> "braid cast"), and a filler word may come first ("okay braidcast"),
	// which changes the token count. The best score wins; on a tie, the longer opening.
	const size_t wanted = wake.tokens.size();
	const size_t most = std::min(said.tokens.size(), wanted + 1);
	const size_t least = wanted > 1 ? wanted - 1 : 1;
	for (size_t take = most; take >= least; --take) {
		const std::vector<std::string> opening(said.tokens.begin(),
						       said.tokens.begin() + static_cast<std::ptrdiff_t>(take));
		if (!IsPartOfPhrase(opening, wake.tokens)) {
			// A hyphen inside a token is how the recognizer writes a split name
			// ("Braid-cast"); Normalize keeps it, so it is dropped here.
			std::string joined;
			for (const std::string &token : opening) {
				if (!joined.empty()) {
					joined.push_back(' ');
				}
				for (char c : token) {
					if (c != '-') {
						joined.push_back(c);
					}
				}
			}
			const double score = Similarity(joined, wake.text);
			if (score >= kSlotAcceptThreshold && score > result.score) {
				result.matched = true;
				result.score = score;
				result.remainder = take < said.tokens.size()
							   ? said.Original(take, said.tokens.size() - 1)
							   : std::string();
			}
		}
		if (take == least) {
			break; // size_t: do not decrement past the floor
		}
	}
	return result;
}

} // namespace Voice
