project(braidcast-integration-tests)

# Headless backend integration tests. Reuses the project's existing cmocka
# harness (find_package + OBS::libobs linkage, mirroring test/cmocka) rather than
# introducing a new framework. Wired from the root CMakeLists behind
# ENABLE_INTEGRATION_TESTS (default OFF) so normal builds are unaffected.

find_package(CMocka CONFIG REQUIRED)

set(_fe "${CMAKE_SOURCE_DIR}/frontend")

# The persistence layer under test, compiled directly into the test binary so the
# REAL SceneCollection::Save/Load + the REAL multistream stores run. These TUs
# reference no Bridge/CEF/ObsBootstrap symbols; bootstrap_shim.cpp supplies the
# four ObsBootstrap/Transitions seams they resolve their singletons through.
set(
  _fe_sources
  "${_fe}/src/scene_persistence.cpp"
  "${_fe}/src/multistream/CanvasRuntime.cpp"
  "${_fe}/src/multistream/CanvasStore.cpp"
  "${_fe}/src/multistream/StorePaths.cpp"
  "${_fe}/utility/CanvasDefinition.cpp"
)

set(_target test_persistence)

add_executable(
  ${_target}
  test_persistence.cpp
  harness.cpp
  harness.hpp
  bootstrap_shim.cpp
  testseam.hpp
  ${_fe_sources}
)

target_include_directories(${_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_fe}/src" "${_fe}/utility")

# Third-party headers as SYSTEM so the project's global /W4 /WX does not flag them.
target_include_directories(${_target} SYSTEM PRIVATE ${CMOCKA_INCLUDE_DIR})

# nlohmann/json (header-only) backs CanvasStore.hpp's ToJson/FromJson; it ships in
# the obs-deps prefix include dir. Mirror the frontend's resolution.
foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
  if(EXISTS "${_prefix}/include/nlohmann/json.hpp")
    target_include_directories(${_target} SYSTEM PRIVATE "${_prefix}/include")
  endif()
endforeach()

target_link_libraries(${_target} PRIVATE OBS::libobs cmocka::cmocka)

# obs.h uses nameless struct/union (C4201), fatal under the project's global
# /W4 /WX. Every TU here includes obs.h, so suppress it target-wide.
if(MSVC)
  target_compile_options(${_target} PRIVATE /wd4201)
endif()

# The test needs a D3D11 device (obs_reset_video); WARP satisfies it on a headless
# CI box (no physical GPU). It loads libobs-d3d11.dll + data/libobs effects at
# runtime, so it must sit in the OBS rundir bin dir like the other executables.
set_target_properties(
  ${_target}
  PROPERTIES FOLDER tests RUNTIME_OUTPUT_DIRECTORY "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}"
)

add_test(
  NAME ${_target}
  COMMAND ${_target}
  WORKING_DIRECTORY "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}"
)
