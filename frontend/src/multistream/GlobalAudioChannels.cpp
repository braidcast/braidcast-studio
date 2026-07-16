#include "GlobalAudioChannels.hpp"

#include "StorePaths.hpp"

#include "../log.hpp"

#include <obs.h>
#include <obs.hpp>

#include <nlohmann/json.hpp>

#include <string>

using json = nlohmann::json;

const std::array<GlobalAudioChannels::Slot, 6> &GlobalAudioChannels::Slots()
{
	static const std::array<Slot, 6> kSlots = {{
		{1, false, "wasapi_output_capture", "Desktop Audio", "desktop"},
		{2, false, "wasapi_output_capture", "Desktop Audio 2", "desktop"},
		{3, true, "wasapi_input_capture", "Mic/Aux", "mic"},
		{4, true, "wasapi_input_capture", "Mic/Aux 2", "mic"},
		{5, true, "wasapi_input_capture", "Mic/Aux 3", "mic"},
		{6, true, "wasapi_input_capture", "Mic/Aux 4", "mic"},
	}};
	return kSlots;
}

const GlobalAudioChannels::Slot *GlobalAudioChannels::SlotForChannel(int channel)
{
	for (const Slot &slot : Slots()) {
		if (slot.channel == channel) {
			return &slot;
		}
	}
	return nullptr;
}

bool GlobalAudioChannels::ApplyDevice(int channel, const std::string &deviceId, std::string &error)
{
	const Slot *slot = SlotForChannel(channel);
	if (!slot) {
		error = "channel " + std::to_string(channel) + " is not a global audio slot";
		return false;
	}

	if (deviceId.empty()) {
		// DISABLE: drop the channel's ref so the source is destroyed.
		obs_set_output_source(channel, nullptr);
		return true;
	}

	// If the channel already carries this slot's source type, just update its
	// device_id in place rather than recreating it.
	OBSSourceAutoRelease cur = obs_get_output_source(channel); // addref'd; may be null
	if (cur) {
		const char *curId = obs_source_get_id(cur);
		if (curId && std::string(curId) == slot->sourceId) {
			OBSDataAutoRelease s = obs_source_get_settings(cur);
			obs_data_set_string(s, "device_id", deviceId.c_str());
			obs_source_update(cur, s);
			return true;
		}
	}

	// Otherwise create a fresh source and bind it (the channel takes its own ref).
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "device_id", deviceId.c_str());
	obs_source_t *source = obs_source_create(slot->sourceId, slot->label, settings, nullptr); // create-ref
	if (!source) {
		error = std::string("obs_source_create failed for ") + slot->sourceId;
		return false;
	}
	obs_set_output_source(channel, source); // channel takes its own ref
	obs_source_release(source);             // drop the create-ref
	return true;
}

std::optional<std::string> GlobalAudioChannels::CurrentDevice(int channel) const
{
	OBSSourceAutoRelease cur = obs_get_output_source(channel); // addref'd; may be null
	if (!cur) {
		return std::nullopt;
	}
	OBSDataAutoRelease settings = obs_source_get_settings(cur);
	const char *id = settings ? obs_data_get_string(settings, "device_id") : nullptr;
	return std::string(id ? id : "");
}

bool GlobalAudioChannels::Persist() const
{
	json obj = json::object();
	for (const Slot &slot : Slots()) {
		OBSSourceAutoRelease cur = obs_get_output_source(slot.channel); // addref'd; may be null
		if (!cur) {
			continue;
		}
		// Save the FULL source blob (settings + volume/mute/monitoring/sync + the
		// "filters" array), not just device_id -- these channel sources are excluded
		// from the scene-collection save (SaveFilter), so this file is the only place
		// their filters and mixer state can persist. A blob that won't round-trip
		// through JSON degrades to the legacy device_id string.
		OBSDataAutoRelease saved = obs_save_source(cur); // full obs_save_source blob
		const char *savedJson = saved ? obs_data_get_json(saved) : nullptr;
		bool stored = false;
		if (savedJson && *savedJson) {
			try {
				obj[std::to_string(slot.channel)] = json::parse(savedJson);
				stored = true;
			} catch (...) {
				stored = false;
			}
		}
		if (!stored) {
			OBSDataAutoRelease settings = obs_source_get_settings(cur);
			const char *deviceId = settings ? obs_data_get_string(settings, "device_id") : nullptr;
			obj[std::to_string(slot.channel)] = deviceId ? std::string(deviceId) : std::string();
		}
	}

	// Store the map as a stringified-JSON blob under "state", the same key/value
	// envelope theme.json/layout.json use. Each channel value is either a full source
	// blob (object) or, for legacy/degraded records, a device_id (string) -- restore
	// dispatches on the type.
	OBSDataAutoRelease root = obs_data_create();
	obs_data_set_string(root, "state", obj.dump().c_str());
	const std::string path = MultistreamBasicPath("audio_devices.json");
	return ReportSaveResult(SaveJsonAtomic(root, path), path);
}

void GlobalAudioChannels::SeedOrRestore()
{
	// Read the saved per-channel map from audio_devices.json (key "state"), matching
	// the legacy ReadJsonString envelope.
	std::string state;
	{
		OBSDataAutoRelease root =
			obs_data_create_from_json_file_safe(MultistreamBasicPath("audio_devices.json").c_str(), "bak");
		if (root) {
			const char *v = obs_data_get_string(root, "state");
			state = v ? v : "";
		}
	}

	auto firstRunSeed = [this]() {
		std::string err;
		ApplyDevice(1, "default", err);
		ApplyDevice(3, "default", err);
		Persist();
		HostLog("[audio] global audio: first-run seed (Desktop Audio + Mic/Aux -> default)");
	};

	if (state.empty()) {
		firstRunSeed();
		return;
	}

	json parsed;
	try {
		parsed = json::parse(state);
	} catch (...) {
		HostLog("[audio] global audio: state parse failed; falling back to first-run seed");
		firstRunSeed();
		return;
	}
	if (!parsed.is_object()) {
		firstRunSeed();
		return;
	}

	int restored = 0;
	for (auto it = parsed.begin(); it != parsed.end(); ++it) {
		int channel = 0;
		try {
			channel = std::stoi(it.key());
		} catch (...) {
			continue;
		}
		if (!SlotForChannel(channel)) {
			continue;
		}
		if (it.value().is_object()) {
			// Full source blob: recreate the source with its filters + mixer state and
			// bind it to the channel. obs_load_source restores the "filters" array.
			const std::string blob = it.value().dump();
			OBSDataAutoRelease data = obs_data_create_from_json(blob.c_str());
			obs_source_t *src = data ? obs_load_source(data) : nullptr; // create-ref
			if (src) {
				obs_set_output_source(channel, src); // channel takes its own ref
				obs_source_release(src);             // drop the create-ref
				++restored;
			} else {
				HostLog("[audio] global audio: restore ch" + std::to_string(channel) +
					" blob load failed");
			}
		} else if (it.value().is_string()) {
			// Legacy device_id map (pre-blob installs): recreate a bare source.
			std::string err;
			if (ApplyDevice(channel, it.value().get<std::string>(), err)) {
				++restored;
			} else {
				HostLog("[audio] global audio: restore ch" + std::to_string(channel) +
					" failed: " + err);
			}
		}
	}
	HostLog("[audio] global audio: restored " + std::to_string(restored) + " channel(s) from audio_devices.json");
}

void GlobalAudioChannels::Clear()
{
	// Drop each global channel's ref so the wasapi sources die before obs_shutdown.
	for (const Slot &slot : Slots()) {
		obs_set_output_source(slot.channel, nullptr);
	}
}
