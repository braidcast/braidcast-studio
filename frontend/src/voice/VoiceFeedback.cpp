#include "voice/VoiceFeedback.hpp"

#include "log.hpp"
#include "util/paths.hpp"

#include <util/platform.h>

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

namespace Voice {

namespace {

// libobs takes audio in blocks; 20 ms is the usual cadence and keeps a cue's latency
// imperceptible.
constexpr size_t kBlockMs = 20;

// A source that produces audio from us and nothing else: no properties, no settings,
// no video. OBS_SOURCE_CAP_DISABLED keeps it out of every "add source" list.
obs_source_info MakeCueSourceInfo()
{
	obs_source_info info = {};
	info.id = kFeedbackSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_CAP_DISABLED;
	info.get_name = [](void *) -> const char * {
		return kFeedbackSourceName;
	};
	info.create = [](obs_data_t *, obs_source_t *source) -> void * {
		return source;
	};
	info.destroy = [](void *) {
	};
	return info;
}

// Once per process: libobs warns about, and refuses, a second registration of an id.
// UI thread, like every caller of Start.
void RegisterCueSourceOnce()
{
	static bool registered = false;
	if (!registered) {
		const obs_source_info info = MakeCueSourceInfo();
		obs_register_source(&info);
		registered = true;
	}
}

} // namespace

VoiceFeedback::~VoiceFeedback()
{
	Stop();
}

bool VoiceFeedback::Start(std::string &error)
{
	// The rundir layout mirrors the installed one: <data>/braidcast/voice/cues.
	return StartFrom(RundirRoot() + "/data/braidcast/voice/cues", error);
}

bool VoiceFeedback::StartFrom(const std::string &cueDir, std::string &error)
{
	if (source_) {
		return true;
	}

	loaded_ = 0;
	for (size_t i = 0; i < cues_.size(); ++i) {
		cues_[i] = WavData{};
		const std::string path = cueDir + "/" + CueName(static_cast<Cue>(i)) + ".wav";
		std::string why;
		if (LoadWavMono(path, cues_[i], why) && cues_[i].sampleRate > 0) {
			++loaded_;
		} else {
			cues_[i] = WavData{};
			HostLog(std::string("[voice] cue missing: ") + CueName(static_cast<Cue>(i)) +
				(why.empty() ? std::string() : " (" + why + ")"));
		}
	}
	if (loaded_ == 0) {
		error = "no cue sounds were found in " + cueDir;
		return false;
	}

	RegisterCueSourceOnce();
	// Private: it plays audio but never appears in the user's source list, and nothing
	// saves it into the scene collection.
	source_ = obs_source_create_private(kFeedbackSourceId, kFeedbackSourceName, nullptr);
	if (!source_) {
		error = "could not create the cue source";
		return false;
	}
	obs_source_set_monitoring_type(source_, OBS_MONITORING_TYPE_MONITOR_ONLY);
	// The monitor drops audio from a source nothing holds active (wasapi-output.c's
	// on_audio_playback checks activate_refs), and this one is in no scene and on no
	// channel, so it holds itself active for as long as it exists.
	obs_source_inc_active(source_);

	{
		std::lock_guard<std::mutex> lock(mutex_);
		quit_ = false;
		queued_ = -1;
		speechWaiting_ = false;
		speechQueued_ = WavData{};
	}
	player_ = std::thread(&VoiceFeedback::PlayerMain, this);
	DBG(LogCat::Voice, "cue source up, %zu of %zu cues", loaded_, cues_.size());
	return true;
}

void VoiceFeedback::Stop()
{
	if (player_.joinable()) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			quit_ = true;
		}
		cv_.notify_all();
		player_.join();
	}
	if (source_) {
		obs_source_dec_active(source_);
		source_ = nullptr; // releases the creation reference
	}
}

void VoiceFeedback::Play(Cue cue, double volume)
{
	const size_t index = static_cast<size_t>(cue);
	if (!(volume > 0.0) || !source_ || index >= cues_.size() || cues_[index].samples.empty()) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		queued_ = static_cast<int>(index);
		volume_ = std::min(volume, 1.0);
	}
	cv_.notify_all();
}

void VoiceFeedback::PlaySamples(WavData audio, double volume)
{
	if (!(volume > 0.0) || !source_ || audio.samples.empty() || audio.sampleRate <= 0) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		speechQueued_ = std::move(audio);
		speechVolume_ = std::min(volume, 1.0);
		speechWaiting_ = true;
	}
	cv_.notify_all();
}

void VoiceFeedback::PlayerMain()
{
	std::vector<float> block;
	for (;;) {
		size_t cue = 0;
		double volume = 0.0;
		bool speech = false;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait(lock, [this] { return quit_ || queued_ >= 0 || speechWaiting_; });
			if (quit_) {
				return;
			}
			// A cue first when both wait: it is short, and the words follow it.
			if (queued_ >= 0) {
				cue = static_cast<size_t>(queued_);
				volume = volume_;
				queued_ = -1;
			} else {
				speech = true;
				speechPlaying_ = std::move(speechQueued_);
				speechQueued_ = WavData{};
				volume = speechVolume_;
				speechWaiting_ = false;
			}
		}

		const WavData &wav = speech ? speechPlaying_ : cues_[cue];
		const size_t blockFrames = static_cast<size_t>(wav.sampleRate) * kBlockMs / 1000;
		block.assign(blockFrames, 0.f);
		obs_source_set_volume(source_, static_cast<float>(volume));

		for (size_t offset = 0; offset < wav.samples.size(); offset += blockFrames) {
			const size_t take = std::min(blockFrames, wav.samples.size() - offset);
			std::copy_n(wav.samples.begin() + static_cast<std::ptrdiff_t>(offset), take, block.begin());
			std::fill(block.begin() + static_cast<std::ptrdiff_t>(take), block.end(), 0.f);

			obs_source_audio audio = {};
			audio.data[0] = reinterpret_cast<const uint8_t *>(block.data());
			audio.frames = static_cast<uint32_t>(block.size());
			audio.speakers = SPEAKERS_MONO;
			audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
			audio.samples_per_sec = static_cast<uint32_t>(wav.sampleRate);
			audio.timestamp = os_gettime_ns();
			obs_source_output_audio(source_, &audio);

			// Paced in real time, and woken at once by Stop or a newer cue; newer
			// samples cut short only samples, never a cue.
			std::unique_lock<std::mutex> lock(mutex_);
			if (cv_.wait_for(lock, std::chrono::milliseconds(kBlockMs), [this, speech] {
				    return quit_ || queued_ >= 0 || (speech && speechWaiting_);
			    })) {
				break;
			}
		}
	}
}

} // namespace Voice
