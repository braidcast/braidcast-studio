# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

This is the **OBS Studio** codebase (a fork tracking `obsproject/obs-studio`). OBS Studio captures, composites, encodes, records, and streams video. It is C/C++ (core + plugins), built with CMake, Licensed GPLv2+. The desktop app frontend is a **CEF-hosted Svelte 5 web UI** — not Qt (see "Frontend" below); the original Qt Widgets frontend was fully removed from the tree, not just retired.

The fork is rebranded **Braidcast**; its repo/folder is **braidcast-studio** (GitHub `braidcast/braidcast-studio`). The old folder name "obs-multistream" referred to OBS's built-in **Multitrack Video** (GoLive) feature — Braidcast's own native multistreaming is canvas-based instead (see "Fork-specific architecture"). This is a **private fork, not submitted upstream**.

## Architecture

Layers, each compiled separately and wired together at runtime via the module loader:

- **`libobs/`** — the core engine, plain C. Defines the public API (`obs.h`) and opaque object model: **sources** (inputs/filters/transitions), **outputs**, **encoders**, **services**, **scenes/scene-items**, **canvases**, plus the audio (`obs-audio*.c`) and video (`obs-video*.c`) pipelines, data/settings (`obs-data.c`), modules (`obs-module.c`), and hotkeys. `obs-internal.h` holds private struct layouts. This layer has no UI and no Qt/CEF.
- **Graphics backends** — `libobs-d3d11/` (Windows), `libobs-opengl/` (Linux/cross), `libobs-metal/` (macOS), plus `libobs-winrt/` (Windows capture helpers). Loaded by libobs as graphics modules; selected per-platform in the root `CMakeLists.txt`.
- **`plugins/`** — each subdirectory is an independently loadable module registering sources/encoders/outputs/services with libobs (e.g. `obs-ffmpeg`, `obs-x264`, `obs-nvenc`, `win-capture`, `mac-capture`, `linux-pipewire`, `rtmp-services`, `obs-outputs`). Three are git submodules: **`obs-browser`** (forked as `braidcast/braidcast-browser`, not upstream OBS's), `obs-websocket`, and `win-dshow`'s `libdshowcapture` (under `deps/`). Some plugins (`obs-browser` panels, `frontend-tools`, `decklink-output-ui`, `decklink-captions`, `aja-output-ui`) are still Qt-coupled internally and are curated/denylisted out of the headless load (see "Frontend" below); `obs-websocket` currently hard-crashes headless for the same reason and is excluded until it's addressed.
- **`frontend/`** — the CEF-hosted Svelte app. `frontend/src/` is the C++ host/bridge/engine layer; `frontend/web/` is the Svelte 5 (runes) UI, built with Vite via `bun`. `frontend/utility/` holds the shared, Qt-free native data-model classes (`CanvasDefinition`, `OBSCanvas`, `CanvasSceneLink`, `OutputBinding`, `StreamProfile`, `UndoManager`) that `frontend/src/` builds on. `frontend/api/` + `frontend/api-impl/` implement a headless, non-Qt `obs_frontend_callbacks` shim (`obs-frontend-api`) so plugins that call into frontend callbacks can still load without a Qt frontend present. See "Frontend — CEF host + Svelte UI" below for the full picture.

`docs/sphinx/` (`backend-design.rst`, `graphics.rst`, `frontends.rst`) documents the API design — consult it before changing libobs public surfaces. `docs/roadmap.md` is the authoritative build log for everything fork-specific (phase-by-phase status, design decisions, what's deferred vs. shipped) — read it before assuming a feature is missing or planned.

## Frontend — CEF host + Svelte UI

The reason the old Qt frontend is gone: Phase 4 (`docs/roadmap.md`) replaced it with **CEF as the whole UI shell** hosting a **Svelte 5** app, keeping libobs and the fork's native-multistream engine unchanged underneath. One executable hosts every CEF process.

- **`frontend/src/main.cpp`** creates the host window and the CEF browser (embedded as a child of a native win32 window, not via `OnContextInitialized`), then drives startup/shutdown (`ObsBootstrap::Start`/`Stop`), the preview/projector/interact windows, and the headless-smoke self-test paths (env-gated: `FE_SMOKE_QUIT_SECONDS` auto-terminates a run; `BRAIDCAST_SELFTEST_STREAM` drives specific self-test state machines).
- **`frontend/src/app.cpp`/`.hpp`** — `App` implements `CefApp`/`CefBrowserProcessHandler`/`CefRenderProcessHandler` in the same binary (both the browser and renderer process role) and registers the custom `app://` scheme before `CefInitialize`.
- **`frontend/src/scheme.cpp`/`.hpp`** — serves the offline Svelte bundle at `app://app/` from the rundir's `data/braidcast/web/` directory (the built `frontend/web/dist`).
- **`frontend/src/bridge.cpp`/`.hpp`** — the JS↔C++ contract the UI runs on: a data-driven method registry (`g_methods`, ~80 operations — `scenes.*`, `sources.*`, `canvas.*`, `streams.*`, `outputBindings.*`, `multistream.*`, `properties.*`, `filters.*`, `transitions.*`, `sceneItems.*`, `audio.*`, `dialog.*`, `window.*`, …). JS calls `window.obs.call(method, params)` (routed through `CefMessageRouter`/`cefQuery` to `Bridge::Dispatch`); C++ pushes server events via `Bridge::EmitEvent` → `window.__obsEmit`. Adding a bridge method is a registry insertion in `bridge.cpp`, not a new branch.
- **`frontend/src/obs_bootstrap.cpp`/`.hpp`** — owns libobs startup (core, D3D11 video, audio, curated module load) and shutdown (drains the deferred-destroy queue before `obs_shutdown`), and owns/exposes every top-level engine singleton the bridge drives: `SceneCollections`, `CanvasStore` (`Canvases()`), `StreamProfileStore`, `OutputBindingStore`, `SceneLinkStore`, `MultistreamEngine` (`Multistream()`), `CanvasRuntime`, `CanvasService`, `AudioMonitor`, `UndoManager`, `VirtualCamManager`, `GlobalAudioChannels`, `StreamMetaStore`, `GeneralSettings`, `AdvancedSettings`. Each is a single shared instance threaded down, constructed in `Start()` and torn down in `Stop()`.
- **`frontend/api/` + `frontend/api-impl/`** — the headless `obs-frontend-api` shim (`HeadlessFrontendCallbacks`) that lets frontend-callback-dependent plugins load without Qt; a curated module allowlist/denylist keeps out the plugins that are still hard-Qt-coupled (see Architecture above).
- **`frontend/src/oauth/`** — the OAuth broker (`broker_strategy`, `registry`, `account_store`) plus per-platform providers (`twitch_provider`, `kick_provider`, `youtube_provider`), talking to `auth.braidcast.com`. **`frontend/src/chat/`** and **`frontend/src/events/`** are the corresponding per-platform chat/events transports (IRC/Pusher/EventSub over WebSocket) feeding the chat hub and event store.
- **`frontend/src/mcp/`** (`HttpServer`, `McpServer`) — an opt-in, localhost-only embedded MCP server exposing curated OBS control tools (scene/source/canvas/stream control) over the same bridge method registry, for AI-agent control of the running app (Phase 5).
- **`frontend/src/overlay/`** — an embedded HTTP+SSE server (`overlay_server`) serving browser-source stream overlays (alertbox/chatbox/goalbar/labels/ticker) from `overlay_store` state.
- **`frontend/src/preview_window.cpp`**, **`projector_window.cpp`**, **`interact_window.cpp`** — native `obs_display`-backed windows (per-canvas preview surfaces embedded as child HWNDs over the CEF UI, projector windows, source-interact windows), not Qt widgets.

The Qt Widgets frontend (`OBSApp`, `OBSBasic`, `frontend/widgets/`, `frontend/docks/`, `frontend/settings/`, and the `frontend_old/` staging tree it was moved to during the migration) is **gone, not dormant** — deleted outright once the CEF/Svelte frontend reached parity (commit `9aa092af3`, "chore: remove dead legacy Qt frontend (frontend_old)"). Qt6 is still fetched as a build dependency only because some plugins (e.g. `obs-browser`'s optional panel support, `frontend-tools`, DeckLink/AJA UI plugins) are still internally Qt-coupled; the app frontend itself has no Qt dependency.

## Fork-specific architecture — native multistream

The reason this fork exists. On top of stock OBS, the frontend adds a layer for streaming one composited scene to several destinations at once via **multiple canvases + output multiplexing**. This code is NOT in upstream OBS. It lives in `frontend/src/multistream/` (engine/runtime/stores) plus the shared data types in `frontend/utility/`. Three-layer data model, unchanged in spirit from the original Qt-era design but now implemented Qt-free:

- **Canvas** = an independent encode target (resolution/FPS/color/encoders). `CanvasDefinition` (`frontend/utility/`, data), `CanvasStore` (`frontend/src/multistream/CanvasStore.{cpp,hpp}` — global registry, persisted to `canvases.json`, always has exactly one `isDefault` definition), `CanvasRuntime` (`frontend/src/multistream/CanvasRuntime.{cpp,hpp}` — owns the live `obs_canvas_t` mixes for every non-Default canvas), `CanvasService` (`frontend/src/multistream/CanvasService.{cpp,hpp}` — the canvas-update/reconciliation domain: structural-field diff, live-edit refusal gate, Default-canvas→global-video coupling, commit-then-reset ordering, inheritor-aware encoder-cache invalidation). `CanvasSceneLink` (`frontend/utility/`) is the uuid-keyed activation-sync map (a non-default canvas scene "follows" a main/Default scene), persisted per scene-collection via `SceneLinkStore`.
- **Stream profile** = a reusable destination credential (platform + key), stored **globally** in `streams.json` via `StreamProfileStore` (`frontend/src/multistream/StreamProfileStore.{cpp,hpp}`), shared across scene collections.
- **Output binding** = a (stream-profile × canvas) pairing, stored **per scene-collection**. `OutputBinding` (`frontend/utility/`), persisted via `OutputBindingStore` (`frontend/src/multistream/OutputBindingStore.{cpp,hpp}`); `OutputBindings::AnyEnabledForCanvas` gates whether a canvas renders/encodes at all (an output-gated preview only exists once its canvas has ≥1 enabled binding).

The **fan-out engine** is `MultistreamEngine` (`frontend/src/multistream/MultistreamEngine.{cpp,hpp}`): encode-once per canvas, then one native `obs_output_t` per enabled binding, driven by that binding's stream profile's `obs_service`, fanned out from the shared per-canvas encoder pair. `MultistreamEngine::IsCanvasLive` guards live-edit hazards (`CanvasService` refuses structural canvas edits — resolution/fps/color/encoder — while an output bound to that canvas is live, since a reset would free a mix a live encoder still references). All of this is driven from the Svelte UI over the bridge (`canvas.*`, `streams.*`, `outputBindings.*`, `multistream.*` methods) rather than from Qt docks/settings tabs.

Other multistream-adjacent stores in `frontend/src/multistream/`: `GlobalAudioChannels` (WASAPI desktop/mic capture on output channels 1–6), `Hotkeys` (libobs hotkey binding/persistence), `VirtualCamManager` (virtual-camera output fed from a chosen canvas), `StreamMetaStore` (remembered per-account/per-profile stream metadata defaults), `StorePaths` (shared config-file path resolution).

Stock single-stream Output/Video Settings and the GoLive/Multitrack path are deliberately left dormant — the Default canvas drives the main pipeline. Recording/replay buffer are also kept dormant by design (streaming-only fork). Roadmap and phase status live in `docs/roadmap.md`.

## Build

CMake **presets** drive everything (`CMakePresets.json`); there is no plain `cmake .` flow. Requires CMake ≥ 3.28, Qt 6 (still fetched for the Qt-coupled plugin bits noted above, not for the app frontend itself), CEF, prebuilt `obs-deps` that CI fetches per the `dependencies` vendor block in `CMakePresets.json`, and **`bun`** on `PATH` (required — no npm/pnpm/yarn fallback — for the Svelte web bundle).

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

Build output goes to `build_x64/` / `build_arm64/` / `build_macos/` / `build_ubuntu/`. The `*-ci` presets (`windows-ci-x64`, `ubuntu-ci`, `macos-ci`) add `-Werror` and are what CI runs. The CI driver scripts are `.github/scripts/Build-Windows.ps1` (requires `$env:CI` set + PowerShell 7), `build-ubuntu`, and `build-macos`. Key CMake options: `ENABLE_FRONTEND` (build `frontend/`, the CEF/Svelte app — despite the option's inherited "requires Qt" help text, the frontend itself is Qt-free), `ENABLE_SCRIPTING`, `ENABLE_HEVC`.

The Svelte web bundle builds as part of the frontend target: CMake runs `bun install && bun run build` from `frontend/web/` (an always-run, incremental `frontend-web` custom target) and copies `frontend/web/dist` into the rundir's data directory, served from there via the `app://` scheme handler.

## Formatting (enforced by CI — `check-format.yaml`)

The project pins exact formatter versions. CI rejects PRs that don't match. Column limit is **120 chars**.

- **C/C++/ObjC** — `clang-format` **exactly 19.1.1**. Run `./build-aux/run-clang-format` (ZSH). Note: the codebase enforces braces on all control statements (see `.clang-format` / recent history).
- **CMake** — `gersemi` (Python). Run `./build-aux/run-gersemi`.
- **Swift** — `swift-format`. Run `./build-aux/run-swift-format`.
- **Frontend (TypeScript/Svelte)** — from `frontend/web/`, run `bun run check` (svelte-check) and `bun run build` before submitting; the Vite build catches type issues `check` alone can miss.
- **Flatpak manifest** — `python3 ./build-aux/format-manifest.py com.braidcast.Braidcast.json`.

## Tests

Backend tests are minimal and CMake-driven (`test/` → `cmocka/`, `test-input/`). `test/test-input` builds as a plugin via the root `CMakeLists.txt`. There is no large unit-test suite; verification is primarily build + manual/headless-smoke.

The CEF/Svelte frontend's primary verification path is a **headless smoke run**: set `FE_SMOKE_QUIT_SECONDS=<n>` to auto-terminate the app after N seconds (automatable log capture), which exercises a battery of self-test functions declared in `frontend/src/obs_bootstrap.hpp` (`Run*SelfTest`, one per feature area — properties, preview editing, settings, canvas/stream-profile/output-binding bridge round-trips, the multistream engine, scene isolation, transforms, audio mixer, projectors, hotkeys, stats, the embedded MCP server, events, overlays). Each self-test restores any state it touches so a smoke run leaves the user's real config files unchanged. `BRAIDCAST_SELFTEST_STREAM=<mode>` drives additional targeted state machines (e.g. `perf-repro`). GUI-only acceptance items (drag/drop, live-canvas edits, real broadcasts) are tracked as "owed" per-feature in `docs/roadmap.md` — they can't be driven headlessly.

## Contribution conventions (from CONTRIBUTING.md)

- **Commit titles use a module prefix**, not Conventional Commits: `libobs:`, `obs-ffmpeg:`, `frontend:`, `plugins:`, `cmake:`, `CI:`. Follow **50/72** (title ≤ 50 chars, body wrapped at 72). The body must explain *why*. American English throughout.
- Each commit should be a self-contained "unit of change" that leaves the project buildable.
- Preserve original authorship when finishing someone's work (`Co-authored-by:` / cherry-pick).
- **AI policy:** treat any AI-assisted change as a draft requiring full human review before it's considered final — the same discipline upstream OBS applies to AI-generated submissions. This fork's own `CONTRIBUTING.md` is currently silent on AI authorship specifically (it no longer carries upstream's human-written-only language verbatim), but since this is a private, non-upstreamed fork today, treat the conservative human-review-first posture as the working policy regardless; if code from here is ever submitted upstream, upstream's own stricter policy governs there.
