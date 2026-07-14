# OBS CMake Windows CPack configuration module

include_guard(GLOBAL)

include(cpackconfig_common)

# Add GPLv2 license file to CPack (shown as the NSIS license page too)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/frontend/data/license/gplv2.txt")
set(CPACK_PACKAGE_VERSION "${OBS_VERSION_CANONICAL}")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-windows-${CMAKE_VS_PLATFORM_NAME}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY FALSE)

# Ship both a portable archive (ZIP) and a system installer (NSIS). The two
# packages must contain the identical file set; CPACK_MONOLITHIC_INSTALL forces
# every generator to install the full non-EXCLUDE_FROM_ALL tree (the Runtime
# component: exe, dependency DLLs, CEF runtime, Svelte web bundle) as one flat
# payload, so the installer can never ship thinner than the zip.
set(CPACK_GENERATOR ZIP NSIS)
set(CPACK_MONOLITHIC_INSTALL TRUE)
set(CPACK_THREADS 0)

# --- NSIS installer (system install; the zip stays the portable option) -------
# Stable, versionless install dir under 64-bit Program Files so re-running the
# installer for a new version upgrades in place; ENABLE_UNINSTALL_BEFORE_INSTALL
# removes the previous version first for a clean replace.
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Braidcast")
set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_MODIFY_PATH OFF)

# Display / Add-Remove-Programs metadata.
set(CPACK_PACKAGE_VENDOR "Braidcast")
set(CPACK_NSIS_PACKAGE_NAME "Braidcast")
set(CPACK_NSIS_DISPLAY_NAME "Braidcast")
set(CPACK_NSIS_HELP_LINK "https://braidcast.com")
set(CPACK_NSIS_URL_INFO_ABOUT "https://braidcast.com")
set(CPACK_NSIS_CONTACT "https://braidcast.com")

# Installer / uninstaller MUI icons, plus the ARP DisplayIcon and the icon the
# shortcuts inherit -- all reuse the app icon (same .ico the exe embeds).
set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/frontend/cmake/windows/braidcast.ico")
set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/frontend/cmake/windows/braidcast.ico")
set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\64bit\\\\braidcast.exe")

# Offer to launch the app from the installer finish page. CPack hardcodes a
# "$INSTDIR\bin\" prefix for this value, so it is given relative to bin/ to reach
# the nested OBS rundir layout: the result is $INSTDIR\bin\64bit\braidcast.exe.
set(CPACK_NSIS_MUI_FINISHPAGE_RUN "64bit\\\\braidcast.exe")

# Start-Menu + Desktop shortcuts. CPACK_PACKAGE_EXECUTABLES cannot be used: it
# assumes the exe sits directly in bin/, but the fork keeps OBS's bin/64bit
# layout, so the shortcuts are created explicitly to the real exe path.
set(
  CPACK_NSIS_CREATE_ICONS_EXTRA
  "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Braidcast.lnk' '$INSTDIR\\\\bin\\\\64bit\\\\braidcast.exe'
   CreateShortCut '$DESKTOP\\\\Braidcast.lnk' '$INSTDIR\\\\bin\\\\64bit\\\\braidcast.exe'"
)
set(
  CPACK_NSIS_DELETE_ICONS_EXTRA
  "Delete '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Braidcast.lnk'
   Delete '$DESKTOP\\\\Braidcast.lnk'"
)

include(CPack)
