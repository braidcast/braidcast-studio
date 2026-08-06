#ifndef OBS_MULTISTREAM_FRONTEND_WEB_BUNDLE_HPP_
#define OBS_MULTISTREAM_FRONTEND_WEB_BUNDLE_HPP_

#include <string>
#include <utility>
#include <vector>

#include "paths.hpp"
#include "string_util.hpp"

// The offline web bundle: where it lives on disk and how its files are typed. Both
// surfaces that serve it read from here -- the app:// scheme handler (scheme.cpp) and
// the overlay HTTP server (overlay/overlay_server.cpp) -- so a file type one of them
// learns about is served correctly by both. A per-server copy of the table lets the
// same asset come back as octet-stream from one surface and correctly typed from the
// other.
namespace WebBundle {

// Absolute path to the bundle root, under the shared rundir data dir.
inline std::string Root()
{
	return RundirRoot() + "/data/braidcast/web";
}

// Map a file extension to a MIME type. Anything unknown serves as octet-stream.
inline std::string ContentTypeForPath(const std::string &path)
{
	static const std::vector<std::pair<std::string, std::string>> kTypes = {
		{".html", "text/html"},        {".htm", "text/html"},        {".js", "text/javascript"},
		{".mjs", "text/javascript"},   {".css", "text/css"},         {".json", "application/json"},
		{".svg", "image/svg+xml"},     {".png", "image/png"},        {".jpg", "image/jpeg"},
		{".jpeg", "image/jpeg"},       {".gif", "image/gif"},        {".ico", "image/x-icon"},
		{".woff", "font/woff"},        {".woff2", "font/woff2"},     {".ttf", "font/ttf"},
		{".wasm", "application/wasm"}, {".map", "application/json"}, {".txt", "text/plain"},
		{".mp3", "audio/mpeg"},        {".ogg", "audio/ogg"},        {".wav", "audio/wav"},
		{".webp", "image/webp"},
	};

	size_t dot = path.find_last_of('.');
	if (dot != std::string::npos) {
		const std::string ext = StringUtil::ToLower(path.substr(dot));
		for (const auto &[suffix, type] : kTypes) {
			if (ext == suffix) {
				return type;
			}
		}
	}
	return "application/octet-stream";
}

} // namespace WebBundle

#endif // OBS_MULTISTREAM_FRONTEND_WEB_BUNDLE_HPP_
