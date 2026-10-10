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
// Engage, and the user did not mute it during the hold: if the user changed their own
// mic's mute during the hold, their choice wins. An unmute shows in libobs; a mute does
// not (the guard has already muted the mic, so muting it again changes nothing), so a
// user mute is recorded where it is asked for: the bridge's mute seam records it with
// the mute (GlobalAudio::SetUserMuted), and for every other path (libobs's per-source
// mute hotkey included) the guard hears the mic's "mute" signal, which it listens to for
// the whole hold, and records it on release (GlobalAudio::NoteUserMute). While engaged
// the persisted mute state is overridden (see GlobalAudio::SetPersistedMuteOverride) so
// a save mid-hold stores what the user wants, never the guard's own mute.
// The source is held weakly, so a mic removed mid-hold simply ends the guard.
//
// libobs reports the guard's own mute to every audio capture callback, VoiceCapture's
// included, exactly as it reports the user's. Engage says whether the guard holds a mute
// of its own, so the engine can tell the two apart.
class MicMuteGuard {
public:
	MicMuteGuard() = default;
	~MicMuteGuard();
	MicMuteGuard(const MicMuteGuard &) = delete;
	MicMuteGuard &operator=(const MicMuteGuard &) = delete;

	// True when, after the call, the mic is muted by the guard rather than by the user:
	// false for no mic, and for a mic that was muted already (the guard then leaves it).
	bool Engage(obs_source_t *mic);
	void Release();
	bool Engaged() const { return engaged_.load(std::memory_order_acquire); }

private:
	// The mic's "mute" signal while the guard holds it: any thread.
	static void OnMute(void *data, calldata_t *params);

	OBSWeakSource weak_;
	std::atomic<bool> engaged_{false};
	bool wasMuted_ = false;
	bool listening_ = false; // connected to the held mic's "mute" signal
	// The last mute (1) or unmute (0) the user made during the hold, as the signal
	// handler saw it, or -1 for none. Written on whichever thread muted the mic.
	std::atomic<int> userWish_{-1};
};

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_MIC_MUTE_GUARD_HPP_
