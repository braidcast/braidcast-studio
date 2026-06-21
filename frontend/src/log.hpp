#ifndef OBS_MULTISTREAM_FRONTEND_LOG_HPP_
#define OBS_MULTISTREAM_FRONTEND_LOG_HPP_

#include <windows.h>

#include <cstdio>
#include <string>

// Mirror a line to both the debugger output and stderr, so lifecycle logging is
// visible whether the host runs under a debugger or with stderr redirected to a
// file. Used by the shell's startup/teardown/CEF callbacks.
inline void HostLog(const std::string &line)
{
	OutputDebugStringA(line.c_str());
	OutputDebugStringA("\n");
	fprintf(stderr, "%s\n", line.c_str());
	fflush(stderr);
}

#endif // OBS_MULTISTREAM_FRONTEND_LOG_HPP_
