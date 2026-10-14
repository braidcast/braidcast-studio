#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_WHISPER_LIBRARY_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_WHISPER_LIBRARY_HPP_

#include <functional>
#include <mutex>
#include <string>

namespace Voice {

// whisper.cpp's DLLs, in load order (each after the ones it imports). braidcast.exe
// delay-loads every one of them (cmake/voice-engine.cmake holds the same list), so none is
// mapped into the process until EnsureWhisperLoaded loads it: ggml-cpu is compiled for
// AVX2, and on a CPU without AVX2 not one instruction of it, static initializers
// included, may ever run.
constexpr const char *kWhisperDlls[] = {"ggml-base.dll", "ggml-cpu.dll", "ggml.dll", "whisper.dll"};

// The way in to whisper: no whisper_* function is called before this has returned true
// (Recognizer::LoadModel, VadEndpointer::Start and the voice self-tests ask it first, and
// the engine asks it before it starts anything that would).
//
// On a CPU CpuSupportsVoice refuses, it returns false with that reason and loads nothing.
// Otherwise it loads each DLL from the exe's own directory and binds the exe's delay-load
// imports from it, so that a missing or mismatched DLL is a reason here rather than a
// delay-load exception, which would end the process, at the first whisper call. The outcome of
// the first attempt past the CPU check is kept for the life of the process (the DLLs are
// never unloaded): later calls return it without trying again.
//
// Any thread. The call that loads holds the others for the few milliseconds it takes.
bool EnsureWhisperLoaded(std::string &reason);

// Why loading failed, when it was tried and failed; "" when it has not been tried or it
// worked. Loads nothing, so it is safe wherever voice merely reports its state.
std::string WhisperLoadFailure();

// The sequence EnsureWhisperLoaded runs, over steps passed in so it can be tested without
// a loader: the CPU check first, every time until a load has been tried, then each DLL
// in kWhisperDlls order until one fails, once for the process.
class WhisperLoader {
public:
	using CpuCheck = std::function<bool(std::string &reason)>;
	using LoadDll = std::function<bool(const char *dll, std::string &reason)>;

	WhisperLoader(CpuCheck cpu, LoadDll load);

	bool Ensure(std::string &reason);
	std::string Failure() const;

private:
	enum class Outcome { NotTried, Loaded, Failed };

	CpuCheck cpu_;
	LoadDll load_;
	mutable std::mutex mutex_;
	Outcome outcome_ = Outcome::NotTried;
	std::string failure_;
};

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_WHISPER_LIBRARY_HPP_
