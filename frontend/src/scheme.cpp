#include "scheme.hpp"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "include/cef_parser.h"
#include "include/cef_resource_handler.h"
#include "include/cef_scheme.h"
#include "include/wrapper/cef_helpers.h"

#include "paths.hpp"

namespace {

// Absolute path to the offline bundle root, under the shared rundir data dir.
std::string BundleRoot()
{
	return RundirRoot() + "/data/braidcast/web";
}

// Map a file extension to a MIME type. Anything unknown serves as octet-stream.
std::string ContentTypeForPath(const std::string &path)
{
	static const std::vector<std::pair<std::string, std::string>> kTypes = {
		{".html", "text/html"},        {".htm", "text/html"},        {".js", "text/javascript"},
		{".mjs", "text/javascript"},   {".css", "text/css"},         {".json", "application/json"},
		{".svg", "image/svg+xml"},     {".png", "image/png"},        {".jpg", "image/jpeg"},
		{".jpeg", "image/jpeg"},       {".gif", "image/gif"},        {".ico", "image/x-icon"},
		{".woff", "font/woff"},        {".woff2", "font/woff2"},     {".ttf", "font/ttf"},
		{".wasm", "application/wasm"}, {".map", "application/json"}, {".txt", "text/plain"},
	};

	size_t dot = path.find_last_of('.');
	if (dot != std::string::npos) {
		std::string ext = path.substr(dot);
		std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(::tolower(c)); });
		for (const auto &[suffix, type] : kTypes) {
			if (ext == suffix) {
				return type;
			}
		}
	}
	return "application/octet-stream";
}

// Resolve app://app/<path> to an absolute file path under the bundle root, then
// read it. Rejects any path that escapes the bundle (..). Returns false (-> 404)
// when the file is missing.
bool ReadBundleFile(const std::string &url_path, std::vector<char> &out, std::string &content_type)
{
	// Strip a leading slash and default the root to index.html.
	std::string rel = url_path;
	if (!rel.empty() && rel.front() == '/') {
		rel.erase(0, 1);
	}
	if (rel.empty()) {
		rel = "index.html";
	}

	// Reject path traversal outright; the bundle is fully self-contained.
	if (rel.find("..") != std::string::npos) {
		return false;
	}

	std::string full = BundleRoot() + "\\" + rel;
	for (char &c : full) {
		if (c == '/') {
			c = '\\';
		}
	}

	std::ifstream file(full, std::ios::binary);
	if (!file) {
		return false;
	}

	file.seekg(0, std::ios::end);
	std::streamoff size = file.tellg();
	file.seekg(0, std::ios::beg);
	out.resize(size > 0 ? size_t(size) : 0);
	if (size > 0) {
		file.read(out.data(), size);
		// A short read would otherwise advertise the full length and serve a
		// garbage tail; trim to what was actually read.
		out.resize(size_t(file.gcount()));
	}

	content_type = ContentTypeForPath(rel);
	return true;
}

// Serves a single in-memory file (or a 404) for one app:// request.
class AppResourceHandler : public CefResourceHandler {
public:
	bool Open(CefRefPtr<CefRequest> request, bool &handle_request, CefRefPtr<CefCallback> callback) override
	{
		CEF_REQUIRE_IO_THREAD();
		handle_request = true; // resolve synchronously below

		CefURLParts parts;
		CefParseURL(request->GetURL(), parts);
		const std::string path = CefString(&parts.path).ToString();

		found_ = ReadBundleFile(path, data_, content_type_);
		callback->Continue();
		return true;
	}

	void GetResponseHeaders(CefRefPtr<CefResponse> response, int64_t &response_length,
				CefString &redirect_url) override
	{
		CEF_REQUIRE_IO_THREAD();
		redirect_url.clear();

		if (!found_) {
			response->SetStatus(404);
			response->SetStatusText("Not Found");
			response->SetMimeType("text/plain");
			response_length = 0;
			return;
		}

		response->SetStatus(200);
		response->SetMimeType(content_type_);

		CefResponse::HeaderMap headers;
		response->GetHeaderMap(headers);
		headers.insert(std::make_pair("Access-Control-Allow-Origin", "*"));
		response->SetHeaderMap(headers);

		response_length = int64_t(data_.size());
	}

	bool Read(void *data_out, int bytes_to_read, int &bytes_read,
		  CefRefPtr<CefResourceReadCallback> callback) override
	{
		CEF_REQUIRE_IO_THREAD();
		bytes_read = 0;
		if (offset_ >= data_.size()) {
			return false; // done
		}

		size_t remaining = data_.size() - offset_;
		size_t to_copy = std::min(size_t(bytes_to_read), remaining);
		memcpy(data_out, data_.data() + offset_, to_copy);
		offset_ += to_copy;
		bytes_read = int(to_copy);
		return true;
	}

	void Cancel() override { CEF_REQUIRE_IO_THREAD(); }

private:
	bool found_ = false;
	std::vector<char> data_;
	std::string content_type_;
	size_t offset_ = 0;

	IMPLEMENT_REFCOUNTING(AppResourceHandler);
};

// Hands a fresh AppResourceHandler to CEF for every app:// request.
class AppSchemeHandlerFactory : public CefSchemeHandlerFactory {
public:
	CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, const CefString &,
					     CefRefPtr<CefRequest>) override
	{
		return new AppResourceHandler();
	}

	IMPLEMENT_REFCOUNTING(AppSchemeHandlerFactory);
};

} // namespace

void RegisterAppSchemeHandlerFactory()
{
	CefRegisterSchemeHandlerFactory(kAppScheme, kAppHost, new AppSchemeHandlerFactory());
}
