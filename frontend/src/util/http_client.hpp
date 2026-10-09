#ifndef OBS_MULTISTREAM_FRONTEND_HTTP_CLIENT_HPP_
#define OBS_MULTISTREAM_FRONTEND_HTTP_CLIENT_HPP_

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Minimal synchronous HTTP client for the bridge's OAuth + platform-API calls
// (Phase 8a). Backed by libcurl. HttpRequest blocks until the transfer
// completes, so callers MUST run it off the CEF UI thread (see AsyncTask).
//
// Thread-safety: HttpRequest holds no shared mutable state -- each call owns its
// own easy handle -- and curl_global_init runs exactly once behind a
// std::once_flag, so concurrent calls from worker threads are safe.
namespace Http {

struct HttpReq {
	std::string method;               // "GET" / "POST" / "PATCH" / "PUT"
	std::string url;                  // absolute URL
	std::vector<std::string> headers; // each entry "Key: Value"
	std::string body;                 // request body (POST/PATCH/PUT)
	std::string contentType;          // sets Content-Type when non-empty
	int timeoutSec = 30;              // whole-request timeout
	// Follow up to 5 HTTPS redirects. Off by default: OAuth flows must observe the 3xx
	// Location themselves. A static-file download opts in (Hugging Face redirects every
	// model file to its CDN).
	bool followRedirects = false;
	// Polled about once a second for the whole transfer, connect included; true aborts it.
	// A stalled connect is otherwise uncancellable until its timeout. On the streaming
	// path it stops the transfer the same clean way `canceled` does; HttpRequest reports
	// it as a transport failure with `error` "cancelled".
	const std::atomic<bool> *cancel = nullptr;
};

struct HttpResponse {
	long status = 0;   // HTTP status code (0 if the transport failed)
	std::string body;  // response body
	std::string error; // transport error string; empty on success
};

// Perform a blocking request. On a transport-level failure `error` is set and
// `status` is 0; an HTTP error (4xx/5xx) is NOT an error here -- `status`
// carries the code and `error` stays empty. Redirects are not
// followed unless req.followRedirects is set.
HttpResponse HttpRequest(const HttpReq &req);

// Streaming GET/POST: response body bytes are handed to `onChunk` as they arrive over
// the wire, so a long-lived server-push stream (YouTube liveChatMessages.streamList)
// surfaces each pushed batch with ~1s latency instead of blocking until the connection
// closes. `onChunk` returns false to abort the transfer (cancellation), which is
// reported as a clean stop -- NOT a transport error. Returns the HTTP status (0 on a
// transport failure, with `error` set).
//
// Only a 2xx body is streamed to `onChunk`; for a non-2xx response the body is captured
// into `errorBody` instead (so the caller can inspect the failure reason, e.g. a 403
// quotaExceeded), and `onChunk` is never invoked. `timeoutSec` applies to CONNECT, not
// the whole transfer -- a streaming connection is meant to stay open -- while a stalled
// (dead but not closed) connection is detected by a low-speed watchdog.
//
// `canceled`, when set, is polled about once a second whether or not bytes arrive, and a
// true stops the transfer the same clean way an `onChunk` false does. Without it a cancel
// is only seen at the next chunk, which on a silent stream is the watchdog's 90 s.
// req.cancel is the flag form of the same thing, polled by the same callback (connect
// included) and before each chunk; either may be set, or both.
//
// Same thread-safety contract as HttpRequest: each call owns its own easy handle.
long HttpRequestStreaming(const HttpReq &req, const std::function<bool(std::string_view chunk)> &onChunk,
			  std::string &errorBody, std::string &error, const std::function<bool()> &canceled = {});

// Percent-encode a string for application/x-www-form-urlencoded bodies and
// query parameters (RFC 3986 unreserved set kept literal).
std::string UrlEncode(const std::string &value);

// Append one `key=value` pair to an application/x-www-form-urlencoded body,
// inserting the `&` separator only when `body` already holds a pair. The single
// form-body builder for every caller that posts one (the OAuth broker's token /
// revoke calls, the Graph API's live-video calls).
inline void AppendForm(std::string &body, const char *key, const std::string &value)
{
	if (!body.empty()) {
		body += "&";
	}
	body += key;
	body += "=";
	body += UrlEncode(value);
}

// Reject a non-2xx response with the caller's label folded into `err`; a 2xx passes through.
inline bool Require2xx(const HttpResponse &resp, const char *label, std::string &err)
{
	if (resp.status < 200 || resp.status >= 300) {
		err = std::string(label) + " failed (HTTP " + std::to_string(resp.status) + "): " + resp.body;
		return false;
	}
	return true;
}

// A response body rendered as one log line. `(empty)` is deliberate rather than a blank:
// "the server explained nothing" and "we never recorded it" are different diagnoses, and a
// terminal error that prints neither leaves the next reader unable to tell them apart.
// Control characters are flattened so a multi-line payload cannot break the line it sits on.
inline std::string BodyForLog(const std::string &body, size_t max = 400)
{
	if (body.empty()) {
		return "(empty)";
	}
	std::string out = body.substr(0, max);
	for (char &c : out) {
		if (static_cast<unsigned char>(c) < 0x20) {
			c = ' ';
		}
	}
	if (body.size() > max) {
		out += "...(truncated)";
	}
	return out;
}

} // namespace Http

#endif // OBS_MULTISTREAM_FRONTEND_HTTP_CLIENT_HPP_
