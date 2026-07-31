#include "AudioMonitor.hpp"

#include <obs.hpp>

#include "bridge.hpp"

namespace {

// Sources with no audio output flag or no active audio are not mixer subjects.
bool IsActiveAudioSource(obs_source_t *source)
{
	if (!source) {
		return false;
	}
	const uint32_t flags = obs_source_get_output_flags(source);
	if ((flags & OBS_SOURCE_AUDIO) == 0) {
		return false;
	}
	return obs_source_audio_active(source);
}

std::string SourceUuid(obs_source_t *source)
{
	const char *uuid = source ? obs_source_get_uuid(source) : nullptr;
	return (uuid && *uuid) ? std::string(uuid) : std::string();
}

// The global output channels carrying desktop-audio + mic captures. These are
// not returned by obs_enum_sources, so the mixer enumerates them explicitly (the
// legacy AudioMixer does the same: channels 1..6).
constexpr int kFirstGlobalChannel = 1;
constexpr int kLastGlobalChannel = 6;

} // namespace

AudioMonitor::AudioMonitor() = default;

AudioMonitor::~AudioMonitor()
{
	ClearAll();
}

void AudioMonitor::AddEntry(obs_source_t *source)
{
	// A LOG fader/volmeter matches the legacy VolumeControl/VolumeMeter mapping.
	Entry entry;
	entry.uuid = SourceUuid(source);
	entry.fader = obs_fader_create(OBS_FADER_LOG);
	entry.volmeter = obs_volmeter_create(OBS_FADER_LOG);
	if (!entry.fader || !entry.volmeter) {
		if (entry.fader) {
			obs_fader_destroy(entry.fader);
		}
		if (entry.volmeter) {
			obs_volmeter_destroy(entry.volmeter);
		}
		return;
	}

	obs_fader_attach_source(entry.fader, source);
	obs_volmeter_attach_source(entry.volmeter, source);

	// Per-entry context so the param-only callback knows which source fired. Owned
	// by the entry; freed in DestroyEntry AFTER the callback is removed.
	entry.ctx = new CallbackCtx{this, entry.uuid};
	// Register the callback LAST, once the entry is fully built. obs copies the
	// Entry by value into the vector, but ctx is a heap pointer so the registered
	// param stays valid across the move.
	obs_volmeter_add_callback(entry.volmeter, AudioMonitor::OnVolmeterUpdated, entry.ctx);
	entries.push_back(entry);
}

void AudioMonitor::DestroyEntry(Entry &entry)
{
	// Remove the callback FIRST so a concurrent audio-thread fire cannot run
	// against a volmeter mid-destroy or a freed ctx, then detach + destroy.
	if (entry.volmeter) {
		obs_volmeter_remove_callback(entry.volmeter, AudioMonitor::OnVolmeterUpdated, entry.ctx);
		obs_volmeter_detach_source(entry.volmeter);
		obs_volmeter_destroy(entry.volmeter);
		entry.volmeter = nullptr;
	}
	if (entry.fader) {
		obs_fader_detach_source(entry.fader);
		obs_fader_destroy(entry.fader);
		entry.fader = nullptr;
	}
	// Safe to free only after remove_callback above guarantees no in-flight fire.
	delete entry.ctx;
	entry.ctx = nullptr;
}

void AudioMonitor::Rebuild()
{
	// Collect the current active audio sources: every enumerated input plus the
	// global output channels (desktop audio / mics). Keep refs so a source cannot
	// vanish between collection and (re)building entries.
	struct Collect {
		std::vector<OBSSourceAutoRelease> sources;
	} collect;

	obs_enum_sources(
		[](void *param, obs_source_t *source) -> bool {
			if (IsActiveAudioSource(source)) {
				static_cast<Collect *>(param)->sources.emplace_back(obs_source_get_ref(source));
			}
			return true;
		},
		&collect);

	for (int ch = kFirstGlobalChannel; ch <= kLastGlobalChannel; ++ch) {
		OBSSourceAutoRelease global = obs_get_output_source(ch); // addref'd
		if (IsActiveAudioSource(global)) {
			collect.sources.emplace_back(std::move(global));
		}
	}

	std::lock_guard<std::mutex> lock(entriesMutex);

	// Drop entries whose source is no longer active (or gone).
	for (auto it = entries.begin(); it != entries.end();) {
		bool keep = false;
		for (const OBSSourceAutoRelease &src : collect.sources) {
			if (SourceUuid(src) == it->uuid) {
				keep = true;
				break;
			}
		}
		if (keep) {
			++it;
		} else {
			DestroyEntry(*it);
			it = entries.erase(it);
		}
	}

	// Add entries for newly active sources.
	for (const OBSSourceAutoRelease &src : collect.sources) {
		const std::string uuid = SourceUuid(src);
		if (uuid.empty()) {
			continue;
		}
		bool exists = false;
		for (const Entry &e : entries) {
			if (e.uuid == uuid) {
				exists = true;
				break;
			}
		}
		if (!exists) {
			AddEntry(src);
		}
	}
}

std::vector<AudioMonitor::SourceInfo> AudioMonitor::List() const
{
	std::vector<SourceInfo> out;
	std::lock_guard<std::mutex> lock(entriesMutex);
	out.reserve(entries.size());
	for (const Entry &e : entries) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(e.uuid.c_str()); // addref'd
		if (!source) {
			continue;
		}
		SourceInfo info;
		info.uuid = e.uuid;
		const char *name = obs_source_get_name(source);
		info.name = name ? name : std::string();
		info.deflection = e.fader ? obs_fader_get_deflection(e.fader) : 0.0f;
		info.volumeDb = e.fader ? obs_fader_get_db(e.fader) : 0.0f;
		info.muted = obs_source_muted(source);
		out.push_back(std::move(info));
	}
	return out;
}

bool AudioMonitor::SetDeflection(const std::string &uuid, float deflection, float &outDeflection, float &outDb)
{
	std::lock_guard<std::mutex> lock(entriesMutex);
	for (Entry &e : entries) {
		if (e.uuid == uuid && e.fader) {
			obs_fader_set_deflection(e.fader, deflection);
			outDeflection = obs_fader_get_deflection(e.fader);
			outDb = obs_fader_get_db(e.fader);
			return true;
		}
	}
	return false;
}

std::map<std::string, AudioMonitor::Levels> AudioMonitor::SnapshotLevels()
{
	std::map<std::string, Levels> snapshot;
	{
		std::lock_guard<std::mutex> lock(levelsMutex);
		snapshot = levels;
	}
	// Re-arm: the next audio-thread callback schedules a fresh emit.
	emitPending.store(false);
	return snapshot;
}

void AudioMonitor::CoalesceLevel(const std::string &uuid, float magnitudeDb, float peakDb)
{
	if (uuid.empty()) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(levelsMutex);
		Levels &l = levels[uuid];
		l.magnitude = magnitudeDb;
		l.peak = peakDb;
	}
	// Arm exactly one emit between drains: the first callback after a drain flips
	// the flag and schedules the marshaled emit; later callbacks only coalesce.
	if (!emitPending.exchange(true)) {
		Bridge::EmitAudioLevels();
	}
}

void AudioMonitor::OnVolmeterUpdated(void *param, const float magnitude[MAX_AUDIO_CHANNELS],
				     const float peak[MAX_AUDIO_CHANNELS],
				     const float /*input_peak*/[MAX_AUDIO_CHANNELS])
{
	auto *ctx = static_cast<CallbackCtx *>(param);
	if (!ctx || !ctx->monitor) {
		return;
	}

	// Reduce per-channel arrays to a single magnitude/peak: the max across active
	// channels (a silent channel reports -inf, so max ignores it). The volmeter
	// already maps to dB in [-inf, 0].
	float maxMag = -96.0f;
	float maxPeak = -96.0f;
	for (int ch = 0; ch < MAX_AUDIO_CHANNELS; ++ch) {
		if (magnitude[ch] > maxMag) {
			maxMag = magnitude[ch];
		}
		if (peak[ch] > maxPeak) {
			maxPeak = peak[ch];
		}
	}

	ctx->monitor->CoalesceLevel(ctx->uuid, maxMag, maxPeak);
}

void AudioMonitor::ClearAll()
{
	std::lock_guard<std::mutex> lock(entriesMutex);
	for (Entry &e : entries) {
		DestroyEntry(e);
	}
	entries.clear();
	{
		std::lock_guard<std::mutex> levelsLock(levelsMutex);
		levels.clear();
	}
	emitPending.store(false);
}
