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

set(_target braidcast-frontend)

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
    src/http_client.cpp
    src/http_client.hpp
    src/async_task.cpp
    src/async_task.hpp
    src/properties_serializer.cpp
    src/properties_serializer.hpp
    src/obs_bootstrap.cpp
    src/obs_bootstrap.hpp
    src/obs_importer.cpp
    src/obs_importer.hpp
    src/GeneralSettings.cpp
    src/GeneralSettings.hpp
    src/AdvancedSettings.cpp
    src/AdvancedSettings.hpp
    src/preview_window.cpp
    src/preview_window.hpp
    src/window_manager.cpp
    # Frameless custom-titlebar chrome (WM_NCCALCSIZE/NCHITTEST + CEF drag regions).
    src/window_chrome.cpp
    src/window_manager.hpp
    src/projector_window.cpp
    src/projector_window.hpp
    src/interact_window.cpp
    src/interact_window.hpp
    src/audio/AudioMonitor.cpp
    src/audio/AudioMonitor.hpp
    src/scene_collections.cpp
    src/scene_collections.hpp
    src/scene_persistence.cpp
    src/scene_persistence.hpp
    src/tray.cpp
    src/tray.hpp
    src/transitions.cpp
    src/transitions.hpp
    src/session_log.cpp
    src/session_log.hpp
    src/log.hpp
    # Build-time platform OAuth credential injection (Phase 8a). obf.c is the
    # symmetric nibble-XOR deobfuscator; provider_creds reads the configured
    # ui-config.h and returns the plaintext-or-deobfuscated client_id.
    src/obf.c
    src/obf.h
    src/provider_creds.cpp
    src/provider_creds.hpp
    # OAuth foundation: provider registry + capability descriptors, the broker auth
    # strategy (loopback listener + BrokerStrategy), and a DPAPI-wrapped
    # per-stream-profile token store. provider.hpp is the interface set the registry +
    # bridge dispatch through; concrete providers (Twitch/Kick/YouTube) plug in.
    src/oauth/provider.cpp
    src/oauth/provider.hpp
    src/oauth/loopback_listener.cpp
    src/oauth/loopback_listener.hpp
    src/oauth/broker_strategy.cpp
    src/oauth/broker_strategy.hpp
    src/oauth/account_store.cpp
    src/oauth/account_store.hpp
    src/oauth/registry.cpp
    src/oauth/registry.hpp
    src/oauth/twitch_provider.cpp
    src/oauth/twitch_provider.hpp
    src/oauth/kick_provider.cpp
    src/oauth/kick_provider.hpp
    src/oauth/youtube_provider.cpp
    src/oauth/youtube_provider.hpp
    # Phase 9.0 chat transport foundation: a reusable libcurl-WebSocket client, the
    # ChatTransport interface, and the ChatHub that runs live transports between
    # go-live and stop. Per-platform transports (Twitch/YouTube/Kick) land in later
    # Phase 9 tasks; the hub manages an empty set until then. None include obs.h, so
    # they stay out of the /wd4201 set.
    src/chat/ws_client.cpp
    src/chat/ws_client.hpp
    src/chat/chat_transport.hpp
    src/chat/chat_hub.cpp
    src/chat/chat_hub.hpp
    # Phase 9.0 Task 3: Twitch IRC-over-WebSocket chat transport (read + send),
    # owned by TwitchProvider; WS only, no obs.h.
    src/chat/twitch_chat.cpp
    src/chat/twitch_chat.hpp
    # Phase 9: third-party emote resolver (7TV/BTTV/FFZ) merged into Twitch chat
    # fragments; HTTP only, no obs.h.
    src/chat/third_party_emotes.cpp
    src/chat/third_party_emotes.hpp
    # Phase 9.0 Task 4: YouTube live-chat transport (liveChatMessages.list long-poll
    # + .insert send), owned by the YouTube provider; HTTP only, no obs.h.
    src/chat/youtube_chat.cpp
    src/chat/youtube_chat.hpp
    # Phase 9.0 Task 5: Kick chat transport (Pusher WebSocket read + REST send),
    # owned by KickProvider; HTTP + WS only, no obs.h.
    src/chat/kick_chat.cpp
    src/chat/kick_chat.hpp
    # Shared Kick/Pusher config (app key/cluster) + slug->chatroom/channel-id lookup,
    # referenced by both kick_chat and kick_events so the app key lives in ONE place.
    src/chat/kick_pusher.cpp
    src/chat/kick_pusher.hpp
    # Phase 9.0 Task 6: aggregate viewer-count poller. One worker polls each
    # connected provider's viewerCount() while live and emits viewers.changed;
    # HTTP only via the providers, no obs.h.
    src/chat/viewer_poller.cpp
    src/chat/viewer_poller.hpp
    # Channel identity: always-on audience-total poller. One worker reads each
    # connected provider's audienceCount() on a ~90s jittered cadence, persists a
    # changed total, and emits channels.stats; HTTP only via the providers, no obs.h.
    src/chat/channel_stats_poller.cpp
    src/chat/channel_stats_poller.hpp
    # Phase 9.2a events-feed foundation: the normalized cross-platform event model +
    # transport interface, a persisted/de-duplicated store, and the EventHub that runs
    # live event transports on the account-connect lifecycle. Per-platform transports
    # (Twitch EventSub / YouTube REST / Kick Pusher) land in later 9.2 tasks. Only
    # event_store.cpp includes obs.h (obs_data safe-JSON persistence) -> /wd4201 set;
    # event_hub.cpp is obs-free like chat_hub.
    src/events/event_model.hpp
    src/events/event_transport.hpp
    src/events/event_store.cpp
    src/events/event_store.hpp
    src/events/event_hub.cpp
    src/events/event_hub.hpp
    # Phase 9.2b: Twitch EventSub-over-WebSocket event transport (follow/sub/resub/
    # gift/cheer/raid), owned by TwitchProvider. HTTP + WS only, no obs.h.
    src/events/twitch_events.cpp
    src/events/twitch_events.hpp
    # Phase 9.2c: YouTube REST event transport (Super Chats / Super Stickers via
    # superChatEvents.list + recent subscribers via subscriptions.myRecentSubscribers),
    # owned by YouTubeProvider. Real-time monetization/membership events arrive via the
    # live-chat sink (youtube_chat forwards them to Events::Hub().Ingest). HTTP only.
    src/events/youtube_events.cpp
    src/events/youtube_events.hpp
    # Phase 9.2d: Kick Pusher event transport (best-effort reverse-engineered
    # sub/gift/host(raid)/follow), owned by KickProvider. Rides Kick's public Pusher
    # (shared config in src/chat/kick_pusher); HTTP + WS only, no obs.h.
    src/events/kick_events.cpp
    src/events/kick_events.hpp
    # Phase 9.3 overlay widgets: loopback HTTP + SSE server (winsock-only, no obs.h)
    # and the widget store (obs_data persistence -> /wd4201 set). The server serves an
    # assembled widget document + the OBSOverlay runtime + user assets off the web bundle.
    src/overlay/overlay_server.cpp
    src/overlay/overlay_server.hpp
    src/overlay/overlay_store.cpp
    src/overlay/overlay_store.hpp
    # Overlay self-test: real loopback CLIENT sockets, so winsock-only (no obs.h) and
    # kept out of obs_bootstrap.cpp to dodge the winsock2/windows.h include-order clash.
    src/overlay/overlay_selftest.cpp
    # Phase 8d: generic ingest-key writeback (marshals an RTMP server+key into a
    # stream profile on TID_UI; includes obs.h -> in the /wd4201 set below).
    src/ingest_writeback.cpp
    src/ingest_writeback.hpp
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
    src/multistream/CanvasService.cpp
    src/multistream/CanvasService.hpp
    src/multistream/GlobalAudioChannels.cpp
    src/multistream/GlobalAudioChannels.hpp
    src/multistream/StreamMetaStore.cpp
    src/multistream/StreamMetaStore.hpp
    src/multistream/Hotkeys.cpp
    src/multistream/Hotkeys.hpp
    src/multistream/VirtualCamManager.cpp
    src/multistream/VirtualCamManager.hpp
    src/multistream/StorePaths.cpp
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
# its taskbar/Explorer entry show the Braidcast mark instead of the default
# blank icon. configure_file substitutes the icon path (forward slashes, which the
# resource compiler accepts) and the shared OBS_VERSION_*/OBS_LEGAL_COPYRIGHT.
if(OS_WINDOWS)
  set(OBS_FRONTEND_ICON "${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/braidcast.ico")
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

# Phase 8a: inject the Twitch OAuth client_id at build time. The cache vars
# default to the environment so CI/.env feed the credential without a -D flag.
# TWITCH_HASH must default to 0 (not empty) so the "0x@TWITCH_HASH@" template
# expands to a valid "0x0" literal on the no-credential path.
# Local dev convenience: auto-load credentials from a gitignored repo-root .env
# (KEY=VALUE lines, CR-tolerant) into the environment so `cmake --preset` picks
# them up with no manual export. CI provides these as real env vars / secrets and
# has no .env, so this is a no-op there.
set(_env_file "${CMAKE_SOURCE_DIR}/.env")
if(EXISTS "${_env_file}")
  file(STRINGS "${_env_file}" _env_lines)
  foreach(_line IN LISTS _env_lines)
    string(STRIP "${_line}" _line)
    if(_line MATCHES "^([A-Za-z_][A-Za-z0-9_]*)=(.*)$")
      set(ENV{${CMAKE_MATCH_1}} "${CMAKE_MATCH_2}")
    endif()
  endforeach()
  message(STATUS "Loaded build credentials from ${_env_file}")
endif()

# Env input name matches .env / CI (TWITCH_CLIENTID); the cache var keeps the
# same name. FORCE-when-present so a re-configure with the credential exported
# overrides a stale empty cache value.
set(TWITCH_CLIENTID "$ENV{TWITCH_CLIENTID}" CACHE STRING "Twitch OAuth client id (public; device-code flow)")
if(DEFINED ENV{TWITCH_CLIENTID})
  set(TWITCH_CLIENTID "$ENV{TWITCH_CLIENTID}" CACHE STRING "Twitch OAuth client id (public; device-code flow)" FORCE)
endif()
set(TWITCH_HASH "$ENV{TWITCH_HASH}" CACHE STRING "Obfuscation key for TWITCH_CLIENTID; 0 means plaintext")
if(NOT TWITCH_HASH)
  set(TWITCH_HASH "0")
endif()

include(cmake/feature-twitch.cmake)

# Kick and YouTube OAuth run through the broker (no baked client_id/secret); the
# feature files enable them unconditionally.
include(cmake/feature-kick.cmake)
include(cmake/feature-youtube.cmake)

# OAuth broker base URL (public, not a secret). Defaults to production; override with the
# BRAIDCAST_BROKER_URL env var for self-host / dev builds.
if(DEFINED ENV{BRAIDCAST_BROKER_URL} AND NOT "$ENV{BRAIDCAST_BROKER_URL}" STREQUAL "")
  set(BRAIDCAST_BROKER_URL "$ENV{BRAIDCAST_BROKER_URL}" CACHE STRING "Braidcast OAuth broker base URL")
else()
  set(BRAIDCAST_BROKER_URL "https://auth.braidcast.com" CACHE STRING "Braidcast OAuth broker base URL")
endif()

# Generate ui-config.h into a dedicated dir and put it on the include path so
# provider_creds.cpp can #include "ui-config.h".
set(_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(cmake/ui-config.h.in "${_generated_dir}/ui-config.h")
target_include_directories(${_target} PRIVATE "${_generated_dir}")

# The bridge envelope uses nlohmann/json, a header-only lib shipped in the
# obs-deps prefix include dir (the legacy frontend gets it transitively via Qt;
# this non-Qt exe must add it explicitly). Add every CMAKE_PREFIX_PATH/include.
foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
  if(EXISTS "${_prefix}/include/nlohmann/json.hpp")
    target_include_directories(${_target} PRIVATE "${_prefix}/include")
  endif()
endforeach()

# libcurl backs the bridge HTTP client (Phase 8a OAuth + platform-API calls).
find_package(CURL REQUIRED)

add_dependencies(${_target} libcef_dll_wrapper)
target_link_libraries(
  ${_target}
  PRIVATE
    libcef_lib
    libcef_dll_wrapper
    ${CEF_STANDARD_LIBS}
    OBS::libobs
    OBS::frontend-api
    CURL::libcurl
    # IFileDialog (shobjidl) lives in ole32/shell32; SHCreateItemFromParsingName in shlwapi.
    ole32
    shell32
    shlwapi
    # DPAPI (CryptProtectData/CryptUnprotectData) wraps the OAuth token store.
    crypt32
    # The embedded MCP server's HttpServer + the PKCE-loopback auth listener use Winsock.
    ws2_32
    # CNG (BCryptGenRandom/SHA-256) backs the PKCE verifier/challenge + CSRF state.
    bcrypt
    # DwmExtendFrameIntoClientArea (frameless drop shadow) + SetWindowSubclass
    # (drag-region hit-test fall-through) for the custom title bar.
    dwmapi
    comctl32
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
  src/obs_importer.cpp
  src/GeneralSettings.cpp
  src/AdvancedSettings.cpp
  src/bridge.cpp
  src/properties_serializer.cpp
  src/preview_window.cpp
  src/projector_window.cpp
  src/interact_window.cpp
  src/audio/AudioMonitor.cpp
  src/scene_collections.cpp
  src/scene_persistence.cpp
  src/tray.cpp
  src/transitions.cpp
  src/ingest_writeback.cpp
  api-impl/frontend_callbacks.cpp
  utility/CanvasDefinition.cpp
  utility/OBSCanvas.cpp
  utility/CanvasSceneLink.cpp
  utility/OutputBinding.cpp
  utility/StreamProfile.cpp
  src/events/event_store.cpp
  src/overlay/overlay_store.cpp
  src/multistream/CanvasStore.cpp
  src/multistream/StreamProfileStore.cpp
  src/multistream/OutputBindingStore.cpp
  src/multistream/SceneLinkStore.cpp
  src/multistream/MultistreamEngine.cpp
  src/multistream/CanvasRuntime.cpp
  src/multistream/CanvasService.cpp
  src/multistream/GlobalAudioChannels.cpp
  src/multistream/StreamMetaStore.cpp
  src/multistream/Hotkeys.cpp
  src/multistream/VirtualCamManager.cpp
  src/multistream/StorePaths.cpp
  src/mcp/McpServer.cpp
  PROPERTIES COMPILE_OPTIONS "/wd4201"
)

# Place the exe in the OBS rundir (bin/64bit) like other targets; keep a distinct
# OUTPUT_NAME so it does not collide with the legacy obs64 in a shared rundir
# during the transition. It still reads the shared braidcast config dir.
set_target_properties_obs(${_target} PROPERTIES FOLDER frontend OUTPUT_NAME braidcast)

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
set(_web_install "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_DATA_DESTINATION}/braidcast/web")

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
