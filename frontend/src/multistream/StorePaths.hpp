#pragma once

#include <util/platform.h>

#include <string>

// Resolves a file under the shared <config>/braidcast/basic directory --
// the SAME directory the legacy Qt frontend writes canvases.json/streams.json
// to. The legacy frontend computes this as userProfilesLocation (which defaults
// to os_get_config_path(nullptr)) + "/braidcast/basic/<file>"; we go
// straight through os_get_config_path with that relative subdir so the new and
// legacy frontends read/write byte-identical files.
inline std::string MultistreamBasicPath(const char *file)
{
	char base[512];
	if (os_get_config_path(base, sizeof(base), "braidcast/basic") <= 0) {
		return std::string();
	}
	std::string path = base;
	path += "/";
	path += file;
	return path;
}
