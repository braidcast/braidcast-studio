cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_PLUGINS "Enable building OBS plugins" ON)

if(NOT ENABLE_PLUGINS)
  set_property(GLOBAL APPEND PROPERTY OBS_FEATURES_DISABLED "Plugin Support")
  return()
endif()

set_property(GLOBAL APPEND PROPERTY OBS_FEATURES_ENABLED "Plugin Support")

macro(require_submodule name)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${name}/CMakeLists.txt")
    message(FATAL_ERROR "Required submodule '${name}' not available.")
  endif()
endmacro()

macro(check_obs_browser)
  if((OS_WINDOWS AND CMAKE_VS_PLATFORM_NAME MATCHES "(ARM64|x64)") OR OS_MACOS OR OS_LINUX)
    require_submodule(obs-browser)
    add_subdirectory(obs-browser)
  else()
    add_custom_target(obs-browser)
    target_disable(obs-browser)
  endif()
endmacro()

# Composite Blur (FiniteSingularity/obs-composite-blur) builds through upstream's own CMakeLists, whose in-tree branch
# calls set_target_properties_obs, so the DLL and data/ install like any other plugin. A function, not a macro, so the
# compile warnings-as-errors override stays scoped to it: its compiler warnings are upstream's to fix. Linker /WX from
# cmake/windows/compilerconfig.cmake still applies. Windows only, as the app is.
function(check_obs_composite_blur)
  if(OS_WINDOWS)
    require_submodule(obs-composite-blur)
    set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
    add_subdirectory(obs-composite-blur)
  endif()
endfunction()

macro(check_obs_websocket)
  require_submodule(obs-websocket)
  add_subdirectory(obs-websocket)
endmacro()

# Add plugins in alphabetical order to retain order in IDE projects
add_obs_plugin(
    aja
    PLATFORMS WINDOWS MACOS LINUX
    WITH_MESSAGE
)
add_obs_plugin(aja-output-ui PLATFORMS WINDOWS MACOS LINUX WITH_MESSAGE)
add_obs_plugin(coreaudio-encoder PLATFORMS WINDOWS MACOS)
add_obs_plugin(
    decklink
    PLATFORMS WINDOWS MACOS LINUX
    WITH_MESSAGE
)
add_obs_plugin(
    decklink-captions
    PLATFORMS WINDOWS MACOS LINUX
    WITH_MESSAGE
)
add_obs_plugin(
    decklink-output-ui
    PLATFORMS WINDOWS MACOS LINUX
    WITH_MESSAGE
)
add_obs_plugin(
    frontend-tools
    PLATFORMS WINDOWS MACOS LINUX
    WITH_MESSAGE
)
add_obs_plugin(image-source)
add_obs_plugin(linux-alsa PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(linux-capture PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(linux-jack PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(linux-pipewire PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(linux-pulseaudio PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(linux-v4l2 PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(mac-avcapture PLATFORMS MACOS)
add_obs_plugin(mac-capture PLATFORMS MACOS)
add_obs_plugin(mac-syphon PLATFORMS MACOS)
add_obs_plugin(mac-videotoolbox PLATFORMS MACOS)
add_obs_plugin(mac-virtualcam PLATFORMS MACOS)
add_obs_plugin(nv-filters PLATFORMS WINDOWS)

check_obs_browser()
check_obs_composite_blur()

add_obs_plugin(obs-ffmpeg)
add_obs_plugin(obs-filters)
add_obs_plugin(obs-libfdk)
add_obs_plugin(obs-nvenc PLATFORMS WINDOWS LINUX ARCHITECTURES x64 x86_64)
add_obs_plugin(obs-outputs)
add_obs_plugin(
    obs-qsv11
    PLATFORMS WINDOWS LINUX
    ARCHITECTURES x64 x86_64
)
add_obs_plugin(obs-text PLATFORMS WINDOWS)
add_obs_plugin(obs-transitions)
add_obs_plugin(
    obs-vst
    PLATFORMS WINDOWS MACOS LINUX
    WITH_MESSAGE
)
add_obs_plugin(obs-webrtc)

check_obs_websocket()

add_obs_plugin(obs-x264)
add_obs_plugin(oss-audio PLATFORMS FREEBSD OPENBSD)
add_obs_plugin(rtmp-services)
add_obs_plugin(sndio PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(text-freetype2)
add_obs_plugin(vlc-video WITH_MESSAGE)
add_obs_plugin(win-capture PLATFORMS WINDOWS)
add_obs_plugin(win-dshow PLATFORMS WINDOWS)
add_obs_plugin(win-wasapi PLATFORMS WINDOWS)
