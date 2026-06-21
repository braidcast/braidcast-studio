#ifndef OBS_MULTISTREAM_FRONTEND_CLIENT_HPP_
#define OBS_MULTISTREAM_FRONTEND_CLIENT_HPP_

#include <list>
#include <memory>

#include "include/cef_client.h"
#include "include/cef_display_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_request_handler.h"
#include "include/wrapper/cef_message_router.h"

class ObsQueryHandler;

// Browser-level callbacks for the UI browser. Tracks live browsers and quits the
// message loop when the last one closes. Owns the browser side of the message
// router and the ObsQueryHandler answering window.cefQuery(). Implements
// CefDisplayHandler/CefLoadHandler so page console output and load completion are
// observable from C++ for headless verification.
class Client : public CefClient,
	       public CefLifeSpanHandler,
	       public CefDisplayHandler,
	       public CefLoadHandler,
	       public CefRequestHandler {
public:
	Client();

	// CefClient methods:
	CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
	CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
	CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
	CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
	bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
				      CefProcessId source_process, CefRefPtr<CefProcessMessage> message) override;

	// CefLifeSpanHandler methods:
	void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
	void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

	// CefDisplayHandler methods:
	bool OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level, const CefString &message,
			      const CefString &source, int line) override;

	// CefLoadHandler methods:
	void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int http_status_code) override;
	void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode error_code,
			 const CefString &error_text, const CefString &failed_url) override;

	// CefRequestHandler methods:
	bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request,
			    bool user_gesture, bool is_redirect) override;
	void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status, int error_code,
				       const CefString &error_string) override;

private:
	// Live browser windows. Only accessed on the CEF UI thread.
	typedef std::list<CefRefPtr<CefBrowser>> BrowserList;
	BrowserList browser_list_;

	// Browser-side message router for the window.cefQuery() bridge, plus the
	// handler it dispatches to. Both live on the browser-process UI thread.
	CefRefPtr<CefMessageRouterBrowserSide> message_router_;
	std::unique_ptr<ObsQueryHandler> obs_query_handler_;

	IMPLEMENT_REFCOUNTING(Client);
};

#endif // OBS_MULTISTREAM_FRONTEND_CLIENT_HPP_
