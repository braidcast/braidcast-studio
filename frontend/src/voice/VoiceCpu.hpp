#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_CPU_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_CPU_HPP_

#include <string>

namespace Voice {

// Whether this CPU can run the whisper.cpp build braidcast links. ggml is compiled
// static with /arch:AVX2 (a static ggml cannot dispatch at run time), so voice
// needs AVX, AVX2, FMA, F16C and BMI2, and the OS must have enabled the AVX register
// state. Fills `reason` with a sentence for the Settings tab when it returns false;
// clears it otherwise. Cheap (a few CPUID reads); call it wherever it is needed.
bool CpuSupportsVoice(std::string &reason);

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_CPU_HPP_
