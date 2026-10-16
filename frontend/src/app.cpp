#include "app.hpp"

#include "include/cef_command_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_request_context.h"
#include "include/cef_request_handler.h"
#include "include/cef_scheme.h"
#include "include/cef_values.h"
#include "include/cef_version.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <string>

#include "devtools_port.hpp"
#include "log.hpp"
#include "scheme.hpp"
#include "util/string_util.hpp"

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

namespace {

// The switches that can stand up a Chrome DevTools Protocol endpoint, or widen what
// may talk to one. Chromium builds its CommandLine from GetCommandLineW() -- on
// Windows CefMainArgs carries only an HINSTANCE, so the process's own argv reaches
// it whether or not this app ever looks at argv -- which would otherwise let
// `braidcast.exe --remote-debugging-port=N` open a listener the environment gate
// never authorized, while the mandatory UI indicator (which reads the gate) reports
// no port at all.
constexpr const char *kDebugPortSwitch = "remote-debugging-port";
constexpr const char *kDebuggingSwitches[] = {kDebugPortSwitch, "remote-debugging-pipe", "remote-debugging-targets",
					      "remote-allow-origins"};

bool IsDebuggingSwitch(const std::string &name)
{
	return std::any_of(std::begin(kDebuggingSwitches), std::end(kDebuggingSwitches),
			   [&name](const char *known) { return StringUtil::EqualsCI(name, known); });
}

// Whether a stripped switch is CEF handing back the port this app itself put in
// CefSettings: the setting is converted into a command-line switch before this
// callback runs, so every authorized launch carries one and warning about it would
// train the one warning that must still mean something into background noise. The
// other three are never set by us, so their presence is always foreign.
bool IsOwnDebugPort(const std::string &name, const std::string &value, uint16_t authorized)
{
	if (authorized == 0 || !StringUtil::EqualsCI(name, kDebugPortSwitch)) {
		return false;
	}
	char *end = nullptr;
	const long parsed = std::strtol(value.c_str(), &end, 10);
	return end != value.c_str() && *end == '\0' && parsed == static_cast<long>(authorized);
}

// CEF 6613 (Chromium 128) removed the Alloy bootstrap, so every build from there on runs
// Chrome's: a Chrome profile with Chrome's preferences, and Chrome-style windows that its
// own UI can open. Older builds run the Alloy bootstrap (CefSettings.chrome_runtime left
// at 0), which registers only a few of these preferences -- printing.enabled among them,
// so applying the lockdown there would change behaviour rather than harden it.
constexpr bool kChromeBootstrap = CHROME_VERSION_BUILD >= 6613;

// Comma-separated list switches that merge with any value already on the command line
// instead of replacing it: a second disable-features switch would otherwise drop every
// feature the first one named.
struct ListSwitch {
	const char *name;
	const char *values;
};
constexpr ListSwitch kMergedListSwitches[] = {
	// obs-browser's BrowserApp sets this policy (browser-app.cpp), but BrowserApp never
	// runs because the frontend owns CEF, so the frontend carries a superset of it. The
	// features keep native-window occlusion from throttling hidden pages, keep media
	// keys, gamepad polling, Bluetooth, Cast discovery, live captions, the global
	// shortcuts portal and the low-disk-space notice (CEF issue 3966) away from browser
	// sources, and keep WebSockets to loopback or LAN hosts out of Local Network Access
	// checks, so a widget's connection is not held behind a prompt nobody answers.
	{"disable-features", "CalculateNativeWinOcclusion,GlobalShortcutsPortal,HardwareMediaKeyHandling,LiveCaption,"
			     "LocalNetworkAccessChecksWebSockets,MediaRouter,StorageNotificationService,WebBluetooth,"
			     "EnableWindowsGamingInputDataFetcher"},
	// Document Picture-in-Picture opens a separate always-on-top window from a page.
	{"disable-blink-features", "DocumentPictureInPictureAPI"},
};

// Index into kMergedListSwitches, or its size when the name is not one of them.
size_t MergedListSwitchIndex(const std::string &name)
{
	const auto found =
		std::find_if(std::begin(kMergedListSwitches), std::end(kMergedListSwitches),
			     [&name](const ListSwitch &list) { return StringUtil::EqualsCI(name, list.name); });
	return static_cast<size_t>(std::distance(std::begin(kMergedListSwitches), found));
}

// Chrome profile preferences that turn off browser features nothing here uses: autofill,
// sign-in, payments, printing, search suggestions, translation, extensions. Names are
// Chromium's (chrome/common/pref_names.h) and each is registered in the bundled Chromium;
// the list is obs-browser's Chrome-runtime lockdown without the names Chromium no longer
// registers, with one difference: upstream's "translate" is not a registered preference, so
// translation is turned off through "translate.enabled", the name Chromium does register.
struct ChromePreference {
	const char *name;
	bool value;
};
constexpr ChromePreference kChromePreferences[] = {
	{"autofill.credit_card_enabled", false},
	{"autofill.enabled", false},
	{"autofill.payment_card_benefits", false},
	{"autofill.payment_cvc_storage", false},
	{"autofill.profile_enabled", false},
	{"browser_labs_enabled", false},
	{"credentials_enable_autosignin", false},
	{"credentials_enable_service", false},
	{"payments.can_make_payment_enabled", false},
	{"printing.enabled", false},
	{"search.suggest_enabled", false},
	{"shopping_list_enabled", false},
	{"side_panel.google_search_side_panel_enabled", false},
	{"side_search.enabled", false},
	{"signin.allowed", false},
	{"signin.allowed_on_next_startup", false},
	{"translate.enabled", false},
	{"url_keyed_anonymized_data_collection.enabled", false},
	{"extensions.block_external_extensions", true},
	{"extensions.disabled", true},
};

// Applies kChromePreferences to the global request context. A preference Chromium
// refuses is reported rather than fatal: the lockdown is hardening, not a precondition.
// Browser-process UI thread only (CefPreferenceManager::SetPreference).
void LockDownChromePreferences()
{
	// BootWarn, not blog: with multi_threaded_message_loop off, CEF calls
	// OnContextInitialized on the main thread before CefInitialize returns, so the
	// session-log handler (ObsBootstrap::Start) is not installed yet.
	CefRefPtr<CefRequestContext> context = CefRequestContext::GetGlobalContext();
	if (!context) {
		Log::BootWarn("[cef] no global request context; Chrome preferences left at their defaults");
		return;
	}

	std::string failures;
	for (const ChromePreference &pref : kChromePreferences) {
		CefRefPtr<CefValue> value = CefValue::Create();
		value->SetBool(pref.value);
		CefString error;
		if (!context->SetPreference(pref.name, value, error)) {
			if (!failures.empty()) {
				failures += "; ";
			}
			failures += std::string(pref.name) + " (" + error.ToString() + ")";
		}
	}
	if (!failures.empty()) {
		Log::BootWarn("[cef] could not set Chrome preference(s): " + failures);
	}
}

// The client for every browser Chrome's own UI creates (a new window or tab opened from
// a Chrome-style surface). Without one that browser is unmanaged: no callbacks reach the
// app, and shutdown blocks until someone closes it by hand. Every real window here is
// created by the host with the shared Client, so this one closes whatever it is handed,
// refuses navigation and any further popup, tab or window meanwhile, and hides Chrome's
// menus and toolbar. Only the Chrome bootstrap ever asks for it
// (CefBrowserProcessHandler::GetDefaultClient).
class UnmanagedBrowserClient : public CefClient,
			       public CefCommandHandler,
			       public CefLifeSpanHandler,
			       public CefRequestHandler {
public:
	CefRefPtr<CefCommandHandler> GetCommandHandler() override { return this; }
	CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
	CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

	bool OnChromeCommand(CefRefPtr<CefBrowser>, int, cef_window_open_disposition_t) override { return true; }
	bool IsChromeAppMenuItemVisible(CefRefPtr<CefBrowser>, int) override { return false; }
	bool IsChromeAppMenuItemEnabled(CefRefPtr<CefBrowser>, int) override { return false; }
	bool IsChromePageActionIconVisible(cef_chrome_page_action_icon_type_t) override { return false; }
	bool IsChromeToolbarButtonVisible(cef_chrome_toolbar_button_type_t) override { return false; }

	void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
	{
		HostLog("[cef] closing a browser window Chrome opened outside the host");
		browser->GetHost()->CloseBrowser(false);
	}

	bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
#if CHROME_VERSION_BUILD >= 6834
			   int,
#endif
			   const CefString &, const CefString &, CefLifeSpanHandler::WindowOpenDisposition, bool,
			   const CefPopupFeatures &, CefWindowInfo &, CefRefPtr<CefClient> &, CefBrowserSettings &,
			   CefRefPtr<CefDictionaryValue> &, bool *) override
	{
		return true;
	}

	bool OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>, bool, bool) override
	{
		return true;
	}

	bool OnOpenURLFromTab(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, const CefString &,
			      CefRequestHandler::WindowOpenDisposition, bool) override
	{
		return true;
	}

private:
	IMPLEMENT_REFCOUNTING(UnmanagedBrowserClient);
};

} // namespace

void App::OnBeforeCommandLineProcessing(const CefString &process_type, CefRefPtr<CefCommandLine> command_line)
{
	// Only customize the browser process (empty type). Chromium propagates these
	// decisions to the GPU/renderer subprocesses it spawns, so they need not (and
	// cannot reliably, this early) be appended per subprocess.
	if (!process_type.empty()) {
		return;
	}

	// Rebuild the command line without the debugging switches. Reset() drops every
	// switch CEF derived from CefSettings too (log file, resource paths, sandbox),
	// so the snapshot is re-appended verbatim minus the four and the merged list
	// switches; the gate's own port is re-appended below, after the strip, so it is the
	// last word either way.
	CefCommandLine::SwitchMap switches;
	command_line->GetSwitches(switches);
	CefCommandLine::ArgumentList arguments;
	command_line->GetArguments(arguments);

	command_line->Reset();

	// Values the launch already carried for the merged list switches, re-appended
	// below together with ours rather than verbatim here.
	std::string inheritedLists[std::size(kMergedListSwitches)];

	const uint16_t devToolsPort = DevToolsPort::Active();

	// Only switches that did NOT come from our own CefSettings are worth reporting.
	// GetSwitches() hands back a map, so a hostile --remote-debugging-port sharing the
	// name with CEF's settings-derived one collapses to a single entry and we cannot
	// tell which value survived; that is tolerable because correctness here rests on
	// the re-append below being last, not on this classification.
	std::string foreign;
	for (const auto &entry : switches) {
		const std::string name = entry.first.ToString();
		if (IsDebuggingSwitch(name)) {
			if (!IsOwnDebugPort(name, entry.second.ToString(), devToolsPort)) {
				if (!foreign.empty()) {
					foreign += ", ";
				}
				foreign += name;
			}
			continue;
		}
		if (const size_t list = MergedListSwitchIndex(name); list < std::size(kMergedListSwitches)) {
			inheritedLists[list] = entry.second.ToString();
			continue;
		}
		if (entry.second.empty()) {
			command_line->AppendSwitch(entry.first);
		} else {
			command_line->AppendSwitchWithValue(entry.first, entry.second);
		}
	}
	for (const CefString &argument : arguments) {
		command_line->AppendArgument(argument);
	}

	// Deferred as well as immediate: this runs inside CefInitialize, well before
	// ObsBootstrap::Start() installs the session-log handler, so an immediate blog()
	// alone would never reach the log file. Not behind the debug mask either -- a
	// release build with debugging off is exactly the launch this refusal matters on.
	if (!foreign.empty()) {
		Log::BootWarn("[devtools] ignoring remote-debugging command-line switch(es): " + foreign +
			      ". The debug port is controlled only by BRAIDCAST_DEBUG and "
			      "BRAIDCAST_DEBUG_COMPONENTS naming 'devtools'; naming it on the command line "
			      "cannot open it.");
	}

	if (devToolsPort != 0) {
		command_line->AppendSwitchWithValue(kDebugPortSwitch, std::to_string(devToolsPort));
	}

	// Keep rasterizing and presenting the UI when the host window is occluded or
	// backgrounded (the user alt-tabs to the game while live). At Chromium defaults
	// the renderer's memory/raster budget is cut and NativeWinOcclusion blanks the
	// composited output; under the GPU load of the live encoders that starves the
	// tile rasterizer, leaving black/stale tiles across the UI until a resize forces
	// a re-raster.
	command_line->AppendSwitch("disable-renderer-backgrounding");
	command_line->AppendSwitch("disable-backgrounding-occluded-windows");
	// Backgrounded renderers also get their timers clamped to ~1/minute, which stalls
	// any interval-driven UI in a detached dock window the user isn't looking at.
	command_line->AppendSwitch("disable-background-timer-throttling");
	for (size_t i = 0; i < std::size(kMergedListSwitches); ++i) {
		std::string value = inheritedLists[i];
		if (!value.empty()) {
			value += ',';
		}
		value += kMergedListSwitches[i].values;
		command_line->AppendSwitchWithValue(kMergedListSwitches[i].name, value);
	}
	// Also obs-browser's: lets an overlay play sound without the user gesture an
	// unattended page never receives.
	command_line->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
	// Chrome bootstrap only. Under Alloy, --disable-extensions also turns off the PDF
	// viewer, and the crash-restore bubble is Chrome UI that Alloy never shows.
	if (kChromeBootstrap) {
		command_line->AppendSwitch("disable-extensions");
		command_line->AppendSwitch("hide-crash-restore-bubble");
	}

	// --disable-gpu stops the CEF GPU-subprocess crash loop (EXCEPTION_BREAKPOINT) on a
	// machine whose GPU process cannot run; compositing then runs via SwiftShader.
	if (!software_mode_.load()) {
		return;
	}
	command_line->AppendSwitch("disable-gpu");
	command_line->AppendSwitch("disable-gpu-compositing");
}

void App::OnContextInitialized()
{
	// Factory registration must happen in the browser process after init.
	RegisterAppSchemeHandlerFactory();

	default_client_ = new UnmanagedBrowserClient();

	if (kChromeBootstrap) {
		LockDownChromePreferences();
	}
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
