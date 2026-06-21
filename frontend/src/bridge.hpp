#ifndef OBS_MULTISTREAM_FRONTEND_BRIDGE_HPP_
#define OBS_MULTISTREAM_FRONTEND_BRIDGE_HPP_

#include "include/wrapper/cef_message_router.h"

// Browser-side query handler for the window.cefQuery() bridge. libobs is
// initialized in this (the browser) process, so obs_get_version_string() is
// answered here. Runs on the browser-process UI thread. This is the seed of the
// real JS<->C++ bridge that 4.1.4 grows into a typed request/response surface.
class ObsQueryHandler : public CefMessageRouterBrowserSide::Handler {
public:
	bool OnQuery(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int64_t query_id,
		     const CefString &request, bool persistent, CefRefPtr<Callback> callback) override;
};

#endif // OBS_MULTISTREAM_FRONTEND_BRIDGE_HPP_
