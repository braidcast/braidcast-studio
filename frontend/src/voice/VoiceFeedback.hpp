#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_FEEDBACK_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_FEEDBACK_HPP_

#include "voice/VoiceListener.hpp"
#include "voice/WavFile.hpp"

#include <obs.hpp>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>

namespace Voice {

inline constexpr const char *kFeedbackSourceId = "braidcast_voice_cues";
inline constexpr const char *kFeedbackSourceName = "Braidcast Voice Cues";
// One per Voice::Cue, in its order (the spec's seven); the files are named by CueName.
inline constexpr size_t kCueCount = 7;

// Plays the command cues to the user only. The source is private (it never appears in
// the user's source list and nothing saves it) and monitor-only, so the cue reaches the
// monitoring device and not the stream mix (libobs skips the mix for a monitor-only
// source in source_output_audio_data and still feeds the monitor's capture callback).
//
// Caveat worth knowing, and warned about in the UI: if the monitoring device is also
// what a Desktop Audio source captures, the audience hears the cue anyway. That is a
// property of loopback capture, not of this code (AudioEndpoints).
//
// Threads: Start, Stop and Play on the UI thread; one player thread paces the samples
// into the source in real time.
class VoiceFeedback {
public:
	VoiceFeedback() = default;
	~VoiceFeedback();
	VoiceFeedback(const VoiceFeedback &) = delete;
	VoiceFeedback &operator=(const VoiceFeedback &) = delete;

	// Loads the cues from <rundir>/data/braidcast/voice/cues and creates the source.
	// False, with a reason, when no cue could be loaded or the source not created;
	// voice control works without cues.
	bool Start(std::string &error);
	// The same from another directory.
	bool StartFrom(const std::string &cueDir, std::string &error);
	// Joins the player and releases the source. Idempotent.
	void Stop();

	// Queues the cue and returns; `volume` is 0 to 1, and 0 plays nothing. A newer cue
	// replaces one still waiting or playing: the user cares about the latest thing that
	// happened, and two cues at once are noise.
	void Play(Cue cue, double volume);

	// Plays arbitrary samples (a spoken confirmation) through the same monitor-only
	// source, so read-back reaches the user and not the stream. It waits for a cue that
	// is playing to finish (the accept cue comes first, then the words); a newer cue or
	// newer samples cut it short.
	void PlaySamples(WavData audio, double volume);

	size_t LoadedCues() const { return loaded_; }

private:
	void PlayerMain();

	OBSSourceAutoRelease source_; // holds the creation reference
	std::array<WavData, kCueCount> cues_;
	size_t loaded_ = 0;

	std::thread player_;
	std::mutex mutex_;
	std::condition_variable cv_;
	bool quit_ = false;
	int queued_ = -1; // the Cue waiting to play, or -1
	double volume_ = 0.0;
	// Samples waiting to play (PlaySamples), and the ones playing; the second is the
	// player thread's alone.
	bool speechWaiting_ = false;
	WavData speechQueued_;
	double speechVolume_ = 0.0;
	WavData speechPlaying_;
};

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_FEEDBACK_HPP_
