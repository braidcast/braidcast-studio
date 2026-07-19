#ifndef OBS_MULTISTREAM_FRONTEND_ADVANCED_SETTINGS_HPP_
#define OBS_MULTISTREAM_FRONTEND_ADVANCED_SETTINGS_HPP_

#include <cstdint>
#include <string>

// Global "Advanced settings" bag, persisted to advanced.json in the shared
// braidcast config dir. Mirrors GeneralSettings exactly: a plain struct
// whose fields are the single source of truth, round-tripped through descriptor
// tables shared by the persistence layer and the bridge so the two can't drift.
// Some fields drive behavior now (process priority at startup, per-output stream
// delay / reconnect / network options applied in MultistreamEngine::StartOutput);
// browserHwAccel is store-only (obs-browser reads its own config).
struct AdvancedSettings {
	// --- process (Windows) ---
	std::string processPriority = "auto"; // auto | normal | aboveNormal | high
	bool disableAudioDucking = false;
	// --- stream delay (per output) ---
	bool streamDelayEnabled = false;
	uint32_t streamDelaySec = 20;
	bool streamDelayPreserve = true;
	// --- automatic reconnect (per output) ---
	bool reconnectEnabled = true;
	uint32_t reconnectRetryDelaySec = 10;
	uint32_t reconnectMaxRetries = 25;
	// --- network (per output) ---
	std::string bindIP = "default"; // "default" (don't bind) or a literal IP
	bool newSocketLoop = false;
	bool lowLatencyMode = false;
	// --- dynamic bitrate (per output) ---
	bool dynamicBitrate = false;
	// --- browser source HW accel (store-only; obs-browser reads its own config) ---
	bool browserHwAccel = true;

	// Round-trip every field to advanced.json (file keys snake_case). Missing keys
	// fall back to the struct defaults. Save() is called on each bridge set.
	void Load();
	bool Save() const;
};

// Field descriptors: the SINGLE source for the wire (camelCase) <-> file
// (snake_case) <-> struct-member mapping. Both the persistence layer
// (AdvancedSettings::Load/Save) and the bridge (settings.getAdvanced/setAdvanced)
// iterate these, so the two layers cannot drift.
struct AdvancedBoolField {
	const char *json;
	const char *file;
	bool AdvancedSettings::*member;
};
struct AdvancedStringField {
	const char *json;
	const char *file;
	std::string AdvancedSettings::*member;
};
struct AdvancedUIntField {
	const char *json;
	const char *file;
	uint32_t AdvancedSettings::*member;
	uint32_t min;
	uint32_t max;
};

inline constexpr AdvancedBoolField kAdvancedBoolFields[] = {
	{"streamDelayEnabled", "stream_delay_enabled", &AdvancedSettings::streamDelayEnabled},
	{"streamDelayPreserve", "stream_delay_preserve", &AdvancedSettings::streamDelayPreserve},
	{"reconnectEnabled", "reconnect_enabled", &AdvancedSettings::reconnectEnabled},
	{"newSocketLoop", "new_socket_loop", &AdvancedSettings::newSocketLoop},
	{"lowLatencyMode", "low_latency_mode", &AdvancedSettings::lowLatencyMode},
	{"dynamicBitrate", "dynamic_bitrate", &AdvancedSettings::dynamicBitrate},
	{"browserHwAccel", "browser_hw_accel", &AdvancedSettings::browserHwAccel},
	{"disableAudioDucking", "disable_audio_ducking", &AdvancedSettings::disableAudioDucking},
};
inline constexpr AdvancedStringField kAdvancedStringFields[] = {
	{"processPriority", "process_priority", &AdvancedSettings::processPriority},
	{"bindIP", "bind_ip", &AdvancedSettings::bindIP},
};
inline constexpr AdvancedUIntField kAdvancedUIntFields[] = {
	{"streamDelaySec", "stream_delay_sec", &AdvancedSettings::streamDelaySec, 0, 7200},
	{"reconnectRetryDelaySec", "reconnect_retry_delay_sec", &AdvancedSettings::reconnectRetryDelaySec, 0, 3600},
	{"reconnectMaxRetries", "reconnect_max_retries", &AdvancedSettings::reconnectMaxRetries, 0, 10000},
};

// Accepted process-priority tokens (validated by the bridge). "auto" resolves to a
// concrete class at apply time -- HIGH while any output is live, ABOVE_NORMAL when
// idle; the other three are manual overrides mapped straight to a Win32 class.
inline constexpr const char *kProcessPriorityTokens[] = {"normal", "aboveNormal", "high", "auto"};

// Apply a process-priority token to the current process, resolving "auto" against the
// supplied live state (live -> "high", idle -> "aboveNormal"); any other token applies
// verbatim. On Windows this maps to SetPriorityClass; elsewhere it is a no-op apart
// from the log line. Unknown tokens are ignored.
void ApplyEffectivePriority(const std::string &token, bool live);

// Opt this process's default-render audio session out of (or back into) Windows'
// automatic ducking. On Windows this maps to IAudioSessionControl2::SetDuckingPreference;
// on other platforms it is a no-op.
void DisableAudioDucking(bool disable);

#endif // OBS_MULTISTREAM_FRONTEND_ADVANCED_SETTINGS_HPP_
