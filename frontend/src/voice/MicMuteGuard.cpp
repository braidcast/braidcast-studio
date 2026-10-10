#include "voice/MicMuteGuard.hpp"

#include "log.hpp"
#include "multistream/GlobalAudioChannels.hpp"

namespace Voice {

namespace {

// True on the thread that is inside the guard's own mute, so the "mute" handler, which
// libobs runs synchronously on that thread, does not take it for the user's.
thread_local bool t_guardMuting = false;

// What the handler last saw the user ask for during the hold.
constexpr int kNoWish = -1;

} // namespace

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
	userWish_.store(kNoWish, std::memory_order_release);
	// Listening starts BEFORE the mic's state is read: a user mute that lands after the
	// read is then signalled to the handler, and one that lands before it is in the read.
	// Connecting after the guard's own mute left a gap where a mute from another thread
	// was neither.
	signal_handler_connect(obs_source_get_signal_handler(mic), "mute", &MicMuteGuard::OnMute, this);
	listening_ = true;
	wasMuted_ = obs_source_muted(mic);
	if (!wasMuted_) {
		// Register the override before muting, so no save can land in between.
		GlobalAudio::SetPersistedMuteOverride(mic, false);
		t_guardMuting = true;
		obs_source_set_muted(mic, true);
		t_guardMuting = false;
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
						  this);
		}
		listening_ = false;
	}
	bool keptMuted = false;
	if (!wasMuted_) {
		// What the handler saw becomes the user's recorded wish (it may have landed before
		// the override was registered), then the hold ends: unmuted unless the user muted
		// the mic during it, atomically with any NoteUserMute.
		const int wish = userWish_.load(std::memory_order_acquire);
		if (wish != kNoWish && mic) {
			GlobalAudio::NoteUserMute(mic, wish != 0);
		}
		keptMuted = GlobalAudio::EndTemporaryMute(mic);
	}
	DBG(LogCat::Voice, "mic mute guard released%s%s", mic ? "" : " (the mic went away mid-hold)",
	    keptMuted ? " (left muted: the user muted it during the hold)" : "");
}

void MicMuteGuard::OnMute(void *data, calldata_t *params)
{
	// Every "mute" on the held mic but the guard's own (t_guardMuting): the guard listens
	// from before it reads the mic's state until before its unmute.
	if (t_guardMuting) {
		return;
	}
	const bool muted = calldata_bool(params, "muted");
	static_cast<MicMuteGuard *>(data)->userWish_.store(muted ? 1 : 0, std::memory_order_release);
	// And at once into the override, so a save mid-hold stores it (when the override is
	// registered by then; Release records it in any case).
	GlobalAudio::NoteHeldUserMute(muted);
}

} // namespace Voice
