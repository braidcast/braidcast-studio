#include "app.hpp"

#include "include/cef_scheme.h"

#include "scheme.hpp"

App::App(std::string startup_url) : startup_url_(std::move(startup_url)) {}

void App::OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar)
{
	// app:// must be registered identically in every process and before
	// CefInitialize. Standard + secure + CORS-enabled so the offline bundle
	// behaves like an https origin (fetch/import/module loading work).
	registrar->AddCustomScheme(kAppScheme, CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE |
						       CEF_SCHEME_OPTION_CORS_ENABLED |
						       CEF_SCHEME_OPTION_FETCH_ENABLED);
}

void App::OnContextInitialized()
{
	// Factory registration must happen in the browser process after init.
	RegisterAppSchemeHandlerFactory();
}

void App::OnWebKitInitialized()
{
	CefMessageRouterConfig config;
	message_router_ = CefMessageRouterRendererSide::Create(config);
}

void App::OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context)
{
	message_router_->OnContextCreated(browser, frame, context);
}

void App::OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context)
{
	message_router_->OnContextReleased(browser, frame, context);
}

bool App::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
				   CefProcessId source_process, CefRefPtr<CefProcessMessage> message)
{
	return message_router_->OnProcessMessageReceived(browser, frame, source_process, message);
}
