#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_MIC_MUTE_GUARD_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_MIC_MUTE_GUARD_HPP_

#include <obs.hpp>

#include <atomic>

namespace Voice {

// Mutes the microphone for the length of a spoken command so viewers do not hear it,
// then puts it back. Engage and Release are called from the libobs hotkey thread, and
// once more from the UI thread in the engine's Stop, after the hotkeys are unregistered;
// they are never called concurrently. Engaged() may be read from any thread.
//
// Release unmutes only when the source is *still* muted and it was unmuted before
// Engage: if the user changed their own mic's mute during the hold, their choice
// wins. While engaged the persisted mute state is overridden (see
// GlobalAudio::SetPersistedMuteOverride) so a save mid-hold cannot store "muted".
// The source is held weakly, so a mic removed mid-hold simply ends the guard.
class MicMuteGuard {
public:
	MicMuteGuard() = default;
	~MicMuteGuard();
	MicMuteGuard(const MicMuteGuard &) = delete;
	MicMuteGuard &operator=(const MicMuteGuard &) = delete;

	void Engage(obs_source_t *mic);
	void Release();
	bool Engaged() const { return engaged_.load(std::memory_order_acquire); }

private:
	OBSWeakSource weak_;
	std::atomic<bool> engaged_{false};
	bool wasMuted_ = false;
};

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_MIC_MUTE_GUARD_HPP_
