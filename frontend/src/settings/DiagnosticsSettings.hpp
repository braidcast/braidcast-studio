#ifndef OBS_MULTISTREAM_FRONTEND_DIAGNOSTICS_SETTINGS_HPP_
#define OBS_MULTISTREAM_FRONTEND_DIAGNOSTICS_SETTINGS_HPP_

// Global diagnostics settings bag, persisted to diagnostics.json in the shared
// braidcast config dir. Mirrors AdvancedSettings exactly: a plain struct whose
// fields are the single source of truth, round-tripped through a descriptor table
// shared by Load and Save so the two can't drift. Backs the gated DEBUG channel's
// persisted default (seeded at boot, flipped by the diagnostics.setDebug bridge
// method).
struct DiagnosticsSettings {
	bool debugLogging = false;

	// Round-trip every field to diagnostics.json (file keys snake_case). Missing
	// keys fall back to the struct defaults.
	void Load();
	void Save() const;
};

// Field descriptor: the SINGLE source for the file (snake_case) <-> struct-member
// mapping, iterated by both DiagnosticsSettings::Load and ::Save so the two
// layers cannot drift.
struct DiagnosticsBoolField {
	const char *file;
	bool DiagnosticsSettings::*member;
};

inline constexpr DiagnosticsBoolField kDiagnosticsBoolFields[] = {
	{"debug_logging", &DiagnosticsSettings::debugLogging},
};

#endif // OBS_MULTISTREAM_FRONTEND_DIAGNOSTICS_SETTINGS_HPP_
