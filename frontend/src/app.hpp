#ifndef OBS_MULTISTREAM_FRONTEND_APP_HPP_
#define OBS_MULTISTREAM_FRONTEND_APP_HPP_

#include <string>

#include "include/cef_app.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_scheme.h"
#include "include/wrapper/cef_message_router.h"

// Browser-process application callbacks. The UI browser itself is created
// explicitly from WinMain() once the host window exists (so it can be embedded
// as a child via CefWindowInfo::SetAsChild), not from OnContextInitialized().
//
// The same executable hosts every CEF process, so App also serves the render
// process: it implements CefRenderProcessHandler and owns the renderer side of
// the message router that exposes window.cefQuery().
class App : public CefApp, public CefBrowserProcessHandler, public CefRenderProcessHandler {
public:
	explicit App(std::string startup_url);

	const std::string &startup_url() const { return startup_url_; }

	// CefApp methods:
	CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
	CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }
	// Register the custom app:// scheme in every process before CefInitialize.
	void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override;

	// CefBrowserProcessHandler methods:
	void OnContextInitialized() override;

	// CefRenderProcessHandler methods:
	void OnWebKitInitialized() override;
	void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
			      CefRefPtr<CefV8Context> context) override;
	void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
			       CefRefPtr<CefV8Context> context) override;
	bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
				      CefProcessId source_process, CefRefPtr<CefProcessMessage> message) override;

private:
	const std::string startup_url_;

	// Renderer-side message router for the window.cefQuery() bridge. Created in
	// OnWebKitInitialized(); only touched on the render-process main thread.
	CefRefPtr<CefMessageRouterRendererSide> message_router_;

	IMPLEMENT_REFCOUNTING(App);
};

#endif // OBS_MULTISTREAM_FRONTEND_APP_HPP_
