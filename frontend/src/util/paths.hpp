#pragma once

#include <windows.h>

#include <string>

// Absolute UTF-8 path to the OBS rundir root (the parent of bin/ and data/),
// derived from the exe path so nothing is hardcoded to a build tree. The exe
// lives in <rundir>/bin/64bit, so strip the exe name, then 64bit, then bin.
// Both the libobs data path and the app:// bundle path resolve off this.
inline std::string RundirRoot()
{
	wchar_t exe_path[MAX_PATH] = {0};
	GetModuleFileNameW(nullptr, exe_path, MAX_PATH);

	std::wstring dir(exe_path);
	for (int i = 0; i < 3; ++i) {
		size_t slash = dir.find_last_of(L"\\/");
		if (slash == std::wstring::npos) {
			break;
		}
		dir.resize(slash);
	}

	if (dir.empty()) {
		return std::string();
	}
	const int len = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
	std::string utf8(len > 0 ? len - 1 : 0, '\0');
	WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, utf8.data(), len, nullptr, nullptr);
	return utf8;
}

// Absolute wide path to the directory that holds the running executable
// (<rundir>/bin/64bit). Kept as a native wide string so it feeds both the Win32
// loader APIs and std::filesystem::path directly without a UTF-8 round-trip.
inline std::wstring ExecutableDir()
{
	wchar_t exe_path[MAX_PATH] = {0};
	GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
	std::wstring dir(exe_path);
	size_t slash = dir.find_last_of(L"\\/");
	if (slash != std::wstring::npos) {
		dir.resize(slash);
	}
	return dir;
}
