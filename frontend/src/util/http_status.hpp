#ifndef OBS_MULTISTREAM_FRONTEND_HTTP_STATUS_HPP_
#define OBS_MULTISTREAM_FRONTEND_HTTP_STATUS_HPP_

// HTTP status -> reason phrase, shared by every responder in the app (the embedded
// MCP control server and the overlay server). One table rather than one per server,
// because a status only one of them returns today is exactly the case a per-server
// table gets wrong: the missing row silently degrades to "OK".
//
// Deliberately dependency-free (no standard headers, no libobs) so the libobs-free
// mcp/HttpServer.cpp can include it.
namespace Http {

struct ReasonPhrase {
	int status;
	const char *phrase;
};

// Size-deduced so adding a status is a one-line edit.
inline constexpr ReasonPhrase kReasons[] = {
	{200, "OK"},
	{202, "Accepted"},
	{400, "Bad Request"},
	{401, "Unauthorized"},
	{403, "Forbidden"},
	{404, "Not Found"},
	{405, "Method Not Allowed"},
	{413, "Payload Too Large"},
	{431, "Request Header Fields Too Large"},
	{500, "Internal Server Error"},
	{503, "Service Unavailable"},
	{504, "Gateway Timeout"},
};

inline const char *ReasonFor(int status)
{
	for (const ReasonPhrase &r : kReasons) {
		if (r.status == status) {
			return r.phrase;
		}
	}
	return "OK";
}

} // namespace Http

#endif // OBS_MULTISTREAM_FRONTEND_HTTP_STATUS_HPP_
