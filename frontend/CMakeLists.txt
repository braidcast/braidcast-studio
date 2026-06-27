cmake_minimum_required(VERSION 3.28...3.30)

# Phase 4.1 CEF/Svelte frontend. 4.1.1 is the shell skeleton: a non-Qt host exe
# that boots libobs and shows one windowed CEF browser serving an offline app://
# page. Ported from the proven spike/ PoC (verdict GO).

# obs-frontend-api is frontend-agnostic; the new exe links OBS::frontend-api.
if(NOT TARGET OBS::frontend-api)
  add_subdirectory(api frontend-api)
endif()

if(NOT TARGET OBS::libobs)
  add_subdirectory("${CMAKE_SOURCE_DIR}/libobs" libobs)
endif()

# CEF: build our OWN /MD libcef_dll_wrapper from the project distribution. The
# prebuilt CEF::Wrapper used by obs-browser is /MT (incompatible with this /MD
# exe that links the /MD obs.lib), so we cannot reuse it. We do NOT force the
# MSVC runtime globally (this is an add_subdirectory of the whole OBS build, and
# forcing it would corrupt sibling targets) -- the OBS default is already /MD,
# and CEF_RUNTIME_LIBRARY_FLAG=/MD aligns the wrapper's Release/Debug configs.
set(CEF_ROOT "${CMAKE_SOURCE_DIR}/.deps/cef_binary_6533_windows_x64" CACHE PATH "CEF distribution for the new frontend")

# Must be set before the dist's find_package(CEF). USE_SANDBOX OFF keeps a plain
# windowed exe (settings.no_sandbox=true); USE_ATL OFF trims dependencies;
# CEF_RUNTIME_LIBRARY_FLAG=/MD flips the wrapper + CEF macros to the dynamic CRT.
set(USE_SANDBOX OFF CACHE BOOL "" FORCE)
set(USE_ATL OFF CACHE BOOL "" FORCE)
set(CEF_RUNTIME_LIBRARY_FLAG "/MD" CACHE STRING "" FORCE)

# Load the distribution's CMake helpers (cef_variables/cef_macros) and build the
# wrapper from its bundled source. Include the dist's FindCEF.cmake directly by
# path: the OBS tree ships its own cmake/finders/FindCEF.cmake (for obs-browser's
# prebuilt /MT wrapper), so find_package(CEF) would resolve to the wrong finder.
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
include("${CEF_ROOT}/cmake/FindCEF.cmake")

add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)

# Logical target for libcef.lib (the import library) per the CEF macro contract.
add_logical_target("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

set(_target obs-multistream-frontend)

add_executable(${_target} WIN32)
add_executable(OBS::studio ALIAS ${_target})

target_sources(
  ${_target}
  PRIVATE
    src/main.cpp
    src/app.cpp
    src/app.hpp
    src/client.cpp
    src/client.hpp
    src/scheme.cpp
    src/scheme.hpp
    src/bridge.cpp
    src/bridge.hpp
    src/properties_serializer.cpp
    src/properties_serializer.hpp
    src/obs_bootstrap.cpp
    src/obs_bootstrap.hpp
    src/preview_window.cpp
    src/preview_window.hpp
    src/window_manager.cpp
    src/window_manager.hpp
    src/projector_window.cpp
    src/projector_window.hpp
    src/audio/AudioMonitor.cpp
    src/audio/AudioMonitor.hpp
    src/scene_collections.cpp
    src/scene_collections.hpp
    src/scene_persistence.cpp
    src/scene_persistence.hpp
    src/transitions.cpp
    src/transitions.hpp
    src/log.hpp
    api-impl/frontend_callbacks.cpp
    api-impl/frontend_callbacks.hpp
    # Phase 4.4.0 native-multistream data model. These five model files are
    # Qt-free (libobs obs_data_* + std only); the *Store files de-Qt the two
    # managers (config dir via os_get_config_path instead of OBSApp).
    utility/CanvasDefinition.cpp
    utility/OBSCanvas.cpp
    utility/CanvasSceneLink.cpp
    utility/OutputBinding.cpp
    utility/StreamProfile.cpp
    # Qt-free undo/redo stack (std-only; no libobs), ported from the legacy
    # undo_stack. Out of the /wd4201 set -- it includes no obs.h.
    utility/UndoManager.cpp
    utility/UndoManager.hpp
    src/multistream/CanvasStore.cpp
    src/multistream/CanvasStore.hpp
    src/multistream/StreamProfileStore.cpp
    src/multistream/StreamProfileStore.hpp
    src/multistream/OutputBindingStore.cpp
    src/multistream/OutputBindingStore.hpp
    src/multistream/SceneLinkStore.cpp
    src/multistream/SceneLinkStore.hpp
    src/multistream/MultistreamEngine.cpp
    src/multistream/MultistreamEngine.hpp
    src/multistream/CanvasRuntime.cpp
    src/multistream/CanvasRuntime.hpp
    src/multistream/Hotkeys.cpp
    src/multistream/Hotkeys.hpp
    src/multistream/VirtualCamManager.cpp
    src/multistream/VirtualCamManager.hpp
    src/multistream/StorePaths.hpp
    # Embedded MCP server (Stage 1): a localhost HTTP/1.1 endpoint speaking the MCP
    # JSON-RPC subset, proxying a generic obs_call tool through Bridge::Dispatch.
    # HttpServer is deliberately libobs-free (Winsock only) so it stays out of the
    # /wd4201 set; only McpServer.cpp includes obs.h.
    src/mcp/HttpServer.cpp
    src/mcp/HttpServer.hpp
    src/mcp/McpServer.cpp
    src/mcp/McpServer.hpp
)

# Windows resource: embed the app icon + version/product metadata so the exe and
# its taskbar/Explorer entry show the OBS MultiStream mark instead of the default
# blank icon. configure_file substitutes the icon path (forward slashes, which the
# resource compiler accepts) and the shared OBS_VERSION_*/OBS_LEGAL_COPYRIGHT.
if(OS_WINDOWS)
  set(OBS_FRONTEND_ICON "${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/obs-multistream.ico")
  configure_file(cmake/windows/obs.rc.in obs.rc)
  target_sources(${_target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/obs.rc")
endif()

# CEF's required compiler/linker flags, defines, and include paths.
set_executable_target_properties(${_target})

target_include_directories(
  ${_target}
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/api-impl"
    # The model files (and the *Store ports that include their headers)
    # resolve "CanvasDefinition.hpp" etc. from utility/.
    "${CMAKE_CURRENT_SOURCE_DIR}/utility"
)

# The bridge envelope uses nlohmann/json, a header-only lib shipped in the
# obs-deps prefix include dir (the legacy frontend gets it transitively via Qt;
# this non-Qt exe must add it explicitly). Add every CMAKE_PREFIX_PATH/include.
foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
  if(EXISTS "${_prefix}/include/nlohmann/json.hpp")
    target_include_directories(${_target} PRIVATE "${_prefix}/include")
  endif()
endforeach()

add_dependencies(${_target} libcef_dll_wrapper)
target_link_libraries(
  ${_target}
  PRIVATE
    libcef_lib
    libcef_dll_wrapper
    ${CEF_STANDARD_LIBS}
    OBS::libobs
    OBS::frontend-api
    # IFileDialog (shobjidl) lives in ole32/shell32; SHCreateItemFromParsingName in shlwapi.
    ole32
    shell32
    shlwapi
    # The embedded MCP server's HttpServer uses Winsock.
    ws2_32
)

# CEF's /DELAYLOAD list (from SET_EXECUTABLE_TARGET_PROPERTIES) names DLLs this
# exe imports nothing from, yielding benign LNK4199 -- fatal under the project's
# global link-time /WX. Ignore just that warning; the delayloads are harmless.
target_link_options(${_target} PRIVATE "/IGNORE:4199")

# obs.h uses nameless struct/union (C4201), fatal under CEF's /W4 /WX. Suppress
# only on the TUs that include obs.h.
set_source_files_properties(
  src/main.cpp
  src/obs_bootstrap.cpp
  src/bridge.cpp
  src/properties_serializer.cpp
  src/preview_window.cpp
  src/projector_window.cpp
  src/audio/AudioMonitor.cpp
  src/scene_collections.cpp
  src/scene_persistence.cpp
  src/transitions.cpp
  api-impl/frontend_callbacks.cpp
  utility/CanvasDefinition.cpp
  utility/OBSCanvas.cpp
  utility/CanvasSceneLink.cpp
  utility/OutputBinding.cpp
  utility/StreamProfile.cpp
  src/multistream/CanvasStore.cpp
  src/multistream/StreamProfileStore.cpp
  src/multistream/OutputBindingStore.cpp
  src/multistream/SceneLinkStore.cpp
  src/multistream/MultistreamEngine.cpp
  src/multistream/CanvasRuntime.cpp
  src/multistream/Hotkeys.cpp
  src/multistream/VirtualCamManager.cpp
  src/mcp/McpServer.cpp
  PROPERTIES COMPILE_OPTIONS "/wd4201"
)

# Place the exe in the OBS rundir (bin/64bit) like other targets; keep a distinct
# OUTPUT_NAME so it does not collide with the legacy obs64 in a shared rundir
# during the transition. It still reads the shared obs-multistream config dir.
set_target_properties_obs(${_target} PROPERTIES FOLDER frontend OUTPUT_NAME obs-multistream)

# Copy the CEF runtime binaries + resources next to the exe in the rundir so it
# launches in place. The dist only ships Release/Debug; the OBS build is
# RelWithDebInfo, for which CEF_BINARY_DIR's $<CONFIGURATION> would expand to a
# nonexistent dir -- pull from the explicit Release dir instead.
set(_cef_rundir "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}")
copy_files(${_target} "${CEF_BINARY_FILES}" "${CEF_ROOT}/Release" "${_cef_rundir}")
copy_files(${_target} "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${_cef_rundir}")

# Build the offline Svelte UI bundle with bun, then install it into the shared
# config-named data dir. bun is required (no npm/pnpm/yarn fallback) so the build
# matches the committed bun.lock.
find_program(BUN_EXECUTABLE bun REQUIRED)

set(_web_dir "${CMAKE_CURRENT_SOURCE_DIR}/web")
set(_web_dist "${_web_dir}/dist")
set(_web_install "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_DATA_DESTINATION}/obs-multistream/web")

# Always-run target: bun install (idempotent) then vite build. bun/vite are fast
# and incremental, so an unconditional rebuild is cheap and avoids stale bundles
# when web sources change without a tracked dependency graph here.
add_custom_target(
  frontend-web
  COMMAND "${BUN_EXECUTABLE}" install
  COMMAND "${BUN_EXECUTABLE}" run build
  WORKING_DIRECTORY "${_web_dir}"
  BYPRODUCTS "${_web_dist}/index.html"
  COMMENT "Build offline Svelte UI bundle (bun + vite)"
  VERBATIM
)

add_dependencies(${_target} frontend-web)

# Copy the freshly built bundle (web/dist) into the rundir data dir.
add_custom_command(
  TARGET ${_target}
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_web_install}"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_web_dist}" "${_web_install}"
  COMMENT "Copy Svelte UI bundle to rundir"
  VERBATIM
)

print_cef_config()
