cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_VST "Enable building OBS with VST plugin" ON)

if(NOT ENABLE_VST)
  target_disable(obs-vst)
  return()
endif()

add_library(obs-vst MODULE)
add_library(OBS::vst ALIAS obs-vst)

target_sources(
  obs-vst
  PRIVATE
    headers/EditorWidget.h
    headers/vst-plugin-callbacks.hpp
    headers/VSTPlugin.h
    obs-vst.cpp
    vst_header/aeffectx.h
    VSTPlugin.cpp
)

target_include_directories(obs-vst PRIVATE vst_header)

target_link_libraries(obs-vst PRIVATE OBS::libobs)

if(OS_WINDOWS)
  # The Braidcast CEF build ships no Qt6, so the Windows VST editor is hosted in
  # a raw Win32 window (win/EditorWidget-win.cpp) and this module links no Qt.
  target_sources(obs-vst PRIVATE win/EditorWidget-win.cpp win/VSTPlugin-win.cpp)
  target_link_libraries(obs-vst PRIVATE bcrypt)
else()
  find_package(Qt6 REQUIRED Widgets)
  target_sources(
    obs-vst
    PRIVATE
      EditorWidget.cpp
      $<$<PLATFORM_ID:Darwin>:mac/EditorWidget-osx.mm>
      $<$<PLATFORM_ID:Darwin>:mac/VSTPlugin-osx.mm>
      $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:linux/EditorWidget-linux.cpp>
      $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:linux/VSTPlugin-linux.cpp>
  )
  target_link_libraries(
    obs-vst
    PRIVATE
      Qt::Widgets
      "$<$<PLATFORM_ID:Darwin>:$<LINK_LIBRARY:FRAMEWORK,Cocoa.framework>>"
      "$<$<PLATFORM_ID:Darwin>:$<LINK_LIBRARY:FRAMEWORK,Foundation.framework>>"
  )
  set_target_properties(
    obs-vst
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

set_target_properties_obs(obs-vst PROPERTIES FOLDER plugins PREFIX "")
