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
	// --- browser source HW accel (store-only; obs-browser reads its own config) ---
	bool browserHwAccel = true;

	// Round-trip every field to advanced.json (file keys snake_case). Missing keys
	// fall back to the struct defaults. Save() is called on each bridge set.
	void Load();
	bool Save() const;
};

// How a field presents itself in the Settings UI. Carried on the field descriptor
// rather than in the Svelte tab so a field is declared exactly once: the tab used
// to restate every label, range and backend default by hand, and had already
// drifted (it seeded streamDelaySec 0 / streamDelayPreserve false against the 20 /
// true above). `order` sorts the rendered form; `group` becomes a section heading,
// emitted in first-appearance order. `enabledWhen` names a bool field in this same
// struct that must be true for this one to be editable -- the delay-seconds box
// following the delay toggle. An empty `label` keeps a field off the form entirely
// while still persisting and round-tripping.
struct SettingsFieldUi {
	const char *label;
	const char *hint;        // "" = none
	const char *group;       // section heading; "" = ungrouped
	const char *enabledWhen; // "" = always editable; else a bool field's json key
	int order;
};

// Field descriptors: the SINGLE source for the wire (camelCase) <-> file
// (snake_case) <-> struct-member mapping, plus the UI presentation above. The
// persistence layer (AdvancedSettings::Load/Save), the bridge
// (settings.getAdvanced/setAdvanced) and the properties form all iterate these, so
// none of the three can drift.
struct AdvancedBoolField {
	const char *json;
	const char *file;
	bool AdvancedSettings::*member;
	SettingsFieldUi ui;
};
struct AdvancedStringField {
	const char *json;
	const char *file;
	std::string AdvancedSettings::*member;
	SettingsFieldUi ui;
	// Fixed choices rendered as a dropdown, as "value\0Label" pairs terminated by a
	// null entry; an empty first value means a free-text box instead.
	const char *const (*options)[2];
};
struct AdvancedUIntField {
	const char *json;
	const char *file;
	uint32_t AdvancedSettings::*member;
	uint32_t min;
	uint32_t max;
	SettingsFieldUi ui;
};

inline constexpr const char *const kProcessPriorityOptions[][2] = {
	{"auto", "Auto - High while live, Above Normal idle"},
	{"normal", "Normal"},
	{"aboveNormal", "Above Normal"},
	{"high", "High"},
	{nullptr, nullptr},
};

inline constexpr AdvancedBoolField kAdvancedBoolFields[] = {
	{"streamDelayEnabled",
	 "stream_delay_enabled",
	 &AdvancedSettings::streamDelayEnabled,
	 {"Enable stream delay", "", "Stream Delay", "", 20}},
	{"streamDelayPreserve",
	 "stream_delay_preserve",
	 &AdvancedSettings::streamDelayPreserve,
	 {"Preserve delay on disconnect/reconnect", "", "Stream Delay", "streamDelayEnabled", 22}},
	{"reconnectEnabled",
	 "reconnect_enabled",
	 &AdvancedSettings::reconnectEnabled,
	 {"Automatically reconnect", "", "Reconnect", "", 30}},
	{"newSocketLoop",
	 "new_socket_loop",
	 &AdvancedSettings::newSocketLoop,
	 {"Enable new networking code", "", "Network", "", 41}},
	{"lowLatencyMode",
	 "low_latency_mode",
	 &AdvancedSettings::lowLatencyMode,
	 {"Low-latency mode", "", "Network", "", 42}},
	{"browserHwAccel",
	 "browser_hw_accel",
	 &AdvancedSettings::browserHwAccel,
	 {"Enable browser source hardware acceleration", "Takes effect after restart.", "Browser", "", 50}},
	{"disableAudioDucking",
	 "disable_audio_ducking",
	 &AdvancedSettings::disableAudioDucking,
	 {"Disable Windows audio ducking", "", "Process", "", 11}},
};
inline constexpr AdvancedStringField kAdvancedStringFields[] = {
	{"processPriority",
	 "process_priority",
	 &AdvancedSettings::processPriority,
	 {"Process priority", "Applies immediately and on next launch.", "Process", "", 10},
	 kProcessPriorityOptions},
	{"bindIP",
	 "bind_ip",
	 &AdvancedSettings::bindIP,
	 {"Bind to IP", "\"default\" binds nothing; otherwise a literal local IP.", "Network", "", 40},
	 nullptr},
};
inline constexpr AdvancedUIntField kAdvancedUIntFields[] = {
	{"streamDelaySec",
	 "stream_delay_sec",
	 &AdvancedSettings::streamDelaySec,
	 0,
	 7200,
	 {"Delay (seconds)", "", "Stream Delay", "streamDelayEnabled", 21}},
	{"reconnectRetryDelaySec",
	 "reconnect_retry_delay_sec",
	 &AdvancedSettings::reconnectRetryDelaySec,
	 0,
	 3600,
	 {"Retry delay (seconds)", "", "Reconnect", "reconnectEnabled", 31}},
	{"reconnectMaxRetries",
	 "reconnect_max_retries",
	 &AdvancedSettings::reconnectMaxRetries,
	 0,
	 10000,
	 {"Maximum retries", "", "Reconnect", "reconnectEnabled", 32}},
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
