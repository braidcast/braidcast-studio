#ifndef OBS_MULTISTREAM_FRONTEND_AUDIO_MIXER_FLAGS_HPP_
#define OBS_MULTISTREAM_FRONTEND_AUDIO_MIXER_FLAGS_HPP_

#include <obs.h>
#include <obs.hpp>

#include <cstdint>

// The per-source flags the audio mixer reads and writes. Shared by the bridge, which reads
// and writes them, and the overlay voice groups, which keep them identical across the
// copies of one overlay.
namespace MixerFlags {

// audio.set{Hidden,VolumeLocked,Pinned}: booleans in the source's private settings, so they
// persist with the scene collection.
inline constexpr char kHidden[] = "mixer_hidden";
inline constexpr char kVolumeLocked[] = "volume_locked";
inline constexpr char kPinned[] = "mixer_pinned";

// One of the keys above; false when absent.
inline bool Get(obs_source_t *source, const char *key)
{
	OBSDataAutoRelease priv = obs_source_get_private_settings(source);
	return priv ? obs_data_get_bool(priv, key) : false;
}

inline void Set(obs_source_t *source, const char *key, bool value)
{
	OBSDataAutoRelease priv = obs_source_get_private_settings(source);
	if (priv) {
		obs_data_set_bool(priv, key, value);
	}
}

// The Advanced Audio dialog's downmix-to-mono, which libobs keeps among the source's flags.
inline bool ForceMono(obs_source_t *source)
{
	return (obs_source_get_flags(source) & OBS_SOURCE_FLAG_FORCE_MONO) != 0;
}

inline void SetForceMono(obs_source_t *source, bool mono)
{
	const uint32_t flags = obs_source_get_flags(source);
	obs_source_set_flags(source,
			     mono ? flags | OBS_SOURCE_FLAG_FORCE_MONO : flags & ~uint32_t(OBS_SOURCE_FLAG_FORCE_MONO));
}

} // namespace MixerFlags

#endif // OBS_MULTISTREAM_FRONTEND_AUDIO_MIXER_FLAGS_HPP_
