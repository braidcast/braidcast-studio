#include "http_client.hpp"

#include <mutex>
#include <string_view>

#include <curl/curl.h>

namespace Http {

namespace {

// curl_global_init is not thread-safe and must run once before any easy handle
// is used. Gate it behind a once_flag so the first HttpRequest from any thread
// initializes the library exactly once.
void EnsureGlobalInit()
{
	static std::once_flag once;
	std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// Ceiling on a response body, generous for the images/JSON API responses this client
// fetches (including third-party emote/badge hosts on the go-live chat path).
// CURLOPT_MAXFILESIZE_LARGE below rejects a response whose declared Content-Length
// exceeds this, but a chunked/gzip-bombed response can lie about or omit
// Content-Length entirely -- WriteToString enforces the same ceiling against the
// actual decompressed bytes as they arrive, so the cap holds either way.
constexpr curl_off_t kMaxResponseBytes = 20 * 1024 * 1024;

size_t WriteToString(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	const size_t total = size * nmemb;
	auto *out = static_cast<std::string *>(userdata);
	if (out->size() + total > static_cast<size_t>(kMaxResponseBytes)) {
		return 0; // short return -> curl aborts the transfer with CURLE_WRITE_ERROR
	}
	out->append(ptr, total);
	return total;
}

// Per-transfer state threaded into the streaming write callback: the easy handle (so
// the callback can read the response status once headers are in), the caller's chunk
// sink, and the error-body accumulator used for a non-2xx response.
struct StreamState {
	CURL *curl = nullptr;
	const std::function<bool(std::string_view)> *onChunk = nullptr;
	std::string *errorBody = nullptr;
	bool aborted = false; // onChunk requested cancellation
};

size_t WriteStreaming(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	const size_t total = size * nmemb;
	auto *st = static_cast<StreamState *>(userdata);

	// The status line + headers are parsed before the first write callback fires, so
	// CURLINFO_RESPONSE_CODE is populated here.
	long status = 0;
	curl_easy_getinfo(st->curl, CURLINFO_RESPONSE_CODE, &status);

	if (status < 200 || status >= 300) {
		// Non-2xx: capture the body (capped) for the caller to inspect instead of
		// streaming it. Keep draining so curl reads the whole error response cleanly.
		if (st->errorBody->size() + total <= static_cast<size_t>(kMaxResponseBytes)) {
			st->errorBody->append(ptr, total);
		}
		return total;
	}

	if (!(*st->onChunk)(std::string_view(ptr, total))) {
		// Caller-driven cancel: short return -> curl aborts with CURLE_WRITE_ERROR,
		// which the caller treats as a clean stop (see the `aborted` flag).
		st->aborted = true;
		return 0;
	}
	return total;
}

// Shared easy-handle setup for both the buffered and streaming paths: URL, verb,
// request headers, and the transport-safety options. The write callback and the
// timeout policy differ between the two and are set by each caller. `headerList` is
// filled with the allocated slist the caller must free after curl_easy_perform.
void ApplyCommonOptions(CURL *curl, const HttpReq &req, struct curl_slist *&headerList)
{
	for (const std::string &header : req.headers) {
		headerList = curl_slist_append(headerList, header.c_str());
	}
	if (!req.contentType.empty()) {
		const std::string ct = "Content-Type: " + req.contentType;
		headerList = curl_slist_append(headerList, ct.c_str());
	}

	curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());

	// HTTP verb. GET is the default; POST/PUT/PATCH go through CUSTOMREQUEST so a
	// PATCH (which libcurl has no dedicated option for) works identically to the
	// others, and POSTFIELDS supplies the body for whichever verb carries one.
	const std::string method = req.method.empty() ? "GET" : req.method;
	if (method != "GET") {
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
	}

	if (headerList) {
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
	}

	// Do NOT follow redirects: OAuth flows must observe the 3xx Location header
	// themselves rather than transparently chasing it.
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
	// NOSIGNAL is mandatory off the main thread: libcurl's default DNS-timeout
	// path uses SIGALRM, which is unsafe in a multithreaded process.
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
}

} // namespace

HttpResponse HttpRequest(const HttpReq &req)
{
	EnsureGlobalInit();

	HttpResponse resp;

	CURL *curl = curl_easy_init();
	if (!curl) {
		resp.error = "curl_easy_init failed";
		return resp;
	}

	struct curl_slist *headerList = nullptr;
	ApplyCommonOptions(curl, req, headerList);

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
	curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, kMaxResponseBytes);

	const long timeout = req.timeoutSec > 0 ? req.timeoutSec : 30;
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);

	const CURLcode code = curl_easy_perform(curl);
	if (code == CURLE_OK) {
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
	} else {
		resp.error = curl_easy_strerror(code);
	}

	if (headerList) {
		curl_slist_free_all(headerList);
	}
	curl_easy_cleanup(curl);
	return resp;
}

long HttpRequestStreaming(const HttpReq &req, const std::function<bool(std::string_view chunk)> &onChunk,
			  std::string &errorBody, std::string &error)
{
	EnsureGlobalInit();

	CURL *curl = curl_easy_init();
	if (!curl) {
		error = "curl_easy_init failed";
		return 0;
	}

	struct curl_slist *headerList = nullptr;
	ApplyCommonOptions(curl, req, headerList);

	StreamState st;
	st.curl = curl;
	st.onChunk = &onChunk;
	st.errorBody = &errorBody;

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteStreaming);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &st);

	// Connect timeout only -- the transfer is a long-lived push stream and must NOT be
	// killed by a whole-request timeout. A connected-but-dead stream (no bytes for a long
	// stretch) is caught by the low-speed watchdog instead.
	const long connectTimeout = req.timeoutSec > 0 ? req.timeoutSec : 30;
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connectTimeout);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 90L);

	const CURLcode code = curl_easy_perform(curl);

	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

	// A caller-driven cancel returns 0 from the write callback, surfacing as
	// CURLE_WRITE_ERROR. That is a clean stop, not a transport failure: leave `error`
	// empty and report whatever status was reached.
	if (code != CURLE_OK && !(code == CURLE_WRITE_ERROR && st.aborted)) {
		error = curl_easy_strerror(code);
	}

	if (headerList) {
		curl_slist_free_all(headerList);
	}
	curl_easy_cleanup(curl);
	return status;
}

std::string UrlEncode(const std::string &value)
{
	static const char hex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(value.size() * 3);
	for (unsigned char c : value) {
		const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
					c == '-' || c == '_' || c == '.' || c == '~';
		if (unreserved) {
			out.push_back(static_cast<char>(c));
		} else {
			out.push_back('%');
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 0x0F]);
		}
	}
	return out;
}

} // namespace Http
