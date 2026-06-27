#ifndef OBS_MULTISTREAM_FRONTEND_BRIDGE_HPP_
#define OBS_MULTISTREAM_FRONTEND_BRIDGE_HPP_

#include <string>

#include <nlohmann/json.hpp>

#include "include/cef_browser.h"
#include "include/wrapper/cef_message_router.h"

// The JS<->C++ bridge: a typed request/response surface plus server->client
// event push, the contract the Svelte UI sits on (4.1.5+).
//
// Request/response (JS->C++): the renderer-side CefMessageRouter sends an
// envelope JSON {"method":<name>,"params":<any>}; ObsQueryHandler dispatches it
// through a method registry and answers Success(<result-json>) / Failure(code,
// msg). Methods are data, not branches: adding one is a single registry
// insertion in bridge.cpp.
//
// Event push (C++->JS): Bridge::EmitEvent posts to the CEF UI thread and
// broadcasts window.__obsEmit(name, payload) to every registered browser's main
// frame, which the JS client (obs-bridge.js) fans out to obs.on() subscribers.
// Browsers register via Bridge::AddBrowser / drop via Bridge::RemoveBrowser;
// emits no-op safely before any browser exists or after all are gone. With a
// single registered browser this is behavior-identical to a single-target emit.
namespace Bridge {

using json = nlohmann::json;

// Build the method registry. Idempotent; call once during bootstrap. Also
// registers the obs frontend event callback that forwards obs->JS as
// "obs.event". Safe to call before the UI browser exists.
void Init();

// Unregister the obs frontend event callback. Call during teardown while libobs
// is still up (before obs_shutdown), on the UI thread.
void Shutdown();

// Seed (first run) or restore (subsequent runs) the global audio devices (Desktop
// Audio / Mic) on output channels 1..6 from audio_devices.json, the way stock OBS
// sets them up. Call after modules load and the default scene is bound, before the
// AudioMonitor is constructed so its initial Rebuild sees the seeded devices.
void SeedGlobalAudio();

// Unbind every global audio channel so the wasapi sources are destroyed before
// obs_shutdown. Call during teardown while libobs is still up.
void ClearGlobalAudio();

// Register / unregister a live browser as an EmitEvent target. Called from the
// CEF UI thread (Client life-span callbacks). EmitEvent broadcasts to ALL
// registered browsers so a state change in one window updates every window.
void AddBrowser(CefRefPtr<CefBrowser> browser);
void RemoveBrowser(CefRefPtr<CefBrowser> browser);

// Dispatch one envelope. Returns true on success (result populated), false on
// failure (error populated with a human-readable message). Runs on the browser
// UI thread (the message-router callback thread).
bool Dispatch(const std::string &method, const json &params, json &result, std::string &error);

// Fan a server-push event to JS. Thread-safe: posts to TID_UI if not already
// there. payload is any JSON value (object/array/scalar/null).
void EmitEvent(const std::string &name, const json &payload);

// Push the current multistream output statuses as the "multistream.changed"
// event. Wired as the engine's onStatusChanged; safe to call off the UI thread
// (EmitEvent marshals to TID_UI).
void EmitMultistreamChanged();

// Push the virtual-camera state ({active, canvas}) as the "virtualCam.changed"
// event. Wired as the VirtualCamManager's onChanged; safe to call off the UI
// thread (it marshals to TID_UI before reading the manager + emitting).
void EmitVirtualCamChanged();

// Push the coalesced per-source audio levels as the "audio.levels" event. Called
// by the AudioMonitor's volmeter callback FROM THE AUDIO THREAD; this marshals to
// TID_UI, throttles to ~30 Hz, then snapshots the monitor's levels and builds the
// payload on the UI thread (the 4.4.4 cross-thread-read discipline).
void EmitAudioLevels();

// Push "audio.changed" so the UI re-lists when the active audio source set
// changes. Safe to call off the UI thread (EmitEvent marshals to TID_UI).
void EmitAudioChanged();

// Write/read a single string value under `key` to/from a MultistreamBasicPath
// JSON file (atomic, with a .bak). Shared by the per-feature key/value stores
// (audio_devices.json / theme.json / layout.json / transitions.json).
bool WriteJsonString(const char *file, const char *key, const std::string &value);
std::string ReadJsonString(const char *file, const char *key);

} // namespace Bridge

// Browser-side query handler for the window.cefQuery() bridge. Parses the
// envelope, dispatches through Bridge::Dispatch, and answers the callback. Runs
// on the browser-process UI thread.
class ObsQueryHandler : public CefMessageRouterBrowserSide::Handler {
public:
	bool OnQuery(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int64_t query_id,
		     const CefString &request, bool persistent, CefRefPtr<Callback> callback) override;
};

#endif // OBS_MULTISTREAM_FRONTEND_BRIDGE_HPP_
