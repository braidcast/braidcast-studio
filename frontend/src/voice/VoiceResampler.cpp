#include "voice/VoiceResampler.hpp"

#include <cmath>

namespace Voice {

namespace {

constexpr double kPi = 3.14159265358979323846;
// 7 kHz: comfortably under the 8 kHz output Nyquist, above speech's useful band.
constexpr double kCutoffHz = 7000.0;

double Sinc(double x)
{
	if (std::fabs(x) < 1e-9) {
		return 1.0;
	}
	return std::sin(kPi * x) / (kPi * x);
}

} // namespace

Downsampler::Downsampler(int inputRate) : inputRate_(inputRate > 0 ? inputRate : kVoiceSampleRate)
{
	passthrough_ = inputRate_ <= kVoiceSampleRate;
	step_ = static_cast<double>(inputRate_) / static_cast<double>(kVoiceSampleRate);

	// Blackman-windowed sinc, normalized to unity gain at DC.
	const double fc = kCutoffHz / static_cast<double>(inputRate_); // cycles per sample
	const double mid = static_cast<double>(kTaps - 1) / 2.0;
	double sum = 0.0;
	for (size_t i = 0; i < kTaps; ++i) {
		const double n = static_cast<double>(i);
		const double window = 0.42 - 0.5 * std::cos(2.0 * kPi * n / static_cast<double>(kTaps - 1)) +
				      0.08 * std::cos(4.0 * kPi * n / static_cast<double>(kTaps - 1));
		const double h = 2.0 * fc * Sinc(2.0 * fc * (n - mid)) * window;
		coeff_[i] = static_cast<float>(h);
		sum += h;
	}
	if (sum > 0.0) {
		for (float &c : coeff_) {
			c = static_cast<float>(static_cast<double>(c) / sum);
		}
	}
	Reset();
}

void Downsampler::Reset()
{
	history_.fill(0.f);
	historyPos_ = 0;
	next_ = 1.0;
	prevFiltered_ = 0.f;
}

size_t Downsampler::Process(const float *in, size_t frames, float *out)
{
	if (!in || !out || frames > kMaxInFrames) {
		return 0;
	}
	if (passthrough_) {
		for (size_t i = 0; i < frames; ++i) {
			out[i] = in[i];
		}
		return frames;
	}

	size_t produced = 0;
	for (size_t i = 0; i < frames; ++i) {
		history_[historyPos_] = in[i];
		historyPos_ = (historyPos_ + 1) % kTaps;

		// Convolve the history. historyPos_ now points at the oldest sample, which
		// pairs with coeff_[0].
		float filtered = 0.f;
		size_t idx = historyPos_;
		for (size_t k = 0; k < kTaps; ++k) {
			filtered += coeff_[k] * history_[idx];
			idx = (idx + 1) % kTaps;
		}

		// Every output instant between the previous filtered sample and this one.
		while (next_ <= 1.0) {
			const float frac = static_cast<float>(next_);
			out[produced++] = prevFiltered_ + frac * (filtered - prevFiltered_);
			next_ += step_;
		}
		next_ -= 1.0;
		prevFiltered_ = filtered;
	}
	return produced;
}

} // namespace Voice
