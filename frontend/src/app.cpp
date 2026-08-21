#include "app.hpp"

#include "include/cef_scheme.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "devtools_port.hpp"
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
	// so the snapshot is re-appended verbatim minus the four; the gate's own port is
	// re-appended below, after the strip, so it is the last word either way.
	CefCommandLine::SwitchMap switches;
	command_line->GetSwitches(switches);
	CefCommandLine::ArgumentList arguments;
	command_line->GetArguments(arguments);

	command_line->Reset();

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
		DevToolsPort::Warn("[devtools] ignoring remote-debugging command-line switch(es): " + foreign +
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
	command_line->AppendSwitchWithValue("disable-features", "CalculateNativeWinOcclusion");

	// --disable-gpu stops the CEF GPU-subprocess crash loop (EXCEPTION_BREAKPOINT on
	// hardware newer than this libcef); compositing then runs via SwiftShader.
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
