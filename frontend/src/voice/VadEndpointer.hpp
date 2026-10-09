#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_VAD_ENDPOINTER_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_VAD_ENDPOINTER_HPP_

#include <cstddef>
#include <string>
#include <vector>

struct whisper_vad_context;

namespace Voice {

// Where an utterance starts and stops, for always-listen mode. Wraps whisper.cpp's
// Silero VAD in streaming form (whisper_vad_detect_speech_no_reset keeps the LSTM
// state across calls), with hysteresis: speech starts at a high probability and ends
// only after a run of low ones, so a pause between words does not end the utterance.
//
// Recognizer worker thread only.
class VadEndpointer {
public:
	// Probability to call it speech, and the lower bar it has to fall under to stop
	// counting as speech. Two numbers rather than one, because a single threshold
	// flaps on every breath.
	static constexpr float kStartProbability = 0.6f;
	static constexpr float kEndProbability = 0.35f;
	// How long the probabilities must stay low before the utterance is over. Long
	// enough to speak a comma, short enough not to feel laggy.
	static constexpr size_t kEndSilenceMs = 600;
	// A single utterance never runs longer than this.
	static constexpr size_t kMaxUtteranceMs = 15000;
	// Silero's window at 16 kHz: whisper.cpp v1.9.4 reads n_window (512) from the model
	// and ZERO-PADS a trailing partial window, then runs it through the LSTM like any
	// other (src/whisper.cpp, whisper_vad_detect_speech_no_reset). A stream fed in
	// arbitrary blocks would therefore see a burst of silence spliced in after every
	// block, so Push holds back the remainder and only ever hands whisper whole windows.
	static constexpr size_t kWindowSamples = 512;

	enum class State {
		Silence, // nothing is being said
		Speech,  // an utterance is under way
		Ended,   // the utterance just finished; the next Push starts fresh
	};

	VadEndpointer() = default;
	~VadEndpointer();
	VadEndpointer(const VadEndpointer &) = delete;
	VadEndpointer &operator=(const VadEndpointer &) = delete;

	// `modelPath` is UTF-8: whisper widens it itself on MSVC, as for the speech model.
	bool Start(const std::string &modelPath, int threads, std::string &error);
	void Stop();
	bool Ready() const { return ctx_ != nullptr; }

	// Feeds 16 kHz mono samples and reports the state after them. Any block size works;
	// whole windows are what reach the model, and a remainder waits for the next call.
	// Ended is reported once, by the call whose samples finished the utterance; the
	// rest of that call's windows are not judged (at most a few tens of ms, when the
	// caller pushes a few windows at a time).
	State Push(const float *samples, size_t count);

	// Forget the utterance in progress, the held-back remainder and the LSTM state.
	void Reset();

	// How long the utterance in progress has run, pauses inside it included. After the
	// Push that reported Ended, how long the utterance that just ended ran, without the
	// silence that ended it.
	size_t SpeechMs() const { return speechMs_; }

private:
	whisper_vad_context *ctx_ = nullptr;
	std::vector<float> pending_; // less than one window, waiting for the rest
	bool inSpeech_ = false;
	size_t silenceMs_ = 0;
	size_t speechMs_ = 0;
};

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_VAD_ENDPOINTER_HPP_
