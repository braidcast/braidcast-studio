#pragma once

#include <array>
#include <optional>
#include <string>

// Owns the global desktop/mic audio channels -- the wasapi capture sources bound
// to OBS output channels 1..6 that stock OBS seeds on first run (Desktop Audio /
// Mic-Aux). The new frontend never seeded them, leaving the mixer empty; this
// subsystem seeds them on first run, restores the saved per-channel device map on
// subsequent runs (audio_devices.json), and unbinds them before obs_shutdown.
//
// Holds no instance state: every method operates on the live OBS global output
// channels + the on-disk map, so the bootstrap owns a single instance purely for a
// clear lifecycle owner. The per-SOURCE fader/volmeter mixer is a separate
// subsystem (AudioMonitor); this is only the global output-channel slots.
class GlobalAudioChannels {
public:
	GlobalAudioChannels() = default;
	~GlobalAudioChannels() = default;

	GlobalAudioChannels(const GlobalAudioChannels &) = delete;
	GlobalAudioChannels &operator=(const GlobalAudioChannels &) = delete;

	// One global output channel: a wasapi capture source bound to channel 1..6 that
	// shows in the mixer (AudioMonitor enumerates channels 1..6).
	struct Slot {
		int channel;          // obs output channel 1..6
		bool input;           // true = mic/aux (wasapi_input_capture), false = desktop (wasapi_output_capture)
		const char *sourceId; // "wasapi_input_capture" | "wasapi_output_capture"
		const char *label;    // "Desktop Audio", "Mic/Aux", ...
		const char *role;     // "desktop" | "mic"
	};

	// The OBS output-channel span these global audio sources occupy. The scene-save
	// filter (SceneCollection::SaveFilter in scene_persistence) excludes exactly this
	// range because the device map persists separately via audio_devices.json, NOT in
	// the scene collection -- keep both in lockstep if the range ever changes.
	static constexpr int kFirstChannel = 1;
	static constexpr int kLastChannel = 6;
	// Whether `channel` is a global audio channel (and thus excluded from scene save).
	static bool IsGlobalChannel(int channel) { return channel >= kFirstChannel && channel <= kLastChannel; }

	// The per-channel slot definitions (Desktop Audio on 1..2, Mic/Aux on 3..6).
	static const std::array<Slot, 6> &Slots();
	// The slot for `channel`, or nullptr if `channel` is not a global audio slot.
	static const Slot *SlotForChannel(int channel);

	// First run: seed Desktop Audio (ch1) + Mic/Aux (ch3) to the OS default device and
	// persist. Subsequent runs: restore the saved per-channel map from
	// audio_devices.json. Never throws. Call after modules load + the default scene is
	// bound, before the AudioMonitor's initial Rebuild so it sees the seeded devices.
	void SeedOrRestore();

	// Bind/update/disable the device on one global channel. An empty deviceId disables
	// (drops the channel's source so it is destroyed). Does NOT persist or rebuild the
	// monitor -- callers do, so seeding can batch. Fills `error` and returns false on
	// failure (unknown channel or obs_source_create failure).
	bool ApplyDevice(int channel, const std::string &deviceId, std::string &error);

	// The device_id currently bound on `channel`, or nullopt if the channel has no
	// source. A bound source with no device_id yields an empty string (not nullopt),
	// so callers can distinguish "unbound" from "bound to the default device".
	std::optional<std::string> CurrentDevice(int channel) const;

	// Persist the current per-channel device map ({"<channel>":"<deviceId>", ...}) for
	// every channel with a bound source to audio_devices.json, as a stringified-JSON
	// blob under "state" (the same envelope theme.json/layout.json use). Returns false
	// if the write failed (already logged); callers may ignore it (batched seeding).
	bool Persist() const;

	// Unbind every global audio channel so the wasapi sources are destroyed before
	// obs_shutdown. Call during teardown while libobs is still up.
	void Clear();
};
