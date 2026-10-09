#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_WAV_FILE_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_WAV_FILE_HPP_

#include <cstdint>
#include <string>
#include <vector>

namespace Voice {

struct WavData {
	int sampleRate = 0;
	std::vector<float> samples; // mono, -1 to 1
};

// Reads a RIFF/WAVE file into mono floats: 16-bit PCM or 32-bit float, any channel
// count (averaged down). Enough for the assets braidcast ships (the recognizer
// fixture and the command cues) and nothing more; it is not a general decoder, and it
// refuses anything else with a reason rather than guessing.
bool LoadWavMono(const std::string &utf8Path, WavData &out, std::string &error);

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_WAV_FILE_HPP_
