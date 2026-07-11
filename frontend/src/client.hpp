#ifndef OBS_MULTISTREAM_FRONTEND_CLIENT_HPP_
#define OBS_MULTISTREAM_FRONTEND_CLIENT_HPP_

#include <list>
#include <memory>

#include "include/cef_client.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_display_handler.h"
#include "include/cef_drag_handler.h"
#include "include/cef_keyboard_handler.h"
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
	       public CefDragHandler,
	       public CefLoadHandler,
	       public CefRequestHandler,
	       public CefContextMenuHandler,
	       public CefKeyboardHandler {
public:
	Client();

	// The single process-wide Client. The main window's browser and every
	// detached window's child browser share ONE Client instance, so all browsers
	// register in the same browser_list_ + Bridge emit registry and the message
	// loop quits only when the LAST browser across all windows closes. Set once in
	// wWinMain after the main client is constructed; read by WindowManager::Detach.
	static CefRefPtr<Client> Shared();
	static void SetShared(CefRefPtr<Client> client);

	// CefClient methods:
	CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
	CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
	CefRefPtr<CefDragHandler> GetDragHandler() override { return this; }
	CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
	CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
	CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
	CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
	bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
				      CefProcessId source_process, CefRefPtr<CefProcessMessage> message) override;

	// CefLifeSpanHandler methods:
	// Cancel every page-initiated popup (window.open / target=_blank): all real
	// windows are host-driven, so the page must never spawn an unmanaged CEF popup.
	bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString &target_url,
			   const CefString &target_frame_name, CefLifeSpanHandler::WindowOpenDisposition target_disposition,
			   bool user_gesture,
			   const CefPopupFeatures &popupFeatures, CefWindowInfo &windowInfo, CefRefPtr<CefClient> &client,
			   CefBrowserSettings &settings, CefRefPtr<CefDictionaryValue> &extra_info,
			   bool *no_javascript_access) override;
	void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
	void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

	// CefDisplayHandler methods:
	bool OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level, const CefString &message,
			      const CefString &source, int line) override;

	// CefDragHandler methods:
	// The Svelte title bar marks its background `-webkit-app-region: drag`; forward
	// the reported regions to the frameless host so that strip becomes the drag grip.
	void OnDraggableRegionsChanged(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
				       const std::vector<CefDraggableRegion> &regions) override;

	// CefLoadHandler methods:
	void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int http_status_code) override;
	void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode error_code,
			 const CefString &error_text, const CefString &failed_url) override;

	// CefRequestHandler methods:
	bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request,
			    bool user_gesture, bool is_redirect) override;
	void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status, int error_code,
				       const CefString &error_string) override;

	// CefContextMenuHandler methods:
	// Suppress CEF's default browser context menu (Print / View source / Reload …)
	// so right-click matches a native desktop app; the web UI supplies its own DOM
	// menus where it needs them.
	void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
				 CefRefPtr<CefContextMenuParams> params, CefRefPtr<CefMenuModel> model) override;

	// CefKeyboardHandler methods:
	// Swallow the browser-chrome shortcuts (reload / whole-UI zoom / find / print /
	// view-source / history back-forward) so the frameless app never acts on them.
	// App shortcuts and the libobs global-hotkey thread run on separate paths, so no
	// user-bindable key is lost here.
	bool OnPreKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent &event, CefEventHandle os_event,
			   bool *is_keyboard_shortcut) override;

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
