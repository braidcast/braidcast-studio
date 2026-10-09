#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_WAKE_GATE_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_WAKE_GATE_HPP_

#include <cstddef>
#include <string>

namespace Voice {

// How much of an utterance the wake check listens to. The wake phrase is at the very
// start by definition, so there is no reason to transcribe more of it with the tiny
// model before deciding.
inline constexpr size_t kWakeWindowMs = 2000;

struct WakeResult {
	bool matched = false;
	// What was said after the wake phrase, in the user's own spelling; empty when the
	// phrase was the whole utterance.
	std::string remainder;
	double score = 0.0;
};

// Whether `text` opens with `wakePhrase`, allowing for the recognizer mangling it, and
// what follows. Matching is fuzzy through the same threshold slot resolution uses, so
// "close enough" means one thing across the feature, with one rule of its own: part of
// the phrase is never enough ("hey" alone does not open "hey braidcast"). Pure.
WakeResult MatchWakePhrase(const std::string &text, const std::string &wakePhrase);

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_WAKE_GATE_HPP_
