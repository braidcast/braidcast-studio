# Builds the vendored whisper.cpp (../whisper.cpp, tag v1.9.4) as DLLs for braidcast.exe:
# whisper.dll, ggml.dll, ggml-base.dll and ggml-cpu.dll. A directory of its own so every
# setting below is scoped to whisper and cannot reach the app or the other vendored deps:
#
# - The app defines UNICODE/_UNICODE globally (cmake/windows/compilerconfig.cmake);
#   upstream builds without them, so they are removed for this subtree.
# - Shared, and delay-loaded by the exe (frontend/cmake/voice-engine.cmake). ggml-cpu
#   is compiled for AVX2 (MSVC implies FMA/F16C with /arch:AVX2), and nothing of it may
#   run on a CPU without those: not a static initializer, and not one of its C++ inline
#   functions, which in a static link can be the one copy the whole exe uses (of the
#   duplicate definitions of an inline function, the linker keeps any one). As DLLs,
#   ggml's code stays in ggml's own images, which are mapped only when
#   Voice::EnsureWhisperLoaded loads them, after Voice::CpuSupportsVoice has passed.
#   BUILD_SHARED_LIBS is a normal variable here, so it is ON for this directory and
#   below and nowhere else in the tree.
# - CPU backend only, one ISA. GGML_BACKEND_DL with GGML_CPU_ALL_VARIANTS would pick an
#   ISA at run time; see frontend/deps/README.md for why voice does not use it.
# - OpenMP off so nothing depends on vcomp140.dll; warnings are upstream's business,
#   so warnings-as-errors is off here, at link as well as compile (the tree adds /WX to
#   every link when CMAKE_COMPILE_WARNING_AS_ERROR is on, and these DLLs now link).
# - WHISPER_BUILD_IS_DEV defaults ON upstream and would make whisper_version() report
#   "1.9.4-dev"; the voice self-test pins the release string.
# - EXCLUDE_FROM_ALL keeps the parakeet library whisper.cpp now defines beside whisper
#   out of the build, since nothing links it. It also drops whisper's and ggml's own
#   install() rules, so the DLLs are staged and installed by
#   frontend/cmake/voice-engine.cmake.
get_directory_property(_whisper_defs COMPILE_DEFINITIONS)
list(REMOVE_ITEM _whisper_defs UNICODE _UNICODE)
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_whisper_defs}")
get_directory_property(_whisper_link_options LINK_OPTIONS)
list(REMOVE_ITEM _whisper_link_options /WX)
set_directory_properties(PROPERTIES LINK_OPTIONS "${_whisper_link_options}")

set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
set(BUILD_SHARED_LIBS ON)

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
