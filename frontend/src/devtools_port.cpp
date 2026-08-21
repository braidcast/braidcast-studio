#include "devtools_port.hpp"

#include <util/base.h>

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "log.hpp"
#include "util/env_config.hpp"
#include "util/string_util.hpp"

namespace {

// Not 9222: that is Chrome's own default, so a developer's browser and this app
// would fight over the same listener.
constexpr uint16_t kDefaultPort = 9333;

// cef_types.h on CefSettings.remote_debugging_port: "Set to a value between 1024
// and 65535 to enable remote debugging on the specified port." Anything outside
// that is not a port CEF will open, so it never reaches CefSettings.
//
// One below the documented ceiling on purpose: CefDevToolsManagerDelegate's socket
// factory has historically bounded the port exclusively (temp_port < 65535), so 65535
// is a value we would accept and CEF would then refuse -- an indicator claiming a port
// that never opened is the one failure this gate exists to prevent.
constexpr long kMinPort = 1024;
constexpr long kMaxPort = 65534;

constexpr const char *kPortVar = "BRAIDCAST_DEVTOOLS_PORT";

uint16_t g_activePort = 0;

// Every message Warn() has raised this process, in order, and never emptied: the vector
// doubles as the "already said this" record. That is what keeps Resolve()'s
// misconfiguration warning from landing twice, since the self-test resolves a second
// time after the flush, when an immediate blog() would reach the session log directly.
// Main thread only.
std::vector<std::string> g_warnings;
// Index of the first message FlushPendingWarnings() has not put on the session log, and
// whether that flush has happened at all -- Warn() needs the second to know whether its
// own immediate blog() already landed there.
size_t g_flushed = 0;
bool g_flushDone = false;

} // namespace

uint16_t DevToolsPort::Resolve()
{
	const std::optional<std::string> master = Env::Raw("BRAIDCAST_DEBUG");
	if (!master || !StringUtil::ParseBool(*master)) {
		return 0;
	}

	const std::optional<std::string> components = Env::Raw("BRAIDCAST_DEBUG_COMPONENTS");
	if (!components || !Log::ParseComponents(*components).devTools) {
		return 0;
	}

	if (const std::optional<std::string> raw = Env::Raw(kPortVar)) {
		// Full consumption, the same rule App::IsOwnDebugPort applies: strtol alone
		// stops at the first non-digit, so Env::Number would take "9333abc" as 9333 and
		// silently open a port nobody typed. Anything that is not exactly a number --
		// and a number CEF would accept -- lands on the default and says so.
		const std::string value = StringUtil::Trim(*raw);
		char *end = nullptr;
		const long requested = std::strtol(value.c_str(), &end, 10);
		const bool wellFormed = !value.empty() && *end == '\0';
		if (!wellFormed || requested < kMinPort || requested > kMaxPort) {
			Warn("[devtools] " + std::string(kPortVar) + "='" + *raw + "' is not a port in [" +
			     std::to_string(kMinPort) + ", " + std::to_string(kMaxPort) + "] -- using " +
			     std::to_string(kDefaultPort) + " instead");
		} else {
			return static_cast<uint16_t>(requested);
		}
	}
	return kDefaultPort;
}

uint16_t DevToolsPort::DecideAtBoot()
{
	g_activePort = Resolve();
	return g_activePort;
}

uint16_t DevToolsPort::Active()
{
	return g_activePort;
}

void DevToolsPort::Warn(const std::string &message)
{
	if (std::find(g_warnings.begin(), g_warnings.end(), message) != g_warnings.end()) {
		return;
	}
	blog(LOG_WARNING, "%s", message.c_str());
	g_warnings.push_back(message);
	// Past the flush, the blog() above already reaches the session log, so this message
	// is done -- leaving it behind the cursor would let a second flush repeat it.
	if (g_flushDone) {
		g_flushed = g_warnings.size();
	}
}

void DevToolsPort::FlushPendingWarnings()
{
	for (; g_flushed < g_warnings.size(); ++g_flushed) {
		blog(LOG_WARNING, "%s", g_warnings[g_flushed].c_str());
	}
	g_flushDone = true;
}
