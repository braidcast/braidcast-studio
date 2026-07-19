#include "Hotkeys.hpp"
#include "../event_names.hpp"

#include "StorePaths.hpp"
#include "bridge.hpp"
#include "log.hpp"
#include "obs_bootstrap.hpp"
#include "MultistreamEngine.hpp"
#include "util/async_task.hpp"

#include <obs.hpp>
#include <util/dstr.hpp>
#include <util/platform.h>

#include <memory>
#include <unordered_map>
#include <vector>

namespace Hotkeys {

namespace {

// DOM KeyboardEvent.code -> obs_key_t. A data table, not a switch ladder: covers
// letters, digits, F1-F24, navigation/editing keys, arrows, the numpad, and the
// common punctuation rows. An unlisted code resolves to OBS_KEY_NONE and the
// binding is skipped. The physical-key DOM codes are layout-independent, so this
// maps to OBS's physical key enum (the same one obs_key_from_virtual_key yields).
struct DomKey {
	const char *code;
	obs_key_t key;
};

const DomKey kDomKeys[] = {
	// Letters (KeyA..KeyZ)
	{"KeyA", OBS_KEY_A},
	{"KeyB", OBS_KEY_B},
	{"KeyC", OBS_KEY_C},
	{"KeyD", OBS_KEY_D},
	{"KeyE", OBS_KEY_E},
	{"KeyF", OBS_KEY_F},
	{"KeyG", OBS_KEY_G},
	{"KeyH", OBS_KEY_H},
	{"KeyI", OBS_KEY_I},
	{"KeyJ", OBS_KEY_J},
	{"KeyK", OBS_KEY_K},
	{"KeyL", OBS_KEY_L},
	{"KeyM", OBS_KEY_M},
	{"KeyN", OBS_KEY_N},
	{"KeyO", OBS_KEY_O},
	{"KeyP", OBS_KEY_P},
	{"KeyQ", OBS_KEY_Q},
	{"KeyR", OBS_KEY_R},
	{"KeyS", OBS_KEY_S},
	{"KeyT", OBS_KEY_T},
	{"KeyU", OBS_KEY_U},
	{"KeyV", OBS_KEY_V},
	{"KeyW", OBS_KEY_W},
	{"KeyX", OBS_KEY_X},
	{"KeyY", OBS_KEY_Y},
	{"KeyZ", OBS_KEY_Z},
	// Top-row digits (Digit0..Digit9)
	{"Digit0", OBS_KEY_0},
	{"Digit1", OBS_KEY_1},
	{"Digit2", OBS_KEY_2},
	{"Digit3", OBS_KEY_3},
	{"Digit4", OBS_KEY_4},
	{"Digit5", OBS_KEY_5},
	{"Digit6", OBS_KEY_6},
	{"Digit7", OBS_KEY_7},
	{"Digit8", OBS_KEY_8},
	{"Digit9", OBS_KEY_9},
	// Function keys (F1..F24)
	{"F1", OBS_KEY_F1},
	{"F2", OBS_KEY_F2},
	{"F3", OBS_KEY_F3},
	{"F4", OBS_KEY_F4},
	{"F5", OBS_KEY_F5},
	{"F6", OBS_KEY_F6},
	{"F7", OBS_KEY_F7},
	{"F8", OBS_KEY_F8},
	{"F9", OBS_KEY_F9},
	{"F10", OBS_KEY_F10},
	{"F11", OBS_KEY_F11},
	{"F12", OBS_KEY_F12},
	{"F13", OBS_KEY_F13},
	{"F14", OBS_KEY_F14},
	{"F15", OBS_KEY_F15},
	{"F16", OBS_KEY_F16},
	{"F17", OBS_KEY_F17},
	{"F18", OBS_KEY_F18},
	{"F19", OBS_KEY_F19},
	{"F20", OBS_KEY_F20},
	{"F21", OBS_KEY_F21},
	{"F22", OBS_KEY_F22},
	{"F23", OBS_KEY_F23},
	{"F24", OBS_KEY_F24},
	// Whitespace / control / editing
	{"Space", OBS_KEY_SPACE},
	{"Enter", OBS_KEY_RETURN},
	{"Tab", OBS_KEY_TAB},
	{"Escape", OBS_KEY_ESCAPE},
	{"Backspace", OBS_KEY_BACKSPACE},
	{"Delete", OBS_KEY_DELETE},
	{"Insert", OBS_KEY_INSERT},
	{"Home", OBS_KEY_HOME},
	{"End", OBS_KEY_END},
	{"PageUp", OBS_KEY_PAGEUP},
	{"PageDown", OBS_KEY_PAGEDOWN},
	// Arrows
	{"ArrowUp", OBS_KEY_UP},
	{"ArrowDown", OBS_KEY_DOWN},
	{"ArrowLeft", OBS_KEY_LEFT},
	{"ArrowRight", OBS_KEY_RIGHT},
	// Numpad
	{"Numpad0", OBS_KEY_NUM0},
	{"Numpad1", OBS_KEY_NUM1},
	{"Numpad2", OBS_KEY_NUM2},
	{"Numpad3", OBS_KEY_NUM3},
	{"Numpad4", OBS_KEY_NUM4},
	{"Numpad5", OBS_KEY_NUM5},
	{"Numpad6", OBS_KEY_NUM6},
	{"Numpad7", OBS_KEY_NUM7},
	{"Numpad8", OBS_KEY_NUM8},
	{"Numpad9", OBS_KEY_NUM9},
	{"NumpadDecimal", OBS_KEY_NUMPERIOD},
	{"NumpadAdd", OBS_KEY_NUMPLUS},
	{"NumpadSubtract", OBS_KEY_NUMMINUS},
	{"NumpadMultiply", OBS_KEY_NUMASTERISK},
	{"NumpadDivide", OBS_KEY_NUMSLASH},
	{"NumpadEnter", OBS_KEY_ENTER},
	{"NumpadEqual", OBS_KEY_NUMEQUAL},
	// Common punctuation row (US physical positions)
	{"Semicolon", OBS_KEY_SEMICOLON},
	{"Equal", OBS_KEY_EQUAL},
	{"Comma", OBS_KEY_COMMA},
	{"Minus", OBS_KEY_MINUS},
	{"Period", OBS_KEY_PERIOD},
	{"Slash", OBS_KEY_SLASH},
	{"Backquote", OBS_KEY_QUOTELEFT},
	{"BracketLeft", OBS_KEY_BRACKETLEFT},
	{"BracketRight", OBS_KEY_BRACKETRIGHT},
	{"Backslash", OBS_KEY_BACKSLASH},
	{"Quote", OBS_KEY_APOSTROPHE},
};

obs_key_t DomCodeToObsKey(const std::string &code)
{
	// Built once; the table is small enough that a flat scan would also do, but a
	// map keeps the set lookup O(1) and the data list the single source of truth.
	static const std::unordered_map<std::string, obs_key_t> map = [] {
		std::unordered_map<std::string, obs_key_t> m;
		for (const DomKey &dk : kDomKeys) {
			m.emplace(dk.code, dk.key);
		}
		return m;
	}();
	auto it = map.find(code);
	return it == map.end() ? OBS_KEY_NONE : it->second;
}

const char *RegistererTypeName(obs_hotkey_registerer_t type)
{
	switch (type) {
	case OBS_HOTKEY_REGISTERER_FRONTEND:
		return "frontend";
	case OBS_HOTKEY_REGISTERER_SOURCE:
		return "source";
	case OBS_HOTKEY_REGISTERER_OUTPUT:
		return "output";
	case OBS_HOTKEY_REGISTERER_ENCODER:
		return "encoder";
	case OBS_HOTKEY_REGISTERER_SERVICE:
		return "service";
	}
	return "unknown";
}

// Resolve a hotkey's owning object to a display name, or "" when none/unresolvable.
// The registerer is a weak ref of the kind matching registerer_type; resolve it to
// a strong ref just long enough to read its name, then release.
std::string OwnerName(obs_hotkey_t *hotkey)
{
	void *registerer = obs_hotkey_get_registerer(hotkey);
	if (!registerer) {
		return std::string();
	}

	std::string name;
	switch (obs_hotkey_get_registerer_type(hotkey)) {
	case OBS_HOTKEY_REGISTERER_SOURCE: {
		OBSSourceAutoRelease s = obs_weak_source_get_source(static_cast<obs_weak_source_t *>(registerer));
		const char *n = s ? obs_source_get_name(s) : nullptr;
		name = n ? n : "";
		break;
	}
	case OBS_HOTKEY_REGISTERER_OUTPUT: {
		OBSOutputAutoRelease o = obs_weak_output_get_output(static_cast<obs_weak_output_t *>(registerer));
		const char *n = o ? obs_output_get_name(o) : nullptr;
		name = n ? n : "";
		break;
	}
	case OBS_HOTKEY_REGISTERER_ENCODER: {
		OBSEncoderAutoRelease e = obs_weak_encoder_get_encoder(static_cast<obs_weak_encoder_t *>(registerer));
		const char *n = e ? obs_encoder_get_name(e) : nullptr;
		name = n ? n : "";
		break;
	}
	case OBS_HOTKEY_REGISTERER_SERVICE: {
		OBSServiceAutoRelease sv = obs_weak_service_get_service(static_cast<obs_weak_service_t *>(registerer));
		const char *n = sv ? obs_service_get_name(sv) : nullptr;
		name = n ? n : "";
		break;
	}
	case OBS_HOTKEY_REGISTERER_FRONTEND:
		break;
	}
	return name;
}

// Render one binding obs_data ({key, shift, control, alt, command}) as a human
// display string like "Ctrl+Shift+F1" via obs_key_combination_to_str.
std::string BindingDisplay(obs_data_t *binding)
{
	obs_key_combination_t combo = {};
	if (obs_data_get_bool(binding, "shift")) {
		combo.modifiers |= INTERACT_SHIFT_KEY;
	}
	if (obs_data_get_bool(binding, "control")) {
		combo.modifiers |= INTERACT_CONTROL_KEY;
	}
	if (obs_data_get_bool(binding, "alt")) {
		combo.modifiers |= INTERACT_ALT_KEY;
	}
	if (obs_data_get_bool(binding, "command")) {
		combo.modifiers |= INTERACT_COMMAND_KEY;
	}
	combo.key = obs_key_from_name(obs_data_get_string(binding, "key"));

	DStr str;
	obs_key_combination_to_str(combo, str);
	return str->array ? str->array : std::string();
}

// The current bindings of a hotkey as [{display}], read back through obs_hotkey_save.
json BindingsArray(obs_hotkey_id id)
{
	json arr = json::array();
	OBSDataArrayAutoRelease bindings = obs_hotkey_save(id);
	const size_t count = bindings ? obs_data_array_count(bindings) : 0;
	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease item = obs_data_array_item(bindings, i);
		arr.push_back(json{{"display", BindingDisplay(item)}});
	}
	return arr;
}

// Parse the required hotkey id from params (accepts the JS-safe numeric string or a
// raw number). Returns false + fills error when missing/unparseable.
bool IdFromParams(const json &params, obs_hotkey_id &id, std::string &error)
{
	if (params.is_object()) {
		auto it = params.find("id");
		if (it != params.end()) {
			if (it->is_string()) {
				try {
					id = static_cast<obs_hotkey_id>(std::stoull(it->get<std::string>()));
					return true;
				} catch (...) {
				}
			} else if (it->is_number_unsigned() || it->is_number_integer()) {
				id = it->get<obs_hotkey_id>();
				return true;
			}
		}
	}
	error = "missing or invalid 'id'";
	return false;
}

// The frontend hotkey ids we own, so we can unregister exactly them on teardown.
obs_hotkey_id g_startStreamingId = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id g_stopStreamingId = OBS_INVALID_HOTKEY_ID;

// Drive the engine start/stop through the SAME path the streaming.start/stop bridge
// methods use, then mirror their streaming.changed push so the UI updates. Fired on
// key-down only (pressed==true) from libobs's hotkey thread. The engine's binding
// list and encoder cache are UI-thread-owned (no locks of their own; every bridge
// method mutates them on the CEF UI thread), so hop there via AsyncTask::PostToUi --
// the same marshal the engine's onOutputStopped wiring uses -- instead of driving
// the engine from the hotkey thread. PostToUi drops the task once the bridge is
// torn down; the MultistreamAlive() guard covers a task CefShutdown drains after
// Stop() reset the engine (mirrors EmitMultistreamChanged).
void OnStartStreaming(void * /*data*/, obs_hotkey_id /*id*/, obs_hotkey_t * /*hotkey*/, bool pressed)
{
	if (!pressed) {
		return;
	}
	AsyncTask::PostToUi([] {
		if (!ObsBootstrap::MultistreamAlive()) {
			return;
		}
		ObsBootstrap::Multistream().StartAllEnabled();
		Bridge::EmitEvent(EventNames::kStreamingChanged,
				  json{{"active", ObsBootstrap::Multistream().AnyLive()}});
	});
}

void OnStopStreaming(void * /*data*/, obs_hotkey_id /*id*/, obs_hotkey_t * /*hotkey*/, bool pressed)
{
	if (!pressed) {
		return;
	}
	AsyncTask::PostToUi([] {
		if (!ObsBootstrap::MultistreamAlive()) {
			return;
		}
		ObsBootstrap::Multistream().StopAll();
		Bridge::EmitEvent(EventNames::kStreamingChanged, json{{"active", false}});
	});
}

// --- per-scene "switch to scene" hotkeys ------------------------------------

// One rebindable frontend hotkey per GLOBAL (Default-canvas) scene. Stock OBS names
// these "OBSBasic.SelectScene"; we suffix the scene source's uuid so each hotkey's
// persisted NAME (the store's Save/Load key) is unique AND stable across a rename.
struct SceneHotkey {
	std::string uuid;
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
};

// uuid -> owned registration. unique_ptr keeps each SceneHotkey node-stable so the
// void* handed to libobs stays valid until we unregister + erase. UI-thread-only,
// like every other member of this store.
std::unordered_map<std::string, std::unique_ptr<SceneHotkey>> g_sceneHotkeys;

// Reconcile runs only between RegisterFrontendHotkeys and UnregisterFrontendHotkeys,
// so a stray scenes.changed during bootstrap-before-register or teardown is a no-op.
bool g_sceneHotkeysActive = false;

std::string SelectSceneDescription(const std::string &sceneName)
{
	return "Switch to scene: " + sceneName;
}

// Fired on key-down from libobs's hotkey thread. Switch the program scene through the
// SAME seam scenes.setCurrent uses (Bridge::SwitchDefaultProgramScene ->
// Transitions::SetProgramScene + ApplyCanvasSceneLinks), never a raw channel-0 bind,
// so linked additional-canvas scenes stay in sync. The engine/bridge state is
// UI-thread-owned, so hop there via PostToUi (the marshal OnStart/StopStreaming use);
// the MultistreamAlive() guard drops a task CefShutdown drains after teardown.
void OnSelectScene(void *data, obs_hotkey_id /*id*/, obs_hotkey_t * /*hotkey*/, bool pressed)
{
	if (!pressed) {
		return;
	}
	// Read the uuid on the hotkey thread: obs_hotkey_unregister runs under the same
	// hotkey mutex that serializes this trigger loop, so the SceneHotkey cannot be
	// freed while this callback is in flight.
	const std::string uuid = static_cast<SceneHotkey *>(data)->uuid;
	AsyncTask::PostToUi([uuid] {
		if (!ObsBootstrap::MultistreamAlive()) {
			return;
		}
		Bridge::SwitchDefaultProgramScene(uuid);
	});
}

} // namespace

void SyncSceneHotkeys()
{
	if (!g_sceneHotkeysActive) {
		return;
	}

	// Authoritative live set. obs_enum_scenes yields ONLY main/Default-canvas scenes
	// (additional-canvas scenes live in their own namespace and switch via
	// CanvasRuntime, not output 0), so these program-switch hotkeys stay scoped to
	// global scenes.
	struct Ctx {
		std::unordered_map<std::string, std::string> live; // uuid -> current name
	} ctx;
	obs_enum_scenes(
		[](void *param, obs_source_t *source) -> bool {
			auto *c = static_cast<Ctx *>(param);
			const char *u = obs_source_get_uuid(source);
			if (u) {
				const char *n = obs_source_get_name(source);
				c->live.emplace(u, n ? n : "");
			}
			return true;
		},
		&ctx);

	// Drop hotkeys whose scene is gone (removal cleanup). Unregister the live hotkey
	// only -- the persisted binding stays dormant in hotkeys.json so an inactive scene
	// collection's per-scene bindings survive a collection swap and restore on return.
	for (auto it = g_sceneHotkeys.begin(); it != g_sceneHotkeys.end();) {
		if (ctx.live.find(it->first) == ctx.live.end()) {
			obs_hotkey_unregister(it->second->id);
			it = g_sceneHotkeys.erase(it);
		} else {
			++it;
		}
	}

	// Register new scenes; keep existing hotkeys' descriptions tracking the current
	// name so a rename shows through in Settings->Hotkeys without touching the binding.
	bool added = false;
	for (const auto &[uuid, name] : ctx.live) {
		auto it = g_sceneHotkeys.find(uuid);
		if (it == g_sceneHotkeys.end()) {
			auto entry = std::make_unique<SceneHotkey>();
			entry->uuid = uuid;
			const std::string hotkeyName = "OBSBasic.SelectScene." + uuid;
			entry->id = obs_hotkey_register_frontend(hotkeyName.c_str(),
								 SelectSceneDescription(name).c_str(), OnSelectScene,
								 entry.get());
			g_sceneHotkeys.emplace(uuid, std::move(entry));
			added = true;
		} else {
			obs_hotkey_set_description(it->second->id, SelectSceneDescription(name).c_str());
		}
	}

	// New registrations start unbound (stock OBS default); re-apply any persisted
	// binding for them (and, on a collection swap, the activated collection's saved
	// scene bindings). Idempotent for hotkeys already at their saved binding.
	if (added) {
		Load();
	}
}

void RegisterFrontendHotkeys()
{
	if (g_startStreamingId == OBS_INVALID_HOTKEY_ID) {
		g_startStreamingId = obs_hotkey_register_frontend("OBSBasic.StartStreaming", "Start Streaming",
								  OnStartStreaming, nullptr);
	}
	if (g_stopStreamingId == OBS_INVALID_HOTKEY_ID) {
		g_stopStreamingId = obs_hotkey_register_frontend("OBSBasic.StopStreaming", "Stop Streaming",
								 OnStopStreaming, nullptr);
	}

	// Register a switch hotkey for every existing global scene before Load, so their
	// ids exist for the by-name binding restore below.
	g_sceneHotkeysActive = true;
	SyncSceneHotkeys();

	// Apply saved bindings now that every hotkey id (frontend + per-scene + source/etc.)
	// exists.
	Load();
	HostLog("[hotkeys] frontend hotkeys registered (Start/Stop Streaming + " +
		std::to_string(g_sceneHotkeys.size()) + " scene switch); bindings loaded from " +
		MultistreamBasicPath("hotkeys.json"));
}

void UnregisterFrontendHotkeys()
{
	if (g_startStreamingId != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(g_startStreamingId);
		g_startStreamingId = OBS_INVALID_HOTKEY_ID;
	}
	if (g_stopStreamingId != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(g_stopStreamingId);
		g_stopStreamingId = OBS_INVALID_HOTKEY_ID;
	}

	g_sceneHotkeysActive = false;
	for (auto &kv : g_sceneHotkeys) {
		obs_hotkey_unregister(kv.second->id);
	}
	g_sceneHotkeys.clear();
}

bool Save()
{
	OBSDataAutoRelease root = obs_data_create();

	// Iterate every hotkey, saving its bindings array under its (stable) name. The
	// enum callback runs under the hotkey lock; obs_hotkey_save re-locks the same
	// recursive mutex, so calling it here is safe.
	struct Ctx {
		obs_data_t *root;
	} ctx{root};
	obs_enum_hotkeys(
		[](void *param, obs_hotkey_id id, obs_hotkey_t *hotkey) -> bool {
			auto *c = static_cast<Ctx *>(param);
			const char *name = obs_hotkey_get_name(hotkey);
			if (!name) {
				return true;
			}
			OBSDataArrayAutoRelease arr = obs_hotkey_save(id);
			obs_data_set_array(c->root, name, arr);
			return true;
		},
		&ctx);

	const std::string path = MultistreamBasicPath("hotkeys.json");
	if (path.empty()) {
		return false;
	}
	return ReportSaveResult(SaveJsonAtomic(root, path), path);
}

void Load()
{
	const std::string path = MultistreamBasicPath("hotkeys.json");
	OBSDataAutoRelease root = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!root) {
		return; // no saved file yet -- registerers' own bindings stand
	}

	// For each live hotkey, if the file has an entry under its name, load it. Looking
	// up by name keeps ids (per-session) out of the persisted format.
	struct Ctx {
		obs_data_t *root;
	} ctx{root};
	obs_enum_hotkeys(
		[](void *param, obs_hotkey_id id, obs_hotkey_t *hotkey) -> bool {
			auto *c = static_cast<Ctx *>(param);
			const char *name = obs_hotkey_get_name(hotkey);
			if (!name) {
				return true;
			}
			OBSDataArrayAutoRelease arr = obs_data_get_array(c->root, name);
			if (arr) {
				obs_hotkey_load(id, arr);
			}
			return true;
		},
		&ctx);
}

json Snapshot()
{
	OBSDataAutoRelease root = obs_data_create();
	struct Ctx {
		obs_data_t *root;
	} ctx{root};
	obs_enum_hotkeys(
		[](void *param, obs_hotkey_id id, obs_hotkey_t *hotkey) -> bool {
			auto *c = static_cast<Ctx *>(param);
			const char *name = obs_hotkey_get_name(hotkey);
			if (!name) {
				return true;
			}
			OBSDataArrayAutoRelease arr = obs_hotkey_save(id);
			obs_data_set_array(c->root, name, arr);
			return true;
		},
		&ctx);

	const char *js = obs_data_get_json(root);
	return js ? json::parse(js) : json::object();
}

bool RestoreFromSnapshot(const json &snap)
{
	if (!snap.is_object()) {
		return false;
	}
	OBSDataAutoRelease root = obs_data_create_from_json(snap.dump().c_str());
	if (!root) {
		return false;
	}

	// For each live hotkey, load its snapshot bindings by NAME. A hotkey with no
	// snapshot entry is reset to an empty binding set so a binding added after the
	// snapshot is reverted. obs_hotkey_load replaces (not merges) bindings.
	struct Ctx {
		obs_data_t *root;
	} ctx{root};
	obs_enum_hotkeys(
		[](void *param, obs_hotkey_id id, obs_hotkey_t *hotkey) -> bool {
			auto *c = static_cast<Ctx *>(param);
			const char *name = obs_hotkey_get_name(hotkey);
			if (!name) {
				return true;
			}
			OBSDataArrayAutoRelease arr = obs_data_get_array(c->root, name);
			if (!arr) {
				arr = obs_data_array_create(); // absent -> clear
			}
			obs_hotkey_load(id, arr);
			return true;
		},
		&ctx);

	const bool saved = Save();
	Bridge::EmitEvent(EventNames::kHotkeysChanged, json::object());
	return saved;
}

bool MethodList(const json & /*params*/, json &result, std::string & /*error*/)
{
	struct Ctx {
		json hotkeys;
	} ctx{json::array()};

	obs_enum_hotkeys(
		[](void *param, obs_hotkey_id id, obs_hotkey_t *hotkey) -> bool {
			auto *c = static_cast<Ctx *>(param);
			const char *name = obs_hotkey_get_name(hotkey);
			const char *desc = obs_hotkey_get_description(hotkey);
			const std::string owner = OwnerName(hotkey);
			c->hotkeys.push_back(json{
				{"id", std::to_string(id)},
				{"name", name ? name : ""},
				{"description", desc ? desc : ""},
				{"registerer", RegistererTypeName(obs_hotkey_get_registerer_type(hotkey))},
				{"owner", owner.empty() ? json(nullptr) : json(owner)},
				{"bindings", BindingsArray(id)},
			});
			return true;
		},
		&ctx);

	result = json{{"hotkeys", std::move(ctx.hotkeys)}};
	return true;
}

bool MethodSet(const json &params, json &result, std::string &error)
{
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
	if (!IdFromParams(params, id, error)) {
		return false;
	}

	auto bindingsIt = params.is_object() ? params.find("bindings") : params.end();
	if (bindingsIt == params.end() || !bindingsIt->is_array()) {
		error = "hotkeys.set requires a 'bindings' array";
		return false;
	}

	std::vector<obs_key_combination_t> combos;
	combos.reserve(bindingsIt->size());
	for (const json &b : *bindingsIt) {
		if (!b.is_object()) {
			continue;
		}
		const obs_key_t key = DomCodeToObsKey(b.value("code", std::string()));
		const bool ctrl = b.value("ctrl", false);
		const bool shift = b.value("shift", false);
		const bool alt = b.value("alt", false);
		const bool meta = b.value("meta", false);

		// Skip an unmapped code that also carries no modifiers: it would be an empty
		// (dead) binding. A code that mapped to NONE but has modifiers is still skipped
		// -- a modifier-only binding is not something the UI offers here.
		if (key == OBS_KEY_NONE) {
			continue;
		}

		obs_key_combination_t combo = {};
		if (ctrl) {
			combo.modifiers |= INTERACT_CONTROL_KEY;
		}
		if (shift) {
			combo.modifiers |= INTERACT_SHIFT_KEY;
		}
		if (alt) {
			combo.modifiers |= INTERACT_ALT_KEY;
		}
		if (meta) {
			combo.modifiers |= INTERACT_COMMAND_KEY;
		}
		combo.key = key;
		combos.push_back(combo);
	}

	obs_hotkey_load_bindings(id, combos.empty() ? nullptr : combos.data(), combos.size());
	Save();

	result = json{{"bindings", BindingsArray(id)}};
	Bridge::EmitEvent(EventNames::kHotkeysChanged, json::object());
	return true;
}

bool MethodClear(const json &params, json &result, std::string &error)
{
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
	if (!IdFromParams(params, id, error)) {
		return false;
	}
	obs_hotkey_load_bindings(id, nullptr, 0);
	Save();
	result = json{{"bindings", json::array()}};
	Bridge::EmitEvent(EventNames::kHotkeysChanged, json::object());
	return true;
}

} // namespace Hotkeys
