project(braidcast-integration-tests)

# Headless backend integration tests. Reuses the project's existing cmocka
# harness (find_package + OBS::libobs linkage, mirroring test/cmocka) rather than
# introducing a new framework. Wired from the root CMakeLists behind
# ENABLE_INTEGRATION_TESTS (default OFF) so normal builds are unaffected.

find_package(CMocka CONFIG REQUIRED)

set(_fe "${CMAKE_SOURCE_DIR}/frontend")

# nlohmann/json is header-only and ships in the obs-deps prefix without a target,
# so every consumer has to find the prefix itself. Mirrors the frontend's
# resolution. SYSTEM so the project's global /W4 /WX does not flag its headers.
function(_add_nlohmann_include _target)
  foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${_prefix}/include/nlohmann/json.hpp")
      target_include_directories(${_target} SYSTEM PRIVATE "${_prefix}/include")
    endif()
  endforeach()
endfunction()

# The persistence layer under test, compiled directly into the test binary so the
# REAL SceneCollection::Save/Load + the REAL multistream stores run. These TUs
# reference no Bridge/CEF/ObsBootstrap symbols; bootstrap_shim.cpp supplies the
# four ObsBootstrap/Transitions seams they resolve their singletons through.
set(
  _fe_sources
  "${_fe}/src/scene/scene_persistence.cpp"
  "${_fe}/src/multistream/CanvasRuntime.cpp"
  "${_fe}/src/multistream/CanvasStore.cpp"
  "${_fe}/src/multistream/StorePaths.cpp"
  "${_fe}/utility/CanvasDefinition.cpp"
  # CanvasStore::Add mints a uuid through it.
  "${_fe}/utility/uuid_util.cpp"
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

# nlohmann/json backs CanvasStore.hpp's ToJson/FromJson.
_add_nlohmann_include(${_target})

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

# test_history: the future home for the sqlite-backed stream-history suite
# (History::Db, then SessionStore/SessionRecorder). braidcast-sqlite is declared
# under ENABLE_FRONTEND, and this directory is added under the independent
# ENABLE_INTEGRATION_TESTS, so a frontend-less configure would otherwise
# reference a target that does not exist.
if(NOT TARGET OBS::sqlite)
  add_subdirectory("${CMAKE_SOURCE_DIR}/frontend/deps" "${CMAKE_BINARY_DIR}/frontend-deps")
endif()

set(
  _history_sources
  "${_fe}/src/history/Db.cpp"
  "${_fe}/src/history/Migrations.cpp"
  "${_fe}/src/history/SessionRecorder.cpp"
  "${_fe}/src/history/SessionStore.cpp"
  # SessionRecorder::Begin mints session/destination uuids through it.
  "${_fe}/utility/uuid_util.cpp"
)

set(_target test_history)

# Db.cpp has no libobs or scene/canvas dependency, and its tests only need
# Harness::TempDir() -- a header-only helper chosen specifically so this target
# does not have to compile harness.cpp (see the doc comment on TempDir() in
# harness.hpp for why that would drag in the persistence sources and
# bootstrap_shim.cpp). A later task that needs the full harness (a booted libobs
# world) should add those sources then.
add_executable(${_target} test_history.cpp harness.hpp ${_history_sources})

target_include_directories(${_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_fe}/src" "${_fe}/utility")

# Third-party headers as SYSTEM so the project's global /W4 /WX does not flag
# them. frontend/deps carries sqlite_orm/sqlite_orm.h (header-only, no target);
# sqlite3.h itself arrives through OBS::sqlite's own SYSTEM PUBLIC include dir.
target_include_directories(${_target} SYSTEM PRIVATE ${CMOCKA_INCLUDE_DIR} "${CMAKE_SOURCE_DIR}/frontend/deps")

# SessionRecorder.cpp serializes canvas uuid / tag lists through nlohmann/json.
_add_nlohmann_include(${_target})

# Link the OBS:: alias, never the raw braidcast-sqlite target: braidcast-sqlite
# carries the compile definitions deciding real SQLite semantics (e.g.
# SQLITE_DEFAULT_FOREIGN_KEYS, which gates whether ON DELETE CASCADE fires at
# all), so this target must link the object that was actually compiled with them
# rather than repeat them here and risk drifting from what the product ships.
target_link_libraries(${_target} PRIVATE OBS::libobs OBS::sqlite cmocka::cmocka)

# obs.h uses nameless struct/union (C4201), fatal under the project's global
# /W4 /WX. harness.hpp includes obs.h, so suppress it target-wide.
if(MSVC)
  target_compile_options(${_target} PRIVATE /wd4201)
endif()

# Mirrors test_persistence's output location for consistency, and gets this
# target ready for the tests this suite will grow into: once one of them calls
# Harness::BootObs (a booted libobs world), it needs libobs-d3d11.dll +
# data/libobs at runtime, which only resolve from the rundir bin dir.
set_target_properties(
  ${_target}
  PROPERTIES FOLDER tests RUNTIME_OUTPUT_DIRECTORY "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}"
)

add_test(
  NAME ${_target}
  COMMAND ${_target}
  WORKING_DIRECTORY "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}"
)
