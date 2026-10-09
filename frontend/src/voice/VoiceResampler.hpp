#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_RESAMPLER_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_RESAMPLER_HPP_

#include <array>
#include <cstddef>

namespace Voice {

inline constexpr int kVoiceSampleRate = 16000;

// Mono any-rate -> 16 kHz for whisper, safe to run on the libobs audio thread: no
// allocation, no locks, no syscalls, fixed work per sample. A 63-tap windowed-sinc
// low pass at 7 kHz (below the 8 kHz output Nyquist) runs at the input rate, and each
// output sample is linearly interpolated between the two filtered samples around its
// fractional position. Interpolating, rather than taking the nearest filtered sample,
// matters at 44.1 kHz, where the ratio is not an integer: the nearest-sample pick jitters
// by up to one input period and costs 15-25 dB of signal-to-noise in the speech band.
//
// Input at exactly 16 kHz is passed through untouched. A lower rate is passed through
// too and would play fast; settings.setAudio only offers 44.1 and 48 kHz.
//
// One instance per capture; not thread safe, and only the audio thread calls Process.
class Downsampler {
public:
	// The most input frames one Process call accepts. libobs hands over about 10 ms
	// per callback (480 frames at 48 kHz); 4096 is headroom for a slow buffer.
	static constexpr size_t kMaxInFrames = 4096;
	// The output ceiling for kMaxInFrames at the lowest supported input rate.
	static constexpr size_t kMaxOutFrames = kMaxInFrames + 8;
	static constexpr size_t kTaps = 63;

	explicit Downsampler(int inputRate);

	int InputRate() const { return inputRate_; }

	// Filters and decimates `frames` mono samples into `out`, which must hold at least
	// kMaxOutFrames. Returns the number of frames written. Returns 0 for a block longer
	// than kMaxInFrames rather than writing past the caller's buffer.
	size_t Process(const float *in, size_t frames, float *out);

	// Drops the filter history, for a new segment after a gap.
	void Reset();

private:
	int inputRate_;
	bool passthrough_;
	double step_ = 1.0; // input samples consumed per output sample
	// Where the next output sample falls, in input samples after the previous filtered
	// sample: it lies between prevFiltered_ (at 0) and the current one (at 1) once <= 1.
	double next_ = 1.0;
	float prevFiltered_ = 0.f;
	std::array<float, kTaps> coeff_{};
	std::array<float, kTaps> history_{}; // the last kTaps input samples, circular
	size_t historyPos_ = 0;
};

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_RESAMPLER_HPP_
