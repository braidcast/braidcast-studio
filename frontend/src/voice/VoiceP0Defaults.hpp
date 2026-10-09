#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_P0_DEFAULTS_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_P0_DEFAULTS_HPP_

// The voice control defaults that were measured rather than chosen. Each value comes
// from braidcast-notes/experiments/voice-p0/RESULTS.md (decision rules D1-D3 and the
// loopback probe). Change a value only by rerunning that probe.
//
// Until the P0 probes have run, every value here is the spec's default
// (specs/2026-09-18-voice-control-design.md), not a measurement. P1 Task 15 was to
// replace them with the measured numbers; it shipped without them, so they are still
// pending P0.
namespace Voice::P0 {

// D1: the default speech model id (a VoiceModels catalog id).
inline constexpr const char *kDefaultModelId = "base.en-q5_1";
// D2: the most whisper threads voice uses; the worker takes min(this, cores / 2).
inline constexpr int kThreadCap = 4;
// D3: whether the Voice tab offers small.en as "higher accuracy".
inline constexpr bool kOfferSmallModel = false;
// Loopback probe case A: a monitor-only source is audible on the stream through
// Desktop Audio when the monitoring device and the Desktop Audio device are the same.
inline constexpr bool kCueLeakOnSharedDevice = true;

} // namespace Voice::P0

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_P0_DEFAULTS_HPP_
