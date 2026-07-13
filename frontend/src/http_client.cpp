#include "http_client.hpp"

#include <mutex>

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

size_t WriteToString(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	const size_t total = size * nmemb;
	auto *out = static_cast<std::string *>(userdata);
	out->append(ptr, total);
	return total;
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

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);

	const long timeout = req.timeoutSec > 0 ? req.timeoutSec : 30;
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);

	// Do NOT follow redirects: OAuth flows must observe the 3xx Location header
	// themselves rather than transparently chasing it.
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
	// NOSIGNAL is mandatory off the main thread: libcurl's default DNS-timeout
	// path uses SIGALRM, which is unsafe in a multithreaded process.
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

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
