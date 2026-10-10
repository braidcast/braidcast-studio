#include "voice/MicMuteGuard.hpp"

#include "log.hpp"
#include "multistream/GlobalAudioChannels.hpp"

namespace Voice {

MicMuteGuard::~MicMuteGuard()
{
	Release();
}

bool MicMuteGuard::Engage(obs_source_t *mic)
{
	if (Engaged()) {
		return !wasMuted_;
	}
	if (!mic) {
		return false;
	}
	weak_ = OBSGetWeakRef(mic);
	wasMuted_ = obs_source_muted(mic);
	if (!wasMuted_) {
		// Register the override before muting, so no save can land in between.
		GlobalAudio::SetPersistedMuteOverride(mic, false);
		obs_source_set_muted(mic, true);
		// After our own mute, so it is not taken for the user's. A user mute between the
		// two is already covered on the bridge path (NoteUserMute before the mute).
		signal_handler_connect(obs_source_get_signal_handler(mic), "mute", &MicMuteGuard::OnMute, nullptr);
		listening_ = true;
	}
	engaged_.store(true, std::memory_order_release);
	DBG(LogCat::Voice, "mic mute guard engaged (was %s)", wasMuted_ ? "muted" : "unmuted");
	return !wasMuted_;
}

void MicMuteGuard::Release()
{
	if (!Engaged()) {
		return;
	}
	engaged_.store(false, std::memory_order_release);
	OBSSourceAutoRelease mic = obs_weak_source_get_source(weak_);
	weak_ = nullptr;
	if (listening_) {
		// First, so the guard's own unmute below is not taken for the user's, and so the
		// handler never runs while EndTemporaryMute decides. Waits out an emission in
		// progress, so a user mute signalled on another thread is noted before that
		// decision. A mic that went away took its signal handler with it.
		if (mic) {
			signal_handler_disconnect(obs_source_get_signal_handler(mic), "mute", &MicMuteGuard::OnMute,
						  nullptr);
		}
		listening_ = false;
	}
	bool keptMuted = false;
	if (!wasMuted_) {
		// Unmutes unless the user muted the mic during the hold, and clears the override,
		// atomically with any NoteUserMute.
		keptMuted = GlobalAudio::EndTemporaryMute(mic);
	}
	DBG(LogCat::Voice, "mic mute guard released%s%s", mic ? "" : " (the mic went away mid-hold)",
	    keptMuted ? " (left muted: the user muted it during the hold)" : "");
}

void MicMuteGuard::OnMute(void *, calldata_t *params)
{
	// Every "mute" on the held mic that is not the guard's own: the guard connects after
	// its mute, to the held mic only, and disconnects before its unmute.
	GlobalAudio::NoteHeldUserMute(calldata_bool(params, "muted"));
}

} // namespace Voice
