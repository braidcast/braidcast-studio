#include "voice/WhisperLibrary.hpp"

#ifdef _WIN32
#include "log.hpp"
#include "util/paths.hpp"
#include "voice/VoiceCpu.hpp"

#include <windows.h>
#include <delayimp.h>
#endif

#include <iterator>
#include <utility>

namespace Voice {

WhisperLoader::WhisperLoader(CpuCheck cpu, LoadDll load) : cpu_(std::move(cpu)), load_(std::move(load)) {}

bool WhisperLoader::Ensure(std::string &reason)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (outcome_ == Outcome::Loaded) {
		reason.clear();
		return true;
	}
	if (outcome_ == Outcome::Failed) {
		reason = failure_;
		return false;
	}
	// Before anything is loaded, and asked again on every call until something has
	// been: a refusal here is not an attempt, so it is not kept.
	if (!cpu_(reason)) {
		return false;
	}
	for (const char *dll : kWhisperDlls) {
		std::string why;
		if (!load_(dll, why)) {
			outcome_ = Outcome::Failed;
			failure_ = why.empty() ? std::string("Voice control's speech engine could not be loaded (") +
							 dll + ")."
					       : why;
			reason = failure_;
			return false;
		}
	}
	outcome_ = Outcome::Loaded;
	reason.clear();
	return true;
}

std::string WhisperLoader::Failure() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return outcome_ == Outcome::Failed ? failure_ : std::string();
}

namespace {

#ifdef _WIN32

#if defined(_MSC_VER)
// The delay-load helper reports a DLL or a function it cannot find by raising one of
// these two; anything else is not ours to catch.
int DelayLoadFilter(DWORD code, DWORD *caught)
{
	if (code == VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND) ||
	    code == VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND)) {
		*caught = code & 0xFFFF;
		return EXCEPTION_EXECUTE_HANDLER;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

// Resolves every function the exe delay-loads from `dll` now, rather than at the first
// call to each, and returns 0 or the Win32 error that stopped it. A frame with __try may
// hold no C++ object, hence the plain types. For a DLL the exe imports nothing from
// (the ggml ones, today), __HrLoadAllImportsForDll returns a failure code without
// raising, which is not an error: there is nothing to bind.
DWORD BindDelayImports(const char *dll)
{
	DWORD caught = 0;
	__try {
		__HrLoadAllImportsForDll(dll);
	} __except (DelayLoadFilter(GetExceptionCode(), &caught)) {
		return caught;
	}
	return 0;
}
#else
// MinGW, which builds this file only for the tree's syntax check: it has no __try. The
// product is built with MSVC.
DWORD BindDelayImports(const char *)
{
	return 0;
}
#endif

// One DLL, by full path, from the directory braidcast.exe is in: the copy installed beside
// it, never one found elsewhere on the search path. Its own imports are looked for in
// that directory first (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR). The handle is never freed: the
// exe's delay-load table points into the DLL for the rest of the process.
bool LoadBesideExe(const char *dll, std::string &reason)
{
	const std::string name(dll);
	const std::wstring path = ExecutableDir() + L"\\" + std::wstring(name.begin(), name.end());
	if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
		reason = "Voice control's speech engine is missing: " + name +
			 " is not next to braidcast.exe. Reinstalling Braidcast restores it.";
	} else if (!LoadLibraryExW(path.c_str(), nullptr,
				   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) {
		reason = "Voice control's speech engine could not be loaded (" + name + ", error " +
			 std::to_string(GetLastError()) + ").";
	} else if (const DWORD error = BindDelayImports(dll); error != 0) {
		reason = "Voice control's speech engine does not match this version of Braidcast (" + name +
			 ", error " + std::to_string(error) + "). Reinstalling Braidcast restores it.";
	} else {
		if (name == kWhisperDlls[std::size(kWhisperDlls) - 1]) {
			HostLog("[voice] speech engine loaded");
		}
		return true;
	}
	HostLog("[voice] speech engine unavailable: " + reason);
	return false;
}

WhisperLoader &ProcessLoader()
{
	static WhisperLoader loader(&CpuSupportsVoice, &LoadBesideExe);
	return loader;
}

#else

// The host test builds link whisper statically: there is nothing to load, and their
// CPUs run it.
WhisperLoader &ProcessLoader()
{
	static WhisperLoader loader(
		[](std::string &reason) {
			reason.clear();
			return true;
		},
		[](const char *, std::string &) { return true; });
	return loader;
}

#endif

} // namespace

bool EnsureWhisperLoaded(std::string &reason)
{
	return ProcessLoader().Ensure(reason);
}

std::string WhisperLoadFailure()
{
	return ProcessLoader().Failure();
}

} // namespace Voice
