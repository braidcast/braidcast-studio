#ifndef OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_VOICE_SELFTEST_HPP_
#define OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_VOICE_SELFTEST_HPP_

#include <obs.h>

// Shared by the self-tests that drive one voice per overlay (OverlayVoice): the smoke battery's
// overlay-voice steps and the overlay-voice stream mode.
namespace OverlayVoiceSelfTest {

// Which of two copies the audio thread lets be heard: "A", "B", "both" or "none".
inline const char *HeardOf(obs_source_t *a, obs_source_t *b)
{
	const bool aOpen = a && !obs_source_audio_gated(a);
	const bool bOpen = b && !obs_source_audio_gated(b);
	return aOpen && bOpen ? "both" : aOpen ? "A" : bOpen ? "B" : "none";
}

} // namespace OverlayVoiceSelfTest

#endif // OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_VOICE_SELFTEST_HPP_
