# Stamp the build's git provenance into a generated header, so every session log can
# identify the exact tree a binary was built from. Runs at configure time and again on
# every build (as a custom target); writes only when the value changes, so it does not
# force a needless relink each build. Never fails the build -- a missing git or a source
# tarball just yields "unknown".
#
# Args (via -D): OUT = header path to write, SRC = git work tree (repo root).

set(_desc "unknown")

find_package(Git QUIET)
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SRC}" describe --tags --always --dirty
    OUTPUT_VARIABLE _desc
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _rc
  )
  if(NOT _rc EQUAL 0 OR _desc STREQUAL "")
    set(_desc "unknown")
  endif()
endif()

set(_content "#pragma once\n#define BRAIDCAST_BUILD_DESCRIBE \"${_desc}\"\n")

set(_existing "")
if(EXISTS "${OUT}")
  file(READ "${OUT}" _existing)
endif()
if(NOT _existing STREQUAL "${_content}")
  file(WRITE "${OUT}" "${_content}")
endif()
