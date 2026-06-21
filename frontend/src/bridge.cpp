#include "bridge.hpp"

#include <obs.h>

#include "log.hpp"

bool ObsQueryHandler::OnQuery(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> /*frame*/, int64_t /*query_id*/,
			      const CefString &request, bool /*persistent*/, CefRefPtr<Callback> callback)
{
	if (request == "getObsVersion") {
		const char *version = obs_get_version_string();
		HostLog(std::string("[bridge] OnQuery getObsVersion -> ") + (version ? version : "(null)"));
		callback->Success(version ? version : "");
		return true;
	}

	return false; // not handled
}
