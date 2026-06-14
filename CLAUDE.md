# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

This is the **OBS Studio** codebase (a fork tracking `obsproject/obs-studio`). OBS Studio captures, composites, encodes, records, and streams video. It is C/C++ (core + plugins) with a Qt 6 desktop frontend, built with CMake. Licensed GPLv2+.

The repo name "obs-multistream" refers to OBS's built-in **Multitrack Video** (a.k.a. GoLive / multi-platform streaming) feature, not a separate product. That code lives in `frontend/utility/` (`MultitrackVideoOutput`, `GoLiveAPI_*`, `BasicOutputHandler`).

## Architecture

Four layers, each compiled separately and wired together at runtime via the module loader:

- **`libobs/`** — the core engine, plain C. Defines the public API (`obs.h`) and opaque object model: **sources** (inputs/filters/transitions), **outputs**, **encoders**, **services**, **scenes/scene-items**, **canvases**, plus the audio (`obs-audio*.c`) and video (`obs-video*.c`) pipelines, data/settings (`obs-data.c`), modules (`obs-module.c`), and hotkeys. `obs-internal.h` holds private struct layouts. This layer has no UI and no Qt.
- **Graphics backends** — `libobs-d3d11/` (Windows), `libobs-opengl/` (Linux/cross), `libobs-metal/` (macOS), plus `libobs-winrt/` (Windows capture helpers). Loaded by libobs as graphics modules; selected per-platform in the root `CMakeLists.txt`.
- **`plugins/`** — each subdirectory is an independently loadable module registering sources/encoders/outputs/services with libobs (e.g. `obs-ffmpeg`, `obs-x264`, `obs-nvenc`, `win-capture`, `mac-capture`, `linux-pipewire`, `rtmp-services`, `obs-outputs`). Three are git submodules: `obs-browser`, `obs-websocket`, and `win-dshow`'s `libdshowcapture` (under `deps/`).
- **`frontend/`** — the Qt desktop app. `OBSApp` (in `OBSApp.cpp`) is the `QApplication`; `OBSStudioAPI.cpp` implements the frontend C API that plugins call. The main window is **`OBSBasic`** in `frontend/widgets/`, deliberately split across many `OBSBasic_*.cpp` partial-implementation files by concern (`OBSBasic_Scenes.cpp`, `OBSBasic_Streaming.cpp`, `OBSBasic_Recording.cpp`, `OBSBasic_Transitions.cpp`, etc.) — when changing main-window behavior, find the partial that owns that concern rather than assuming one giant file. Settings UI is in `frontend/settings/`, dockable panels in `frontend/docks/`.

`docs/sphinx/` (`backend-design.rst`, `graphics.rst`, `frontends.rst`) documents the API design — consult it before changing libobs public surfaces.

## Build

CMake **presets** drive everything (`CMakePresets.json`); there is no plain `cmake .` flow. Requires CMake ≥ 3.28, Qt 6, and prebuilt `obs-deps`/CEF that CI fetches per the `buildspec`/`dependencies` vendor block.

First, fetch submodules:
```
git submodule update --init --recursive
```

Configure + build (pick the preset for your platform):
```
# Windows x64 (also: windows-arm64)
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo

# macOS (Xcode generator)
cmake --preset macos

# Linux (Ninja generator)
cmake --preset ubuntu
cmake --build build_ubuntu
```

Build output goes to `build_x64/` / `build_arm64/` / `build_macos/` / `build_ubuntu/`. The `*-ci` presets (`windows-ci-x64`, `ubuntu-ci`, `macos-ci`) add `-Werror` and are what CI runs. The CI driver scripts are `.github/scripts/Build-Windows.ps1` (requires `$env:CI` set + PowerShell 7), `build-ubuntu`, and `build-macos`. Key CMake options: `ENABLE_FRONTEND` (Qt UI), `ENABLE_SCRIPTING`, `ENABLE_HEVC`, `ENABLE_BROWSER`.

## Formatting (enforced by CI — `check-format.yaml`)

The project pins exact formatter versions. CI rejects PRs that don't match. Column limit is **120 chars**.

- **C/C++/ObjC** — `clang-format` **exactly 19.1.1**. Run `./build-aux/run-clang-format` (ZSH). Note: the codebase enforces braces on all control statements (see `.clang-format` / recent history).
- **CMake** — `gersemi` (Python). Run `./build-aux/run-gersemi`.
- **Swift** — `swift-format`. Run `./build-aux/run-swift-format`.
- **Flatpak manifest** — `python3 ./build-aux/format-manifest.py com.obsproject.obs-multistream.json`.

## Tests

Tests are minimal and CMake-driven (`test/` → `cmocka/`, `test-input/`). `test/test-input` builds as a plugin via the root `CMakeLists.txt`. There is no large unit-test suite or single-test runner; verification is primarily build + manual.

## Contribution conventions (from CONTRIBUTING.md)

- **Commit titles use a module prefix**, not Conventional Commits: `libobs:`, `obs-ffmpeg:`, `frontend:`, `plugins:`, `cmake:`, `CI:`. Follow **50/72** (title ≤ 50 chars, body wrapped at 72). The body must explain *why*. American English throughout.
- Each commit should be a self-contained "unit of change" that leaves the project buildable.
- Preserve original authorship when finishing someone's work (`Co-authored-by:` / cherry-pick).
- **AI policy:** CONTRIBUTING.md states all submitted content must be human-written and heavily discourages AI-assisted code. Upstream may reject or ban AI-generated submissions. Treat any generated change as a draft requiring full human review before it goes near the upstream project.
