# Voice control's speech engine: whisper.cpp and the ggml it vendors, built as DLLs by
# deps/whisper-build because ggml-cpu is compiled for AVX2 and nothing of it may run on a
# CPU without AVX2. Linked to ${_target} like any library, but delay-loaded, so none of
# them is mapped when the app starts: Voice::EnsureWhisperLoaded (src/voice/WhisperLibrary)
# loads them from the exe's own directory the first time voice is used, and only after
# Voice::CpuSupportsVoice has passed. Expects ${_target}, OBS_OUTPUT_DIR and
# OBS_EXECUTABLE_DESTINATION.
#
# The list is in load order, each library after the ones it imports. It is the one list
# behind the delay-load flags, the rundir copy and the install rules here, and it must
# match kWhisperDlls in src/voice/WhisperLibrary.hpp and the lists in
# .github/scripts/Check-VoiceDlls.ps1, Smoke-Package.ps1 and Package-Msix.ps1.
set(_voice_engine_targets ggml-base ggml-cpu ggml whisper)

set(_voice_engine_files "")
foreach(_voice_engine_target IN LISTS _voice_engine_targets)
  list(APPEND _voice_engine_files "$<TARGET_FILE:${_voice_engine_target}>")
endforeach()

# The exe calls only whisper_* functions, so whisper.dll is the one DLL it imports from;
# whisper.dll imports the ggml DLLs itself. The ggml DLLs are delay-loaded too, so that a
# direct ggml_* call added later cannot quietly map ggml-cpu at launch again; with no
# import from one of them the linker says LNK4199, which frontend/CMakeLists.txt already
# ignores for CEF's list. A delay-load cannot import data (LNK1194), so whisper and ggml
# must only ever be called, never have a variable of theirs read.
# .github/scripts/Check-VoiceDlls.ps1 checks the built exe: these four as delay-load
# imports only, and all of them beside it.
if(MSVC)
  foreach(_voice_engine_target IN LISTS _voice_engine_targets)
    target_link_options(${_target} PRIVATE "/DELAYLOAD:$<TARGET_FILE_NAME:${_voice_engine_target}>")
  endforeach()
  target_link_libraries(${_target} PRIVATE delayimp)
else()
  message(WARNING "${_target}: voice engine DLLs are not delay-loaded with this toolchain")
endif()

# Beside the exe in the rundir and in the package. $<TARGET_RUNTIME_DLLS> already carries
# them, since CMake follows the link graph, but the loader looks for exactly these four
# files beside the exe, so they are named here rather than left to that. whisper.cpp's
# own install() rules do not run (its directory is EXCLUDE_FROM_ALL), so without the rule
# below the package would ship an exe whose voice control says its engine is missing.
add_custom_command(
  TARGET ${_target}
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}"
  COMMAND
    "${CMAKE_COMMAND}" -E copy_if_different ${_voice_engine_files}
    "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}"
  COMMENT "Copying the voice engine DLLs to the run directory"
  VERBATIM
)
install(FILES ${_voice_engine_files} DESTINATION "${OBS_EXECUTABLE_DESTINATION}" COMPONENT Runtime)
