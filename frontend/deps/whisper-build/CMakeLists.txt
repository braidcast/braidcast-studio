# Builds the vendored whisper.cpp (../whisper.cpp, tag v1.9.4) as static libraries
# for braidcast.exe. A directory of its own so every setting below is scoped to
# whisper and cannot reach the app or the other vendored deps:
#
# - The app defines UNICODE/_UNICODE globally (cmake/windows/compilerconfig.cmake);
#   upstream builds without them, so they are removed for this subtree.
# - Static + CPU backend only. ggml cannot combine a static build with runtime CPU
#   dispatch (ggml/src/CMakeLists.txt:188-189 and :487-489: GGML_BACKEND_DL requires
#   BUILD_SHARED_LIBS, GGML_CPU_ALL_VARIANTS requires GGML_BACKEND_DL), so the ISA is
#   fixed at AVX2 (MSVC implies FMA/F16C with /arch:AVX2) and Voice::CpuSupportsVoice
#   gates the feature at runtime.
# - OpenMP off so nothing depends on vcomp140.dll; warnings are upstream's business,
#   so warnings-as-errors is off here.
# - WHISPER_BUILD_IS_DEV defaults ON upstream and would make whisper_version() report
#   "1.9.4-dev"; the voice self-test pins the release string.
# - EXCLUDE_FROM_ALL also drops whisper's and ggml's install() rules: they are
#   static and nothing of theirs ships beside the exe. It also keeps the parakeet
#   library whisper.cpp now defines beside whisper out of the build, since nothing
#   links it.
get_directory_property(_whisper_defs COMPILE_DEFINITIONS)
list(REMOVE_ITEM _whisper_defs UNICODE _UNICODE)
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_whisper_defs}")

set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
set(BUILD_SHARED_LIBS OFF)

set(GGML_NATIVE OFF)
set(GGML_SSE42 ON)
set(GGML_AVX ON)
set(GGML_AVX2 ON)
set(GGML_BMI2 ON)
set(GGML_AVX512 OFF)
set(GGML_OPENMP OFF)
set(GGML_CCACHE OFF)
set(GGML_BLAS OFF)
set(GGML_CUDA OFF)
set(GGML_VULKAN OFF)
set(GGML_OPENCL OFF)

set(WHISPER_BUILD_IS_DEV OFF)
set(WHISPER_BUILD_TESTS OFF)
set(WHISPER_BUILD_EXAMPLES OFF)
set(WHISPER_BUILD_SERVER OFF)
set(WHISPER_CURL OFF)
set(WHISPER_SDL2 OFF)

add_subdirectory(
  "${CMAKE_CURRENT_SOURCE_DIR}/../whisper.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/whisper.cpp"
  EXCLUDE_FROM_ALL
  SYSTEM
)

set_target_properties(whisper ggml ggml-base ggml-cpu PROPERTIES FOLDER deps)
