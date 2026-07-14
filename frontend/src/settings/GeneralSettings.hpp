#ifndef OBS_MULTISTREAM_FRONTEND_GENERAL_SETTINGS_HPP_
#define OBS_MULTISTREAM_FRONTEND_GENERAL_SETTINGS_HPP_

#include <string>

// Global "General settings" bag, persisted to general.json in the shared
// braidcast config dir. Some fields drive behavior now (projector
// always-on-top); the rest are persisted prefs that later backlog items
// (Multiview, System Tray, Importer) read. A plain struct in the style of
// CanvasDefinition.
struct GeneralSettings {
	// --- wired now ---
	bool projectorAlwaysOnTop = false;
	// --- preview-drag snapping (fields defined here; behavior wired in task 7c) ---
	bool snapEnabled = true;
	double snapDistance = 10.0; // px in canvas space
	bool snapToEdge = true;     // screen/canvas edges
	bool snapToSource = true;   // other scene items' edges
	bool snapToCenter = true;   // canvas center lines
	// --- warnings (consumed by the frontend Studio bar) ---
	bool warnBeforeGoLive = false;
	bool warnBeforeStop = false;
	// --- persisted prefs consumed by later backlog items ---
	bool startMinimized = false;                   // Item 11 (tray)
	bool minimizeToTray = false;                   // Item 11
	bool alwaysShowTray = false;                   // Item 11
	std::string multiviewLayout = "horizontalTop"; // Item 10
	bool multiviewDrawNames = true;                // Item 10
	bool multiviewDrawSafeAreas = false;           // Item 10
	bool importerPrompts = true;                   // Item 17
	bool scenesGridMode = false;                   // Item 13 (Scenes panel grid/list view)

	// Round-trip every field to general.json (file keys snake_case). Missing keys
	// fall back to the struct defaults. Save() is called on each bridge set.
	void Load();
	void Save() const;
};

// Field descriptors: the SINGLE source for the wire (camelCase) <-> file
// (snake_case) <-> struct-member mapping. Both the persistence layer
// (GeneralSettings::Load/Save) and the bridge (settings.getGeneral/setGeneral)
// iterate these, so the two layers cannot drift.
struct GeneralBoolField {
	const char *json;
	const char *file;
	bool GeneralSettings::*member;
};
struct GeneralStringField {
	const char *json;
	const char *file;
	std::string GeneralSettings::*member;
};
struct GeneralDoubleField {
	const char *json;
	const char *file;
	double GeneralSettings::*member;
	double min;
	double max;
};

inline constexpr GeneralBoolField kGeneralBoolFields[] = {
	{"projectorAlwaysOnTop", "projector_always_on_top", &GeneralSettings::projectorAlwaysOnTop},
	{"snapEnabled", "snap_enabled", &GeneralSettings::snapEnabled},
	{"snapToEdge", "snap_to_edge", &GeneralSettings::snapToEdge},
	{"snapToSource", "snap_to_source", &GeneralSettings::snapToSource},
	{"snapToCenter", "snap_to_center", &GeneralSettings::snapToCenter},
	{"warnBeforeGoLive", "warn_before_go_live", &GeneralSettings::warnBeforeGoLive},
	{"warnBeforeStop", "warn_before_stop", &GeneralSettings::warnBeforeStop},
	{"startMinimized", "start_minimized", &GeneralSettings::startMinimized},
	{"minimizeToTray", "minimize_to_tray", &GeneralSettings::minimizeToTray},
	{"alwaysShowTray", "always_show_tray", &GeneralSettings::alwaysShowTray},
	{"multiviewDrawNames", "multiview_draw_names", &GeneralSettings::multiviewDrawNames},
	{"multiviewDrawSafeAreas", "multiview_draw_safe_areas", &GeneralSettings::multiviewDrawSafeAreas},
	{"importerPrompts", "importer_prompts", &GeneralSettings::importerPrompts},
	{"scenesGridMode", "scenes_grid_mode", &GeneralSettings::scenesGridMode},
};
inline constexpr GeneralStringField kGeneralStringFields[] = {
	{"multiviewLayout", "multiview_layout", &GeneralSettings::multiviewLayout},
};
inline constexpr GeneralDoubleField kGeneralDoubleFields[] = {
	{"snapDistance", "snap_distance", &GeneralSettings::snapDistance, 0.0, 100.0},
};

#endif // OBS_MULTISTREAM_FRONTEND_GENERAL_SETTINGS_HPP_
