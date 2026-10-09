#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_CAPTURE_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_CAPTURE_HPP_

#include "voice/VoiceResampler.hpp"
#include "voice/VoiceRing.hpp"

#include <obs.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace Voice {

// Taps the microphone's audio and feeds 16 kHz mono into `ring`.
//
// OnAudio runs on the source's audio thread: it allocates nothing, takes no lock, logs
// nothing and calls neither whisper nor libobs. Everything it touches (the downmix
// scratch, the resampler, the cached rate and channel count, the ring) is set up in
// Bind, on the UI thread, before the callback is registered; the registration takes the
// same audio_cb_mutex libobs holds while dispatching, so the callback sees it complete.
//
// Bind/Unbind/WatchChannelChanges run on the UI thread, and so does the destructor.
// Unbind removes the libobs callback before releasing the source reference, so no
// callback can be in flight afterwards. The ring is never reset here: its consumer (the
// recognizer worker) may be reading it, and Reset is only safe with both sides idle.
class VoiceCapture {
public:
	explicit VoiceCapture(SpscRing &ring);
	~VoiceCapture();
	VoiceCapture(const VoiceCapture &) = delete;
	VoiceCapture &operator=(const VoiceCapture &) = delete;

	// Binds to whatever source is on global audio `channel`. Returns false when the
	// channel is empty; the capture then stays unbound and waits for a channel change.
	bool Bind(uint32_t channel);
	void Unbind();
	bool Bound() const { return bound_ != nullptr; }
	uint32_t Channel() const { return channel_.load(std::memory_order_acquire); }
	// The bound source's name, for the UI's "listening to <device>"; empty when unbound.
	std::string DeviceName() const;

	// Watches the global channel_change signal and rebinds when the bound channel's
	// source is swapped. `onChanged` runs on the UI thread after a rebind (check
	// Bound() there: the new source may be none).
	void WatchChannelChanges(std::function<void()> onChanged);
	void StopWatchingChannelChanges();

	// True if any block since the last call arrived muted. Called once per segment, from
	// the recognizer worker. libobs reports our own push-to-talk mute here too.
	bool TakeMutedSeen() { return mutedSeen_.exchange(false, std::memory_order_acq_rel); }

	// Test seam: feed planar float audio exactly as libobs would, at `rate`. Only while
	// unbound (it may rebuild the resampler, which the audio thread would be using).
	void FeedForTest(int rate, const float *const *planes, size_t channels, size_t frames, bool muted);
	int InputRateForTest() const { return resampler_ ? resampler_->InputRate() : 0; }

private:
	static void OnAudioThunk(void *param, obs_source_t *source, const struct audio_data *data, bool muted);
	static void OnChannelChange(void *param, calldata_t *data);
	void OnAudio(const struct audio_data &data, bool muted);
	void Feed(const float *const *planes, size_t channels, size_t frames, bool muted);
	void EnsureResampler(int rate);

	SpscRing &ring_;
	OBSSource bound_;
	std::atomic<uint32_t> channel_{0};
	std::atomic<bool> mutedSeen_{false};
	std::unique_ptr<Downsampler> resampler_;
	// The mix format, read from libobs in Bind so the audio thread never has to ask.
	size_t channels_ = 0;
	std::function<void()> onChanged_;
	bool watching_ = false;
	// Expires when watching stops or the capture is destroyed, so a rebind already queued
	// on the UI thread does nothing to a capture that has gone away.
	std::shared_ptr<int> watchToken_;
	// Audio-thread scratch, sized once so the callback never allocates.
	std::array<float, Downsampler::kMaxInFrames> mono_{};
	std::array<float, Downsampler::kMaxOutFrames> out_{};
};

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_CAPTURE_HPP_
