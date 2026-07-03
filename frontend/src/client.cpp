#include "client.hpp"

#include "include/cef_app.h"
#include "include/wrapper/cef_helpers.h"

#include "bridge.hpp"
#include "log.hpp"
#include "window_chrome.hpp"

Client::Client() = default;

namespace {
// The single process-wide Client (main window + every detached window's browser
// share it). Only touched on the CEF UI thread.
CefRefPtr<Client> g_shared_client;
} // namespace

CefRefPtr<Client> Client::Shared()
{
	return g_shared_client;
}

void Client::SetShared(CefRefPtr<Client> client)
{
	g_shared_client = client;
}

bool Client::OnBeforePopup(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> /*frame*/,
			   const CefString &target_url, const CefString & /*target_frame_name*/,
			   CefLifeSpanHandler::WindowOpenDisposition /*target_disposition*/, bool /*user_gesture*/,
			   const CefPopupFeatures & /*popupFeatures*/, CefWindowInfo & /*windowInfo*/,
			   CefRefPtr<CefClient> & /*client*/, CefBrowserSettings & /*settings*/,
			   CefRefPtr<CefDictionaryValue> & /*extra_info*/, bool * /*no_javascript_access*/)
{
	CEF_REQUIRE_UI_THREAD();
	HostLog("[cef] OnBeforePopup canceled (host-driven detach): " + target_url.ToString());
	return true; // cancel: all real windows are host-driven, never page popups
}

void Client::OnAfterCreated(CefRefPtr<CefBrowser> browser)
{
	CEF_REQUIRE_UI_THREAD();

	if (!message_router_) {
		CefMessageRouterConfig config;
		message_router_ = CefMessageRouterBrowserSide::Create(config);

		obs_query_handler_ = std::make_unique<ObsQueryHandler>();
		message_router_->AddHandler(obs_query_handler_.get(), false);
	}

	browser_list_.push_back(browser);

	// Register this browser as an EmitEvent target: server->client push
	// broadcasts to every live UI browser. (obs-browser OSR sources have no
	// Client and are never seen here.)
	Bridge::AddBrowser(browser);

	HostLog("[cef] browser created");
}

void Client::OnBeforeClose(CefRefPtr<CefBrowser> browser)
{
	CEF_REQUIRE_UI_THREAD();

	if (message_router_) {
		message_router_->OnBeforeClose(browser);
	}

	// Unregister this browser so a late event push can't touch a dying browser.
	Bridge::RemoveBrowser(browser);

	for (BrowserList::iterator it = browser_list_.begin(); it != browser_list_.end(); ++it) {
		if ((*it)->IsSame(browser)) {
			browser_list_.erase(it);
			break;
		}
	}

	if (browser_list_.empty()) {
		// Last browser closed: drop the router (and its handler) before the
		// app shuts down, then quit the application message loop.
		if (message_router_) {
			message_router_->RemoveHandler(obs_query_handler_.get());
			message_router_ = nullptr;
		}
		obs_query_handler_.reset();

		CefQuitMessageLoop();
	}
}

bool Client::OnConsoleMessage(CefRefPtr<CefBrowser> /*browser*/, cef_log_severity_t /*level*/, const CefString &message,
			      const CefString &source, int line)
{
	HostLog("[console] " + message.ToString() + " (" + source.ToString() + ":" + std::to_string(line) + ")");
	return false;
}

void Client::OnDraggableRegionsChanged(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> /*frame*/,
				       const std::vector<CefDraggableRegion> &regions)
{
	CEF_REQUIRE_UI_THREAD();
	WindowChrome::SetDraggableRegions(browser, regions);
}

void Client::OnLoadEnd(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame, int http_status_code)
{
	CEF_REQUIRE_UI_THREAD();
	if (frame->IsMain()) {
		HostLog("[cef] page loaded: " + frame->GetURL().ToString() +
			" (status=" + std::to_string(http_status_code) + ")");
	}
}

void Client::OnLoadError(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame, ErrorCode error_code,
			 const CefString &error_text, const CefString &failed_url)
{
	CEF_REQUIRE_UI_THREAD();
	if (error_code == ERR_ABORTED) {
		return;
	}
	HostLog("[cef] load error " + std::to_string(error_code) + " (" + error_text.ToString() + ") for " +
		failed_url.ToString());
	(void)frame;
}

bool Client::OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> /*request*/,
			    bool /*user_gesture*/, bool /*is_redirect*/)
{
	CEF_REQUIRE_UI_THREAD();

	if (message_router_) {
		message_router_->OnBeforeBrowse(browser, frame);
	}
	return false;
}

void Client::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus /*status*/,
				       int /*error_code*/, const CefString & /*error_string*/)
{
	CEF_REQUIRE_UI_THREAD();

	if (message_router_) {
		message_router_->OnRenderProcessTerminated(browser);
	}
}

void Client::OnBeforeContextMenu(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> /*frame*/,
				 CefRefPtr<CefContextMenuParams> /*params*/, CefRefPtr<CefMenuModel> model)
{
	CEF_REQUIRE_UI_THREAD();
	// Drop every default entry (Back/Reload/Print/View source/Inspect) so a right-click
	// on the UI does nothing browser-y; the web app renders its own menus in the DOM.
	model->Clear();
}

bool Client::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
				      CefProcessId source_process, CefRefPtr<CefProcessMessage> message)
{
	CEF_REQUIRE_UI_THREAD();

	if (message_router_ && message_router_->OnProcessMessageReceived(browser, frame, source_process, message)) {
		return true;
	}
	return false;
}
