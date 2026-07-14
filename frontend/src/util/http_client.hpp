#ifndef OBS_MULTISTREAM_FRONTEND_HTTP_CLIENT_HPP_
#define OBS_MULTISTREAM_FRONTEND_HTTP_CLIENT_HPP_

#include <string>
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
};

struct HttpResponse {
	long status = 0;   // HTTP status code (0 if the transport failed)
	std::string body;  // response body
	std::string error; // transport error string; empty on success
};

// Perform a blocking request. On a transport-level failure `error` is set and
// `status` is 0; an HTTP error (4xx/5xx) is NOT an error here -- `status`
// carries the code and `error` stays empty. Redirects are NOT followed (OAuth
// flows must observe 3xx Location themselves).
HttpResponse HttpRequest(const HttpReq &req);

// Percent-encode a string for application/x-www-form-urlencoded bodies and
// query parameters (RFC 3986 unreserved set kept literal).
std::string UrlEncode(const std::string &value);

} // namespace Http

#endif // OBS_MULTISTREAM_FRONTEND_HTTP_CLIENT_HPP_
