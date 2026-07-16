#pragma once

#include <nlohmann/json.hpp>

#include <obs.h>

#include <string>

// Hotkey enumeration, binding, persistence, and the frontend hotkeys the CEF host
// owns (Start/Stop Streaming). libobs already FIRES bound hotkeys on its own: the
// hotkey thread (started in obs_startup) polls the OS keyboard via GetAsyncKeyState
// on Windows and invokes each bound hotkey's callback globally -- so once bindings
// are loaded they "just work" with no key-event injection or keyboard hook needed.
namespace Hotkeys {

using json = nlohmann::json;

// Register the frontend-owned hotkeys (Start/Stop Streaming) wired to the
// multistream engine, then load saved bindings from hotkeys.json. Call once during
// bootstrap AFTER the engine + scenes are up so the callbacks have something to
// drive and every hotkey id (frontend + source/output/etc.) exists for Load to
// resolve by name. Idempotent within a process.
void RegisterFrontendHotkeys();

// Unregister the frontend-owned hotkeys. Call during teardown while libobs is still
// up (before obs_shutdown), on the UI thread.
void UnregisterFrontendHotkeys();

// Persist EVERY hotkey's current bindings to hotkeys.json, keyed by hotkey NAME
// (stable across runs; ids are per-session). Atomic save with a .bak. Returns false
// on write failure (already logged).
bool Save();

// Load saved bindings from hotkeys.json: for each saved name, resolve the live
// hotkey id and apply its bindings. Hotkeys with no saved entry keep whatever
// bindings their registerer loaded. Safe before any hotkey exists (no-op).
void Load();

// Capture EVERY hotkey's current bindings as a JSON blob keyed by hotkey NAME, in
// the SAME shape Save()/Load() persist (a restorable snapshot, unlike MethodList's
// display-only output). Used by settings.snapshot.
json Snapshot();

// Revert every live hotkey to the bindings in `snap` (a blob from Snapshot()):
// each named entry's bindings are loaded, and a hotkey absent from the snapshot is
// cleared, so a binding added after the snapshot is undone. Persists + emits
// hotkeys.changed. Used by settings.restore. Returns false on a malformed snapshot or
// a failed persist (already logged).
bool RestoreFromSnapshot(const json &snap);

// Bridge method bodies (registered in g_methods). See bridge.cpp / the header doc
// on each for the JSON contract.
bool MethodList(const json &params, json &result, std::string &error);
bool MethodSet(const json &params, json &result, std::string &error);
bool MethodClear(const json &params, json &result, std::string &error);

} // namespace Hotkeys
