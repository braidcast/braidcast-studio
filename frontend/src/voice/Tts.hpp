#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_TTS_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_TTS_HPP_

#include "voice/WavFile.hpp"

#include <cstddef>
#include <string>

namespace Voice {

// The longest confirmation worth speaking, in UTF-8 bytes. Read-back exists so the user
// does not have to look at the screen; anything longer than a sentence defeats that.
inline constexpr size_t kMaxSpokenChars = 200;

// Synthesizes `text` (UTF-8) into mono samples with the Windows speech API (SAPI 5,
// plain COM), at whatever rate the stream ends up in (16 kHz asked for). Synchronous,
// and it initializes COM on the calling thread for the call, so it runs on a worker,
// never on the UI thread.
//
// SAPI rather than the WinRT synthesizer because C++/WinRT's async API needs C++20
// coroutines and this repository builds at C++17 (index correction 10).
bool Synthesize(const std::string &text, WavData &out, std::string &error);

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_TTS_HPP_
