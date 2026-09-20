#ifndef OBS_MULTISTREAM_FRONTEND_UTIL_SPEAKER_LAYOUT_HPP_
#define OBS_MULTISTREAM_FRONTEND_UTIL_SPEAKER_LAYOUT_HPP_

#include <string>

#include <obs.h>

// The wire and on-disk names for libobs' speaker layouts. Shared because three places
// need the same mapping -- the settings bridge, the persisted audio settings and the
// boot that applies them -- and a second copy would be free to drift from the first.
namespace Audio {

// The name for `layout`, or "stereo" for one this build does not name. Never null.
const char *SpeakerLayoutName(speaker_layout layout);

// The layout `name` stands for. False leaves `out` untouched, which is how a caller
// tells an unknown name from a valid one rather than silently getting stereo.
bool SpeakerLayoutFromName(const std::string &name, speaker_layout &out);

// The two rates libobs' mix supports. A persisted or imported value outside them is not
// clamped -- it is refused, and the caller falls back to its own default.
constexpr uint32_t kSampleRate44100 = 44100;
constexpr uint32_t kSampleRate48000 = 48000;
inline bool SampleRateSupported(uint32_t hz)
{
	return hz == kSampleRate44100 || hz == kSampleRate48000;
}

} // namespace Audio

#endif // OBS_MULTISTREAM_FRONTEND_UTIL_SPEAKER_LAYOUT_HPP_
