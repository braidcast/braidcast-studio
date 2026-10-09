#include "voice/VadEndpointer.hpp"

#include "log.hpp"
#include "voice/VoiceResampler.hpp"

#include <whisper.h>

#include <cstddef>

namespace Voice {

VadEndpointer::~VadEndpointer()
{
	Stop();
}

bool VadEndpointer::Start(const std::string &modelPath, int threads, std::string &error)
{
	Stop();
	whisper_vad_context_params params = whisper_vad_default_context_params();
	params.n_threads = threads;
	params.use_gpu = false; // the CPU backend is the only one linked
	// UTF-8 as is, as Recognizer::LoadModel does: whisper widens the VAD path itself on
	// MSVC too (whisper_vad_init_from_file_with_params).
	ctx_ = whisper_vad_init_from_file_with_params(modelPath.c_str(), params);
	if (!ctx_) {
		error = "could not load the voice activity model";
		return false;
	}
	Reset();
	return true;
}

void VadEndpointer::Stop()
{
	if (ctx_) {
		whisper_vad_free(ctx_);
		ctx_ = nullptr;
	}
	pending_.clear();
	inSpeech_ = false;
	silenceMs_ = 0;
	speechMs_ = 0;
}

void VadEndpointer::Reset()
{
	if (ctx_) {
		whisper_vad_reset_state(ctx_);
	}
	pending_.clear();
	inSpeech_ = false;
	silenceMs_ = 0;
	speechMs_ = 0;
}

VadEndpointer::State VadEndpointer::Push(const float *samples, size_t count)
{
	const State unchanged = inSpeech_ ? State::Speech : State::Silence;
	if (!ctx_ || !samples || count == 0) {
		return unchanged;
	}
	pending_.insert(pending_.end(), samples, samples + count);
	const size_t whole = pending_.size() / kWindowSamples * kWindowSamples;
	if (whole == 0) {
		return unchanged;
	}
	// The streaming form: it keeps the LSTM state, so consecutive calls behave as one
	// continuous stream rather than as independent clips.
	const bool ok = whisper_vad_detect_speech_no_reset(ctx_, pending_.data(), static_cast<int>(whole));
	pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(whole));
	if (!ok) {
		return unchanged;
	}

	// Valid until the next call into the context.
	const int probCount = whisper_vad_n_probs(ctx_);
	const float *probs = whisper_vad_probs(ctx_);
	if (probCount <= 0 || !probs) {
		return unchanged;
	}
	// Each probability covers an equal slice of what was just judged: one window, 32 ms,
	// for Silero at 16 kHz. Derived rather than assumed, so a model with a smaller
	// window still counts time right.
	const size_t msPerProb =
		(whole * 1000 / static_cast<size_t>(kVoiceSampleRate)) / static_cast<size_t>(probCount);

	for (int i = 0; i < probCount; ++i) {
		const float p = probs[i];
		if (!inSpeech_) {
			if (p >= kStartProbability) {
				inSpeech_ = true;
				speechMs_ = msPerProb;
				silenceMs_ = 0;
			}
			continue;
		}
		speechMs_ += msPerProb;
		if (p < kEndProbability) {
			silenceMs_ += msPerProb;
			if (silenceMs_ >= kEndSilenceMs) {
				DBG(LogCat::Voice, "vad: utterance ended after %zu ms", speechMs_);
				inSpeech_ = false;
				silenceMs_ = 0;
				return State::Ended;
			}
		} else {
			silenceMs_ = 0;
		}
		if (speechMs_ >= kMaxUtteranceMs) {
			DBG(LogCat::Voice, "vad: utterance hit the %zu ms cap", kMaxUtteranceMs);
			inSpeech_ = false;
			silenceMs_ = 0;
			return State::Ended;
		}
	}
	return inSpeech_ ? State::Speech : State::Silence;
}

} // namespace Voice
