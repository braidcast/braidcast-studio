#include "voice/VoiceCapture.hpp"

#include "log.hpp"
#include "util/async_task.hpp"

#include <media-io/audio-io.h>

#include <algorithm>
#include <cstring>

namespace Voice {

VoiceCapture::VoiceCapture(SpscRing &ring) : ring_(ring) {}

VoiceCapture::~VoiceCapture()
{
	StopWatchingChannelChanges();
	Unbind();
}

void VoiceCapture::EnsureResampler(int rate)
{
	if (!resampler_ || resampler_->InputRate() != rate) {
		// UI thread only, with the callback unregistered: Bind and FeedForTest reach
		// here, the audio callback never does.
		resampler_ = std::make_unique<Downsampler>(rate);
	}
}

bool VoiceCapture::Bind(uint32_t channel)
{
	Unbind();
	channel_.store(channel, std::memory_order_release);
	OBSSourceAutoRelease src = obs_get_output_source(channel);
	if (!src) {
		DBG(LogCat::Voice, "capture: channel %u has no source", channel);
		return false;
	}
	// The callback receives the mix format (planar float at the output rate and channel
	// count), so read it here once: the audio thread may not call libobs. An audio reset
	// changes it, and the engine rebinds after every reset.
	audio_t *audio = obs_get_audio();
	const int rate = audio ? static_cast<int>(audio_output_get_sample_rate(audio)) : 0;
	channels_ = audio ? audio_output_get_channels(audio) : 0;
	if (rate <= 0 || channels_ == 0) {
		DBG(LogCat::Voice, "capture: no audio output to read the mix format from");
		return false;
	}
	EnsureResampler(rate);
	resampler_->Reset();
	mutedSeen_.store(false, std::memory_order_release);
	bound_ = src.Get();
	obs_source_add_audio_capture_callback(bound_, &VoiceCapture::OnAudioThunk, this);
	DBG(LogCat::Voice, "capture: bound to '%s' on channel %u at %d Hz, %zu channel(s)", obs_source_get_name(bound_),
	    channel, rate, channels_);
	return true;
}

void VoiceCapture::Unbind()
{
	if (!bound_) {
		return;
	}
	// Remove first: this takes the same mutex libobs holds while running callbacks, so
	// no callback is in flight once it returns and the source ref can safely drop.
	obs_source_remove_audio_capture_callback(bound_, &VoiceCapture::OnAudioThunk, this);
	DBG(LogCat::Voice, "capture: unbound from '%s'", obs_source_get_name(bound_));
	bound_ = nullptr;
}

std::string VoiceCapture::DeviceName() const
{
	if (!bound_) {
		return std::string();
	}
	const char *name = obs_source_get_name(bound_);
	return name ? name : std::string();
}

void VoiceCapture::WatchChannelChanges(std::function<void()> onChanged)
{
	if (watching_) {
		return;
	}
	onChanged_ = std::move(onChanged);
	watchToken_ = std::make_shared<int>(0);
	signal_handler_connect(obs_get_signal_handler(), "channel_change", &VoiceCapture::OnChannelChange, this);
	watching_ = true;
}

void VoiceCapture::StopWatchingChannelChanges()
{
	if (!watching_) {
		return;
	}
	// Disconnecting waits out an emission in progress on another thread (libobs holds
	// the signal's mutex while it runs the callbacks), so none is in flight afterwards.
	signal_handler_disconnect(obs_get_signal_handler(), "channel_change", &VoiceCapture::OnChannelChange, this);
	watching_ = false;
	watchToken_.reset();
	onChanged_ = nullptr;
}

void VoiceCapture::OnChannelChange(void *param, calldata_t *data)
{
	auto *self = static_cast<VoiceCapture *>(param);
	long long channel = 0;
	if (!calldata_get_int(data, "channel", &channel) || channel < 0 ||
	    static_cast<uint32_t>(channel) != self->channel_.load(std::memory_order_acquire)) {
		return;
	}
	// libobs emits this while holding its channel mutex, so the rebind (which calls back
	// into libobs) has to happen off this stack. The token guards the queued task against
	// a capture that is stopped or destroyed before it runs; both happen on the UI thread,
	// as the task does, so checking it there is enough.
	const uint32_t ch = static_cast<uint32_t>(channel);
	std::weak_ptr<int> token = self->watchToken_;
	AsyncTask::QueueOnUi([self, ch, token] {
		// Also dropped when the capture was bound to another channel after this was
		// queued (the engine follows the mic to whichever channel holds it now): binding
		// `ch` again would undo that.
		if (token.expired() || self->channel_.load(std::memory_order_acquire) != ch) {
			return;
		}
		self->Bind(ch);
		if (self->onChanged_) {
			self->onChanged_();
		}
	});
}

void VoiceCapture::OnAudioThunk(void *param, obs_source_t *, const struct audio_data *data, bool muted)
{
	if (data) {
		static_cast<VoiceCapture *>(param)->OnAudio(*data, muted);
	}
}

// The source's audio thread. No allocation, lock, log, whisper or libobs call: only
// the cached format, the preallocated scratch, the resampler and the ring.
void VoiceCapture::OnAudio(const struct audio_data &data, bool muted)
{
	const float *planes[MAX_AV_PLANES] = {};
	const size_t channels = std::min<size_t>(channels_, MAX_AV_PLANES);
	for (size_t i = 0; i < channels; ++i) {
		planes[i] = reinterpret_cast<const float *>(data.data[i]);
	}
	Feed(planes, channels, data.frames, muted);
}

void VoiceCapture::Feed(const float *const *planes, size_t channels, size_t frames, bool muted)
{
	if (muted) {
		mutedSeen_.store(true, std::memory_order_release);
	}
	if (!resampler_ || !planes || channels == 0 || frames == 0 || !planes[0]) {
		return;
	}

	size_t done = 0;
	while (done < frames) {
		const size_t take = std::min<size_t>(Downsampler::kMaxInFrames, frames - done);
		if (channels == 1) {
			std::memcpy(mono_.data(), planes[0] + done, take * sizeof(float));
		} else {
			const float scale = 1.0f / static_cast<float>(channels);
			for (size_t i = 0; i < take; ++i) {
				float sum = 0.f;
				for (size_t c = 0; c < channels; ++c) {
					sum += planes[c] ? planes[c][done + i] : 0.f;
				}
				mono_[i] = sum * scale;
			}
		}
		const size_t produced = resampler_->Process(mono_.data(), take, out_.data());
		ring_.Write(out_.data(), produced);
		done += take;
	}
}

void VoiceCapture::FeedForTest(int rate, const float *const *planes, size_t channels, size_t frames, bool muted)
{
	if (Bound()) {
		return;
	}
	EnsureResampler(rate);
	Feed(planes, channels, frames, muted);
}

} // namespace Voice
