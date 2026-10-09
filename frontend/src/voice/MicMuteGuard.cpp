#include "voice/MicMuteGuard.hpp"

#include "log.hpp"
#include "multistream/GlobalAudioChannels.hpp"

namespace Voice {

MicMuteGuard::~MicMuteGuard()
{
	Release();
}

void MicMuteGuard::Engage(obs_source_t *mic)
{
	if (Engaged() || !mic) {
		return;
	}
	weak_ = OBSGetWeakRef(mic);
	wasMuted_ = obs_source_muted(mic);
	if (!wasMuted_) {
		// Register the override before muting, so no save can land in between.
		GlobalAudio::SetPersistedMuteOverride(mic, false);
		obs_source_set_muted(mic, true);
	}
	engaged_.store(true, std::memory_order_release);
	DBG(LogCat::Voice, "mic mute guard engaged (was %s)", wasMuted_ ? "muted" : "unmuted");
}

void MicMuteGuard::Release()
{
	if (!Engaged()) {
		return;
	}
	engaged_.store(false, std::memory_order_release);
	OBSSourceAutoRelease mic = obs_weak_source_get_source(weak_);
	weak_ = nullptr;
	if (mic && !wasMuted_ && obs_source_muted(mic)) {
		obs_source_set_muted(mic, false);
	}
	// Cleared after the unmute, so a save in between still stores "unmuted".
	if (!wasMuted_) {
		GlobalAudio::ClearPersistedMuteOverride();
	}
	DBG(LogCat::Voice, "mic mute guard released%s", mic ? "" : " (the mic went away mid-hold)");
}

} // namespace Voice
