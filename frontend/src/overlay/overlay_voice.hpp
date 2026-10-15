#ifndef OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_VOICE_HPP_
#define OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_VOICE_HPP_

#include <string>
#include <vector>

struct obs_source;
typedef struct obs_source obs_source_t;

// One voice per overlay. Copies of one overlay -- braidcast_overlay sources bound to the
// same overlay_id, as a scene duplicate makes them -- form a libobs audio group
// (obs_source_set_audio_group), in which the audio thread lets exactly one copy be heard.
// This module publishes each copy's group, keeps the copies' audio settings identical
// (one mixer strip stands for all of them), and answers which copy that strip is.
//
// Threads. libobs signals reach this module on the creating, graphics, destroy-task and
// hotkey threads; there it only publishes the group (lock-free atomics) and queues work
// with AsyncTask::QueueOnUi, never looking a source up. Adoption and fan-out run on the
// CEF UI thread, holding no source pointer past one pass: queued work names sources by
// uuid. The per-source callbacks are static and check a module-wide alive flag, so Stop
// needs no per-source disconnect.
//
// Persistence. The gate is libobs's and never saved. Adoption and fan-out change saved
// audio settings on the copies, and save them, by design: they are the settings the copy
// now has.
namespace OverlayVoice {

// The audio settings kept identical across a group: what the Advanced Audio Properties
// dialog edits, mute, push-to-mute and push-to-talk (every field that decides the source's
// volume or whether it is muted), plus the strip's own mixer flags. One table in
// overlay_voice.cpp drives adoption, fan-out and comparison over exactly these.
enum class Field {
	Volume,
	Muted,
	Balance,
	SyncOffset,
	Mixers,
	Monitoring,
	ForceMono,
	PushToMute,
	PushToMuteDelay,
	PushToTalk,
	PushToTalkDelay,
	MixerHidden,
	VolumeLocked,
	MixerPinned,
	Count,
};

// Connect to libobs's global source signals. After LoadCuratedModules (which registers
// braidcast_overlay) and before the scene collection loads. UI thread.
void Start();

// Clear the alive flag, disconnect the global signals and forget every group. After
// Bridge::Shutdown, while libobs is still up. UI thread.
void Stop();

// Copy `field` from `source` onto every other copy in its group whose value differs, right
// after the caller wrote it. Returns whether any copy changed; the caller persists, as it
// does its own change. A source in no group, or one not yet adopted into its group, changes
// nothing; nor does a call made while another copy's later change to the field is still
// queued, which then spreads instead. UI thread.
bool FanOut(obs_source_t *source, Field field);

// The uuid of `uuid`'s group representative (the copy with the lowest rank), or `uuid`
// itself when it is in no group. The mixer strip is not always this copy: the bridge gives
// it to the first copy by rank that the mixer lists (MixerStripOf in bridge.cpp). UI thread.
std::string RepresentativeOf(const std::string &uuid);

struct Group {
	std::string overlayId;
	std::vector<std::string> members; // uuids by rank; the first is the representative
};

// The group `uuid` represents, or nullptr when it represents none. UI thread; valid until
// the next UI task.
const Group *GroupRepresentedBy(const std::string &uuid);

} // namespace OverlayVoice

#endif // OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_VOICE_HPP_
