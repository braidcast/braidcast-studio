#include "client.hpp"

#include "include/cef_app.h"
#include "include/wrapper/cef_helpers.h"

#include <chrono>
#include <string>
#include <unordered_map>

#include "bridge.hpp"
#include "gpu_safe_mode.hpp"
#include "log.hpp"
#include "windowing/window_chrome.hpp"

Client::Client() = default;

namespace {
// The single process-wide Client (main window + every detached window's browser
// share it). Only touched on the CEF UI thread.
CefRefPtr<Client> g_shared_client;

// Render-process crash bookkeeping per browser id, so the reload in
// OnRenderProcessTerminated is bounded. A deterministic crash would otherwise
// reload -> crash -> reload at full speed, spawning a render process per cycle.
// Steady (not wall) clock: a forward system-clock jump larger than the window would
// otherwise refill the retry budget mid-loop and defeat the bound.
struct RendererCrash {
	int count = 0;
	std::chrono::steady_clock::time_point last{};
};
std::unordered_map<int, RendererCrash> g_rendererCrashes;
constexpr int kMaxRendererReloads = 3;
// A browser that survived this long since its last crash counts as recovered, so an
// unrelated crash later in the session still gets the full retry budget.
constexpr std::chrono::seconds kRendererCrashWindow{60};
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
	g_rendererCrashes.erase(browser->GetIdentifier());

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
	const std::string msg = message.ToString();
	const std::string loc = " (" + source.ToString() + ":" + std::to_string(line) + ")";

	// The web logger prefixes each line with "[<L>][<cat>]" (L in D/I/W/E). Parse
	// it and re-emit through blog at the matching level so the line lands in the
	// session file with its level + category. Anything without the prefix is stray
	// library console output; capture it untagged under [cef] at INFO.
	int level = 0;
	if (msg.size() >= 5 && msg[0] == '[' && msg[2] == ']' && msg[3] == '[') {
		switch (msg[1]) {
		case 'D':
			level = LOG_DEBUG;
			break;
		case 'I':
			level = LOG_INFO;
			break;
		case 'W':
			level = LOG_WARNING;
			break;
		case 'E':
			level = LOG_ERROR;
			break;
		default:
			break;
		}
	}

	const size_t catEnd = level != 0 ? msg.find(']', 4) : std::string::npos;
	if (level != 0 && catEnd != std::string::npos && catEnd > 4) {
		const std::string cat = msg.substr(4, catEnd - 4);
		// Apply the category filter host-side: the web logger gates only on the
		// coarse DEBUG flag, so a filtered spec (BRAIDCAST_DEBUG_COMPONENTS=preview) must drop
		// the other categories' debug lines here. Info/warn/error always pass --
		// they are not part of the gated channel.
		LogCat parsed{};
		if (level == LOG_DEBUG && LogCatFromName(cat, parsed) && !Log::DebugEnabled(parsed)) {
			return false;
		}
		std::string rest = msg.substr(catEnd + 1);
		if (!rest.empty() && rest[0] == ' ') {
			rest.erase(0, 1);
		}
		blog(level, "[%s] %s%s", cat.c_str(), rest.c_str(), loc.c_str());
	} else {
		blog(LOG_INFO, "[cef] %s%s", msg.c_str(), loc.c_str());
	}
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
		// A completed main-frame load proves the renderer composited this run, so the
		// GPU path is healthy: clear the boot sentinel a crash-looping GPU subprocess
		// would otherwise leave behind for next-boot fallback detection.
		GpuSafeMode::NotePaintSuccess();
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

void Client::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status, int /*error_code*/,
				       const CefString & /*error_string*/)
{
	CEF_REQUIRE_UI_THREAD();

	if (message_router_) {
		message_router_->OnRenderProcessTerminated(browser);
	}

	// Without a reload the window keeps its last painted frame but is inert: no JS,
	// no timers, no bridge subscriptions, and nothing on screen says so. A detached
	// dock in that state reads as live data that simply stopped changing. The retry is
	// rate-bounded, not lifetime-capped: at most kMaxRendererReloads reloads per
	// kRendererCrashWindow, so a crash that reproduces immediately settles as one dead
	// window, while one that only recurs after the window keeps recovering at that rate.
	RendererCrash &crash = g_rendererCrashes[browser->GetIdentifier()];
	const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	if (crash.last != std::chrono::steady_clock::time_point{} && now - crash.last > kRendererCrashWindow) {
		crash.count = 0;
	}
	crash.last = now;
	crash.count++;

	const std::string detail =
		"[cef] render process terminated (status=" + std::to_string(static_cast<int>(status)) + ", attempt " +
		std::to_string(crash.count) + "/" + std::to_string(kMaxRendererReloads) + ")";
	if (crash.count > kMaxRendererReloads) {
		HostLog(detail + " -- giving up, leaving the window inert");
		return;
	}
	HostLog(detail + " -- reloading browser");
	browser->Reload();
}

void Client::OnBeforeContextMenu(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> /*frame*/,
				 CefRefPtr<CefContextMenuParams> /*params*/, CefRefPtr<CefMenuModel> model)
{
	CEF_REQUIRE_UI_THREAD();
	// Drop every default entry (Back/Reload/Print/View source/Inspect) so a right-click
	// on the UI does nothing browser-y; the web app renders its own menus in the DOM.
	model->Clear();
}

bool Client::OnPreKeyEvent(CefRefPtr<CefBrowser> /*browser*/, const CefKeyEvent &event, CefEventHandle /*os_event*/,
			   bool * /*is_keyboard_shortcut*/)
{
	CEF_REQUIRE_UI_THREAD();

	if (event.type != KEYEVENT_RAWKEYDOWN && event.type != KEYEVENT_KEYDOWN) {
		return false;
	}

	const bool ctrl = (event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0;
	const bool alt = (event.modifiers & EVENTFLAG_ALT_DOWN) != 0;
	const int key = event.windows_key_code;

	// Ctrl-combos that are pure browser chrome in a frameless desktop app: reload,
	// print, find / find-next, view-source, and the three zoom keys (+, -, 0 for both
	// the main row and the numpad). None carry app meaning, so consume them.
	if (ctrl) {
		switch (key) {
		case 'R':
		case 'P':
		case 'F':
		case 'G':
		case 'U':
		case '0':
		case VK_OEM_PLUS:
		case VK_OEM_MINUS:
		case VK_ADD:
		case VK_SUBTRACT:
			return true;
		default:
			break;
		}
	}

	// F5 reload (with or without Ctrl) and Alt+Left/Right history navigation would
	// tear down or navigate away from the single-page app; swallow them too.
	if (key == VK_F5) {
		return true;
	}
	if (alt && (key == VK_LEFT || key == VK_RIGHT)) {
		return true;
	}

	return false;
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
