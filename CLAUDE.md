# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

This is the **OBS Studio** codebase (a fork tracking `obsproject/obs-studio`). OBS Studio captures, composites, encodes, records, and streams video. It is C/C++ (core + plugins) with a Qt 6 desktop frontend, built with CMake. Licensed GPLv2+.

The fork is rebranded **Braidcast**; its repo/folder is **braidcast-studio** (GitHub `braidcast/braidcast-studio`). The old folder name "obs-multistream" referred to OBS's built-in **Multitrack Video** (GoLive) feature — Braidcast's own native multistreaming is canvas-based instead (see "Fork-specific architecture"). (Note: this file's "Qt 6 frontend" description below is pre-Phase-4; the frontend has since been rewritten to a CEF-hosted Svelte UI — see `docs/roadmap.md` Phase 4. A fuller CLAUDE.md refresh is owed.)

## Architecture

Four layers, each compiled separately and wired together at runtime via the module loader:

- **`libobs/`** — the core engine, plain C. Defines the public API (`obs.h`) and opaque object model: **sources** (inputs/filters/transitions), **outputs**, **encoders**, **services**, **scenes/scene-items**, **canvases**, plus the audio (`obs-audio*.c`) and video (`obs-video*.c`) pipelines, data/settings (`obs-data.c`), modules (`obs-module.c`), and hotkeys. `obs-internal.h` holds private struct layouts. This layer has no UI and no Qt.
- **Graphics backends** — `libobs-d3d11/` (Windows), `libobs-opengl/` (Linux/cross), `libobs-metal/` (macOS), plus `libobs-winrt/` (Windows capture helpers). Loaded by libobs as graphics modules; selected per-platform in the root `CMakeLists.txt`.
- **`plugins/`** — each subdirectory is an independently loadable module registering sources/encoders/outputs/services with libobs (e.g. `obs-ffmpeg`, `obs-x264`, `obs-nvenc`, `win-capture`, `mac-capture`, `linux-pipewire`, `rtmp-services`, `obs-outputs`). Three are git submodules: `obs-browser`, `obs-websocket`, and `win-dshow`'s `libdshowcapture` (under `deps/`).
- **`frontend/`** — the Qt desktop app. `OBSApp` (in `OBSApp.cpp`) is the `QApplication`; `OBSStudioAPI.cpp` implements the frontend C API that plugins call. The main window is **`OBSBasic`** in `frontend/widgets/`, deliberately split across many `OBSBasic_*.cpp` partial-implementation files by concern (`OBSBasic_Scenes.cpp`, `OBSBasic_Streaming.cpp`, `OBSBasic_Recording.cpp`, `OBSBasic_Transitions.cpp`, etc.) — when changing main-window behavior, find the partial that owns that concern rather than assuming one giant file. Settings UI is in `frontend/settings/`, dockable panels in `frontend/docks/`.

`docs/sphinx/` (`backend-design.rst`, `graphics.rst`, `frontends.rst`) documents the API design — consult it before changing libobs public surfaces.

## Fork-specific architecture — native multistream

The reason this fork exists. On top of stock OBS, the frontend adds a layer for streaming one composited scene to several destinations at once via **multiple canvases + output multiplexing**. This code is NOT in upstream OBS and is the most likely place for new work. It lives in `frontend/utility/`, `frontend/docks/`, `frontend/settings/`, and `frontend/widgets/OBSBasic_Canvases.cpp`. Three-layer data model:

- **Canvas** = an independent encode target (resolution/FPS). `CanvasDefinition` (data), `CanvasManager` (registry), `OBSCanvas` (libobs binding), `CanvasSceneLink` (scene↔canvas association). Edited via `CanvasEditorDialog`; configured in the Settings → **Canvas** tab (`OBSBasicSettings_Canvas.cpp`).
- **Stream profile** = a reusable destination credential (platform + key), stored **globally** in `streams.json`, shared across scene collections. Configured in the Settings → **Streams** tab.
- **Output binding** = a (stream-profile × canvas) pairing, stored **per scene-collection** in `output_bindings`. `OutputBinding` (`frontend/utility/OutputBinding.{cpp,hpp}`); configured in the Settings → **Outputs** tab. `OutputBindings::AnyEnabledForCanvas` gates whether a canvas renders/encodes.

The **fan-out engine** is `MultistreamOutput` (`frontend/utility/`): encode-once per canvas, then multiplex to every enabled output bound to it. The **Multistream dock** (`frontend/docks/MultistreamDock.cpp`) shows per-output live status; **per-canvas preview docks** are `CanvasDock` (`frontend/docks/CanvasDock.cpp`), which hosts an `OBSBasicPreview` parameterized by `targetCanvas` so each dock edits its own canvas's scene (drag/select/transform scoped per-canvas; null `targetCanvas` = the unchanged central preview).

`MultistreamOutput::IsCanvasLive` guards live-edit hazards (refuses canvas-video resets while an output is live). Stock single-stream Output/Video Settings tabs and the GoLive/Multitrack path are deliberately left dormant — the default Canvas now drives the main pipeline. Roadmap and phase status live in `docs/roadmap.md`.

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
- **Flatpak manifest** — `python3 ./build-aux/format-manifest.py com.braidcast.Braidcast.json`.

## Tests

Tests are minimal and CMake-driven (`test/` → `cmocka/`, `test-input/`). `test/test-input` builds as a plugin via the root `CMakeLists.txt`. There is no large unit-test suite or single-test runner; verification is primarily build + manual.

## Contribution conventions (from CONTRIBUTING.md)

- **Commit titles use a module prefix**, not Conventional Commits: `libobs:`, `obs-ffmpeg:`, `frontend:`, `plugins:`, `cmake:`, `CI:`. Follow **50/72** (title ≤ 50 chars, body wrapped at 72). The body must explain *why*. American English throughout.
- Each commit should be a self-contained "unit of change" that leaves the project buildable.
- Preserve original authorship when finishing someone's work (`Co-authored-by:` / cherry-pick).
- **AI policy:** CONTRIBUTING.md states all submitted content must be human-written and heavily discourages AI-assisted code. Upstream may reject or ban AI-generated submissions. Treat any generated change as a draft requiring full human review before it goes near the upstream project.
