#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_CPU_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_CPU_HPP_

#include <string>

namespace Voice {

// Whether this CPU can run the whisper.cpp build braidcast ships. ggml-cpu is compiled
// for one ISA, /arch:AVX2 (deps/whisper-build), so voice needs AVX, AVX2, FMA, F16C and
// BMI2, and the OS must have enabled the AVX register state. EnsureWhisperLoaded asks
// this before it maps any of whisper's DLLs. Fills `reason` with a sentence for the
// Settings tab when it returns false; clears it otherwise. Cheap (a few CPUID reads);
// call it wherever it is needed.
bool CpuSupportsVoice(std::string &reason);

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_CPU_HPP_
