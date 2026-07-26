#ifndef OBS_MULTISTREAM_FRONTEND_INNERTUBE_CLIENT_HPP_
#define OBS_MULTISTREAM_FRONTEND_INNERTUBE_CLIENT_HPP_

#include <chrono>
#include <functional>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

// The anonymous InnerTube client: youtube.com's own youtubei/v1 endpoints, read exactly as a
// logged-out web client reads them. THE SINGLE PLACE an InnerTube request is built and sent --
// the live-chat read (chat/youtube_innertube) and the concurrent-viewer read
// (oauth/youtube_provider) both go through Post() below, so the declared client identity, the
// process-wide rate ceiling and the compliance rule below hold for every one of them at once
// rather than per consumer.
//
// WHY it exists: the official Data API answers for these reads bill against ONE Cloud
// project's 10,000-unit daily budget shared by every install -- liveChatMessages.list alone
// cost a four-destination streamer roughly 166,000 units a day, and a videos.list viewer poll
// every 20s another 8,640. These endpoints cost ZERO quota.
//
// It lives in util/ rather than under either consumer because both need it and neither owns
// it: chat/ already depends on oauth/provider.hpp, so hanging the client off chat/ would put a
// cycle between the two. Both depending on util/ -- where http_client and json_util already
// live -- is the layering that holds.
//
// COMPLIANCE, absolute and now enforced at ONE request site (see Post()): every request is
// ANONYMOUS. No OAuth token, no Authorization header, no API key, no ?key=, no cookies.
namespace InnerTube {

using json = nlohmann::json;

// Process-wide ceiling on InnerTube request rate, shared by every reader in the process.
// Each reader already paces itself, but a user with many live destinations would otherwise
// multiply that pacing by the destination count and present a traffic shape no ordinary
// client has. A token bucket rather than a per-reader delay because the quantity to bound is
// the AGGREGATE: the readers are independent workers that never coordinate, so the only place
// the total exists is here.
//
// Thread-safe by contract -- readers run on separate workers and block in Acquire().
class RateLimiter {
public:
	RateLimiter(double ratePerSecond, double burst);

	// Consume one token, sleeping until one is available. Returns false when `canceled`
	// turned true during the wait (nothing was consumed); true once a token was taken.
	bool Acquire(const std::function<bool()> &canceled);

private:
	std::mutex mutex_;
	const double ratePerSecond_;
	const double burst_;
	double tokens_;
	std::chrono::steady_clock::time_point last_;
};

// The ONE limiter every InnerTube request in this process passes through. Post() acquires
// from it, so no consumer can opt out of the aggregate cap by forgetting to.
RateLimiter &SharedRateLimiter();

// The browser this app claims to be on every reverse-engineered endpoint it reads -- InnerTube
// here, and Kick's unofficial /api/v2 lookup, whose research flags UA fingerprinting on that
// path. ONE definition so the two cannot drift into claiming different browsers, and derived
// from the CEF this app actually embeds rather than hand-written (see the .cpp).
const char *BrowserUserAgent();

// One anonymous POST's outcome. Deliberately NOT collapsed into a bool: the consumers react
// differently to a transport failure (says nothing about the endpoint -- retry), a non-2xx
// (the surface may have stopped working) and a 2xx whose payload lacks the object they wanted,
// so all three have to stay distinguishable from here.
struct Result {
	// HTTP status, or 0 when the transport itself failed (`error` then carries why).
	long status = 0;
	std::string error;

	// The parsed 2xx payload; a null json for a non-2xx or an unparseable body, which the
	// JsonUtil accessors read through without a per-hop null check.
	json body;

	// The rate-limiter wait was interrupted by `canceled` -- NOTHING was sent, so this is
	// neither a failure nor any verdict about the endpoint.
	bool canceled = false;

	// The request produced a usable payload. A consumer that has to tell a transport failure
	// from a non-2xx reads `status` directly instead.
	bool ok() const { return status >= 200 && status < 300 && body.is_object(); }
};

// POST `fields` to the absolute youtubei/v1 `url`, as a logged-out web client: the client
// context block is merged in here, the browser headers are set here, and the shared rate
// ceiling is acquired here. `canceled` interrupts the rate-limiter wait; an empty function
// means there is no cancellation to honor.
Result Post(const char *url, const json &fields, const std::function<bool()> &canceled = {});

} // namespace InnerTube

#endif // OBS_MULTISTREAM_FRONTEND_INNERTUBE_CLIENT_HPP_
