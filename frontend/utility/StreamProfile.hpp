#pragma once

#include <obs.hpp>
#include <string>

/* A persisted stream *profile* — one reusable credential (service + server +
 * key/account), global and canvas-agnostic. Mirrors CanvasDefinition. The
 * service settings are stored as a raw obs_data JSON blob (exactly what
 * obs_service_create consumes) and only rehydrated into a real obs_service on
 * demand. Exactly one profile is isPrimary == true (mirrors the Default canvas
 * invariant); the primary drives OBS's single global service until Phase 2e. */
struct StreamProfile {
	std::string uuid;
	std::string label;                     // user-entered, e.g. "AnimeCruizer"
	std::string serviceId = "rtmp_common"; // "rtmp_common" | "rtmp_custom" | "whip_custom"
	OBSDataAutoRelease settings;           // obs_service settings blob (may be null -> empty)
	bool isPrimary = false;

	/* Platform shown as the display prefix. For rtmp_common it is the
	 * "service" key (e.g. "YouTube - RTMPS" -> "YouTube"); for custom/WHIP a
	 * generic label. Never empty. */
	[[nodiscard]] std::string PlatformName() const;
	/* "{platform} - {label}", or just platform when label is empty. */
	[[nodiscard]] std::string DisplayName() const;

	[[nodiscard]] OBSDataAutoRelease ToData() const;
	static StreamProfile FromData(obs_data_t *data);
};
