#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_TEXT_NORMALIZE_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_TEXT_NORMALIZE_HPP_

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace Voice {

// A transcript flattened for matching, with a way back to the original text.
struct Normalized {
	std::string text;                // the normalized form, single-spaced
	std::vector<std::string> tokens; // text split on spaces
	// For each token, the byte range it came from in the original input.
	std::vector<std::pair<size_t, size_t>> spans;
	std::string source; // the original input, untouched

	// The original text spanning tokens [first, last], with the punctuation attached to
	// them ("World!" keeps its "!") and nothing else. Used where the user's own spelling
	// matters: a chat message, a scene name echoed back.
	std::string Original(size_t first, size_t last) const;
};

// Lowercases, drops punctuation that is not inside a word, collapses whitespace,
// turns number words 0-99 into digits ("twenty one" and "twenty-one" -> "21") and
// collapses spelled abbreviations ("b. r. b." -> "brb"). ASCII only: whisper's English
// models emit ASCII, and a non-ASCII byte is passed through untouched rather than
// mangled.
Normalized Normalize(const std::string &input);

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_TEXT_NORMALIZE_HPP_
