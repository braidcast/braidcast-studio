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
// Event push (C++->JS): Bridge::EmitEvent posts to the CEF UI thread, gets the
// UI browser's main frame, and ExecuteJavaScripts window.__obsEmit(name,
// payload), which the JS client (obs-bridge.js) fans out to obs.on()
// subscribers. The UI browser ref is held via Bridge::SetUiBrowser; emits no-op
// safely before it exists or after it is gone.
namespace Bridge {

using json = nlohmann::json;

// Build the method registry. Idempotent; call once during bootstrap. Also
// registers the obs frontend event callback that forwards obs->JS as
// "obs.event". Safe to call before the UI browser exists.
void Init();

// Unregister the obs frontend event callback. Call during teardown while libobs
// is still up (before obs_shutdown), on the UI thread.
void Shutdown();

// Hold / drop the UI browser used as the EmitEvent target. Called from the CEF
// UI thread (Client life-span callbacks).
void SetUiBrowser(CefRefPtr<CefBrowser> browser);
void ClearUiBrowser();

// Dispatch one envelope. Returns true on success (result populated), false on
// failure (error populated with a human-readable message). Runs on the browser
// UI thread (the message-router callback thread).
bool Dispatch(const std::string &method, const json &params, json &result, std::string &error);

// Fan a server-push event to JS. Thread-safe: posts to TID_UI if not already
// there. payload is any JSON value (object/array/scalar/null).
void EmitEvent(const std::string &name, const json &payload);

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
