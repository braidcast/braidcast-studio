#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_FUZZY_MATCH_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_FUZZY_MATCH_HPP_

#include <cstddef>
#include <string>
#include <vector>

namespace Voice {

// How close a slot has to be before it is accepted at all.
inline constexpr double kSlotAcceptThreshold = 0.75;
// How far ahead the winner must be before the choice is unambiguous. Two scenes with
// near-identical names must ask rather than guess: switching to the wrong one on air
// is worse than admitting confusion.
inline constexpr double kSlotMargin = 0.08;

// 0 to 1, where 1 means the same words. Blends character-bigram overlap with
// whole-token overlap; a candidate whose words include the query's words in a row
// ("gameplay" in "Gameplay Cam") scores high, penalized by how much extra it carries;
// and the same letters spaced differently ("game play" for "Gameplay") score just
// under an exact match. Both inputs go through Normalize here.
double Similarity(const std::string &query, const std::string &candidate);

struct SlotMatch {
	bool ok = false;        // a single clear winner
	bool ambiguous = false; // two or more candidates too close to choose between
	size_t index = 0;
	double score = 0.0;
	std::string runnerUp; // the other candidate, when ambiguous
};

// The best candidate for `query`, or a miss. Candidates are matched case-insensitively
// and through the same normalization as the transcript. An exact match wins outright
// over any candidate that merely contains it ("BRB" over "BRB 2").
SlotMatch BestMatch(const std::string &query, const std::vector<std::string> &candidates);

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_FUZZY_MATCH_HPP_
