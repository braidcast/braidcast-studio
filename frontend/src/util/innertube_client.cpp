#include "innertube_client.hpp"

#include <algorithm>
#include <string>

#include "include/cef_version.h"

#include "../chat/ws_client.hpp" // CancelableSleep -- the shared sliced, cancel-honoring wait
#include "../log.hpp"
#include "env_config.hpp"
#include "http_client.hpp"
#include "json_util.hpp"

namespace InnerTube {

namespace {

#define BRAIDCAST_INNERTUBE_STR2(x) #x
#define BRAIDCAST_INNERTUBE_STR(x) BRAIDCAST_INNERTUBE_STR2(x)

// The client identity every request below declares. THE TWO MUST STAY PLAUSIBLE TOGETHER,
// which is why they sit adjacent:
//
//   - The User-Agent is derived from the Chromium version of the CEF this app actually
//     embeds (cef_version.h), reduced to Chrome's own MAJOR.0.0.0 form. Built from the
//     dependency rather than hand-written so it tracks a CEF bump instead of decaying into
//     a fixed fleet-wide string that claims a Chrome nobody runs any more. Compile-time
//     because a runtime read would mean plumbing the browser host into a worker thread.
//   - clientVersion is the InnerTube WEB client's OWN version, unrelated to the Chromium
//     version -- do not conflate them. Pinned rather than scraped out of a watch page (the
//     HTML dependency this client exists to avoid); InnerTube accepts an aging value for
//     years, and the env override exists for the day it stops.
constexpr const char *kUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
				   "Chrome/" BRAIDCAST_INNERTUBE_STR(CHROME_VERSION_MAJOR) ".0.0.0 Safari/537.36";
constexpr const char *kClientName = "WEB";
constexpr const char *kDefaultClientVersion = "2.20240814.00.00";

// Aggregate ceiling across every reader in the process. At the chat read's 2s active floor a
// reader draws 0.5 req/s, so this leaves eight simultaneous live destinations completely
// unthrottled (4.0 req/s) with headroom for their bootstraps and for the far cheaper 20s
// viewer poll, and only a pathological destination count is paced -- at which point the
// throttle simply stretches each reader's cadence rather than dropping anything.
constexpr double kSharedRatePerSecond = 4.0;
constexpr double kSharedBurst = 8.0;

constexpr int kRequestTimeoutSec = 15;

// Pinned at compile time; overridable without a rebuild for the day InnerTube starts
// rejecting an aged WEB version.
const std::string &ClientVersion()
{
	static const std::string version =
		Env::Value("BRAIDCAST_YOUTUBE_INNERTUBE_CLIENT_VERSION", kDefaultClientVersion);
	return version;
}

// The InnerTube request context: the identity a logged-out web client declares. No `user`
// block, no visitor data, no credential of any kind. hl/gl pin the response language and
// region, so a server-side locale guess cannot re-localize a field a caller parses.
json ClientContext()
{
	return json{{"client", json{{"clientName", kClientName},
				    {"clientVersion", ClientVersion()},
				    {"hl", "en"},
				    {"gl", "US"}}}};
}

} // namespace

RateLimiter::RateLimiter(double ratePerSecond, double burst)
	: ratePerSecond_(ratePerSecond),
	  burst_(burst),
	  tokens_(burst),
	  last_(std::chrono::steady_clock::now())
{
}

bool RateLimiter::Acquire(const std::function<bool()> &canceled)
{
	// Loop rather than sleep-once: the wait is computed under the lock and released before
	// sleeping, so another reader may take the refilled token first and this one recomputes.
	while (!canceled()) {
		std::chrono::milliseconds wait{};
		{
			const std::lock_guard<std::mutex> guard(mutex_);
			const auto now = std::chrono::steady_clock::now();
			const double elapsed = std::chrono::duration<double>(now - last_).count();
			last_ = now;
			tokens_ = std::min(burst_, tokens_ + elapsed * ratePerSecond_);
			if (tokens_ >= 1.0) {
				tokens_ -= 1.0;
				return true;
			}
			wait = std::chrono::milliseconds(
				static_cast<long long>((1.0 - tokens_) / ratePerSecond_ * 1000.0) + 1);
		}
		if (Chat::CancelableSleep(wait, canceled)) {
			return false;
		}
	}
	return false;
}

RateLimiter &SharedRateLimiter()
{
	static RateLimiter limiter(kSharedRatePerSecond, kSharedBurst);
	return limiter;
}

const char *BrowserUserAgent()
{
	return kUserAgent;
}

// Post one InnerTube call.
//
// DELIBERATELY NOT YouTubeProvider::SendAuthed / SendAuthedStreaming. Those attach the
// account's OAuth token and refresh it on a 401, and these endpoints must be read ANONYMOUSLY
// -- as a logged-out web client, with no Authorization header, no API key, no ?key=, and no
// cookies. Attaching a credential to a reverse-engineered surface is what turns an unofficial
// read into a risk to the user's account, so this goes straight through Http::HttpRequest and
// must stay that way. Do not "fix" it back onto the provider helpers.
Result Post(const char *url, const json &fields, const std::function<bool()> &canceled)
{
	Result out;

	// An absent predicate means "no cancellation to honor", normalized once here so no call
	// site on a thread without a cancel flag has to hand in an always-false lambda of its own.
	static const std::function<bool()> kNeverCanceled = [] { return false; };
	const std::function<bool()> &cancel = canceled ? canceled : kNeverCanceled;

	if (!SharedRateLimiter().Acquire(cancel)) {
		out.canceled = true;
		return out;
	}

	// The caller's fields plus the client identity. Assigning the context last means a caller
	// cannot accidentally ship a context of its own.
	json body = fields;
	body["context"] = ClientContext();

	Http::HttpReq req;
	req.method = "POST";
	req.url = url;
	req.contentType = "application/json";
	req.body = body.dump();
	req.timeoutSec = kRequestTimeoutSec;
	req.headers.push_back(std::string("User-Agent: ") + kUserAgent);
	req.headers.push_back("X-YouTube-Client-Name: 1"); // 1 = WEB, the numeric form of kClientName
	req.headers.push_back("X-YouTube-Client-Version: " + ClientVersion());
	req.headers.push_back("Accept-Language: en-US,en;q=0.9");

	const Http::HttpResponse resp = Http::HttpRequest(req);
	out.status = resp.status;
	out.error = resp.error;
	if (resp.status >= 200 && resp.status < 300) {
		out.body = JsonUtil::ParseJson(resp.body);
	}
	// Response size is logged because it is a load-bearing quantity here, not trivia: the
	// continuation form of a read costs a twentieth of the videoId form, and a regression
	// back onto the expensive form is otherwise invisible.
	DBG(LogCat::Net, "innertube: POST %s -> HTTP %ld, %zu bytes in (%s)", url, resp.status, resp.body.size(),
	    resp.error.empty() ? "no transport error" : resp.error.c_str());
	return out;
}

} // namespace InnerTube
