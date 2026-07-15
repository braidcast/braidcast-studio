# Braidcast — Roadmap

Native multistreaming built into this OBS Studio fork via **canvas / output
multiplexing**: canvases (resolution + fps + encoder settings) become the unit
of streaming, each wired to one or more destinations. Built on OBS's existing
`obs_canvas_t` + output primitives rather than a rewrite, to keep the capture
and plugin ecosystem.

This is a **private fork**, not submitted upstream. The app is rebranded
**Braidcast** (forked from OBS Studio, licensed GPLv2+).

Status legend: ✅ done · 🔧 in progress · 🐛 open bug · 🔭 planned · 💭 in design
· ⏸ deferred

---

## Phase 1 — Canvas foundation ✅ COMPLETE

The Default Canvas is the single source of truth for the main video pipeline
and the one streaming output. Streaming-only (1 video + 1 audio encoder per
canvas); recording, replay buffer, and multitrack/GoLive are hidden and inert.

- ✅ **Data model + persistence** — `CanvasDefinition`, `CanvasManager`,
  global `canvases.json`, per-scene-collection `canvas_bindings`.
- ✅ **Settings → Canvas tab** — add/remove canvases (immediate + auto-saved,
  like profiles); Default card pinned, additional-canvas grid.
- ✅ **Canvas editor dialog** — name/resolution/fps + Video | Audio | Advanced
  tabs (`OBSPropertiesView`); "Use Default" inheritance toggles.
- ✅ **Inheritance & color resolution (4a)** — per-field inheritance from the
  Default Canvas resolved at encode time (`ToVideoInfo`).
- ✅ **Output rewire & tab removal (4b)** — Default Canvas drives
  `obs_reset_video` + the stream encoder via an always-Advanced output handler;
  Settings → Output and Video tabs removed; all color settings moved onto the
  canvas; recording/replay/GoLive UI hidden.
- ✅ **Full inert pass (4b T7)** — recording and GoLive made truly dormant
  (GoLive never constructed, no record/replay auto-start or hotkeys, encoder
  seeding validated). See `docs/superpowers/` specs/plans (gitignored).
- ✅ **Settings polish** — Canvas tab moved below Stream; themed Canvas list
  icon across all themes.

### Tooling ✅

- ✅ **`magefile.go`** — `mage build` / `buildAll` / `run` (portable) /
  `format` / `configure` / `deps` / `tag`. Captures the VS-bundled
  cmake/clang-format paths, portable-mode launch, version-tag fix, and the
  Norton TLS-revocation dependency prefetch.

---

## Open bugs 🐛

- ✅ **MOOT — "Use Default resolution" toggle stretched full-width** in the canvas
  editor — the idian `ToggleSwitch` fills its layout cell and reads like a
  slider, though it behaves as a toggle. **Not codeable as written:** the Qt
  frontend was deleted outright (commit `9aa092af3`), so no `ToggleSwitch` and no
  Qt canvas editor exist. The canvas editor is now a Svelte modal (Phase 4/7).
- ✅ **MOOT — Pre-existing `verticalLayout_35` AUTOUIC name-collision warning** —
  cosmetic; uic auto-renames. Gone with the Qt tree (`9aa092af3`) — there is no
  `.ui` file or AUTOUIC step in the build anymore.

## Verification owed (manual, can't be driven headlessly)

- 🔧 **Live streaming** — partially exercised; confirmed Phase 1 streams the
  Default Canvas only (additional canvases not yet wired — see Phase 2).
- 🔭 **GUI visual passes** — Settings tabs, canvas editor, control panel hide
  state across themes.
- 🔭 **2a scene model (runtime)** — code-complete but unverified live: create a
  2nd canvas + scene + current-scene, restart, confirm it survives (no collapse
  onto main); Add Source defaults to the existing source; Settings/plugin-added
  canvases get a default scene. Scene-link sync stays dormant until the 2b UI
  populates the link map.

---

## Phase 2 — Multi-destination 🔧 2a–2e + 2b code-complete, live broadcast owed

The goal: each canvas streams to its own destination(s) simultaneously — one
encode per canvas, fanned out to many platforms (Twitch + YouTube + Kick + …).
This closes the Phase 1 seam where additional canvases are definitions only.

**Model decided: per-canvas scenes over shared sources** (Model 1). Each canvas
owns its own scenes (normal OBS scenes); sources stay global/shared, so a canvas
scene references and positions them. Layout is per-canvas (transform/visibility);
content is shared (properties/filters/name). "Link scene" = activation sync
(switch main program scene → linked canvases follow). Rejected Model 2 (one base
scene + per-canvas overrides) as a high-cost custom layer. This **revises** the
Phase 1-era "canvas references one shared scene" decision (which only held for
same-aspect canvases). Full design: `docs/superpowers/specs/2026-06-14-canvas-
multidestination-design.md`. Surfaced as **dockable canvas windows** (preview +
scene tree + 🔗 link + per-canvas output), all dockable, phased visibility.

SE.Live failure modes explicitly designed against: broken Link scene; broken
canvas CRUD; a non-switchable "primary" destination forcing re-encodes; GPU
blowout/FPS drops from redundant encodes; frame-skips invisible to OBS Stats.
Our fixes: working uuid-keyed link; reuse Phase 1 CanvasManager CRUD; no
privileged primary; one encode per canvas, shared where settings match; native
`obs_output_t` per destination so OBS Stats see everything.

**Build decomposition (each: spec → plan → implement):**

- ✅ **2a — Scene model** — per-canvas scenes over shared sources, scene-link
  activation sync, Add-Source "use existing" default. Plan:
  `docs/superpowers/plans/2026-06-14-canvas-scene-model.md` (built on libobs's
  `obs_canvas_scene_create`/`obs_canvas_set_channel`). **Code-complete**
  (build-green + two-stage reviewed; GUI runtime verification owed). Capstone
  fix: a Phase-1 carry-forward bug had additional canvases created in `OBSInit`
  then destroyed by `ClearSceneData` before the collection loaded, so their
  scenes/bindings restored onto the main canvas. Now recreated inside `LoadData`
  (after the clear, before `obs_load_sources`); bindings applied after sources
  load; default scenes seeded only for still-empty canvases. **2b/2c must keep
  additional-canvas creation in the load path, not `OBSInit`.**
  Phase 2 was re-decomposed into ordered sub-phases 2c → 2d → 2e → 2b; all are
  now on `canvas-foundation`:
- ✅ **2c — Streams tab** — global, reusable stream profiles (`streams.json`),
  master-detail Streams settings page, primary-only mirror into OBS's single
  `obs_service`. Build-green + GUI-verified (persistence, primary mirror,
  Cancel-discards).
- ✅ **2d — Outputs tab** — an output = (stream profile × canvas) + `enabled`,
  persisted per scene-collection (`output_bindings`). Settings → Outputs routing
  page grouped by canvas with a searchable profile dropdown and a single-key
  in-use guard. Build-green + GUI-verified (persist across restart, in-use block).
- ✅ **2e — Multistream engine + dock** — encode-once-per-canvas fan-out
  (`MultistreamOutput`), one `obs_output` per enabled binding driven by its
  profile's `obs_service`, handler-level single-key guard, and a Multistream dock
  (per-canvas groups, status dots, Go Live / Stop All). Build-green + GUI-verified
  to the connection-failure (Error) path; a real sustained broadcast is owed
  (needs live creds — see `docs/issues.md` #2).
- ✅ **2b — Dockable canvas windows** — one dock per additional canvas: a scene
  list + an `OBSQTDisplay` of that canvas's mix, splitter auto-orienting from the
  dock shape, layout persisted by canvas uuid. Build-green + GUI-verified
  (scene add scoped to canvas, persistence across restart, clean teardown).

### Canvas-dock UX iteration (batch 2) ✅ 2026-06-16

Reworked the canvas-preview UX around **Settings → Outputs as the single source
of truth**, plus supporting fixes. All build-green and GUI-verified by the user.

- ✅ **Output-gated previews** — a canvas renders a preview only when it has ≥1
  enabled output binding (`OutputBindings::AnyEnabledForCanvas`, pushed via the
  `OBSBasic::OutputBindingsChanged` signal). The Default canvas keeps the central
  preview and swaps to a **canvas-agnostic** "Preview Disabled — enable a canvas
  in Settings → Outputs" placeholder until enabled.
- ✅ **Additional canvas docks appear-on-enable** — each additional canvas's dock
  is created **floating** only while it has an enabled output and is destroyed
  when disabled, reconciled live off `OutputBindingsChanged`.
- ✅ **Canvas-dock anatomy** — in-dock per-scene source tree, section
  headers/divider/toolbars, a footer (canvas-editor `⚙` + polled live-status `●`),
  and per-scene `🔗` link toggles (Model-1 activation sync to the program scene).
- ✅ **Point-4 considered and reverted** — briefly made the Default preview a
  dockable panel; it collapsed the central widget and starved the layout, so it
  was reverted. Default preview stays the central widget; stock Scenes/Sources
  docks untouched.
- ✅ **Stream-profile duplicate validation** — block a profile that reuses
  another's stream key (exact) or display name (case-insensitive `Platform -
  Name`) at every commit point; fixed the panel button clipping and the
  stale-list-on-Apply.
- ✅ **Canvas editor / cards** — resolution presets up to 4K (landscape +
  vertical, combo still editable); cards show friendly encoder names via
  `obs_encoder_get_display_name`.
- ✅ **Theme cleanup** — removed the dangling `qproperty-videoIcon` (no matching
  Q_PROPERTY since the Video tab was folded into Canvas) that logged on every
  Settings open.

### Hardening & cleanup pass ✅ 2026-06-19

A low-risk cleanup + the high-value items from the Phase-2 holistic review
(`docs/issues.md` #3). All build-green; runtime-unverified items noted.

- ✅ **Cleanup pass** — deduped the State→color status palette into
  `MultistreamOutput::StateColor`; removed dead members (`SetPrimary`,
  `AnyActive`, 2-arg `Unset`) and two dead locale keys.
- ✅ **R1 — `OBSBasic::ForEachStreamableCanvas`** — hoisted the duplicated
  "Default-first then non-ephemeral canvases" walk; routed the two true-dup sites
  (Multistream dock, Outputs tab) through it. The Stream-tab / SceneCollections
  walks were left alone (drifted intent).
- ✅ **C2 — deleted-profile surfaced distinctly** — a binding pointing at a
  deleted stream profile now reads as "profile deleted" (dock row + Outputs badge)
  rather than identically to an unset one.
- ✅ **P2 — incremental dock refresh** — split `MultistreamDock` into structural
  `Refresh` vs. status-only `UpdateStatuses`; status ticks no longer rebuild the
  tree. Also fixed a latent staleness bug (dock didn't refresh on binding edits
  while visible) via a new `OutputBindingsChanged → Refresh` connection.
- ✅ **H1 — live canvas-edit crash guard** — editing an additional canvas's
  resolution while it was live on a multistream output called
  `obs_canvas_reset_video` unguarded (UAF). Added `MultistreamOutput::IsCanvasLive`
  and guarded both reset sites (direct edit + Default-branch follower loop);
  warn/revert gated to actual resolution/FPS changes. The editor dialog also
  **locks** the resolution/FPS inputs while the canvas is live, with the guard as a
  backstop. Runtime-unverified (needs a live output, same constraint as #2).

---

## Phase 3 — Per-canvas editing & studio mode 🔭 PLANNED (not started)

The Phase-2 model lets an additional canvas be *populated* but not visually
*laid out*, and studio mode / the central editable stage are Default-only. Phase
3 closes that. Each item needs a scoping spike before planning — the central
preview (`OBSBasicPreview`) is OBS's most load-bearing widget and assumes the
main program canvas in several places.

- ✅ **3a — Editable canvas-dock previews (keystone) — IMPLEMENTED (build-green +
  reviewed; GUI acceptance owed).** The canvas dock now hosts an `OBSBasicPreview`
  bound to its canvas, so sources in a canvas dock are draggable / selectable /
  stretch / crop / rotate, scoped to that canvas's scene. Done via **Approach A**:
  parameterized `OBSBasicPreview` with a `targetCanvas` member (default `nullptr` =
  the central preview's exact prior behavior); `TargetScene()`/`TargetVideoInfo()`
  route scene + base-resolution, a per-surface `surface*` viewport decouples
  paint/hit-test, and a self-contained `CanvasPreviewRender` draw callback paints
  `obs_canvas_render` + the editing overlays. Plan:
  `docs/superpowers/plans/2026-06-20-editable-canvas-previews.md` (5 tasks, commits
  `69f4d59e`..`cbf227c5`). Per-task spec+quality reviews + a holistic final review
  all passed (null-path invariant, multi-dock isolation, no stale-viewport window).
  **Owed:** the manual GUI acceptance pass (drag isolation across docks, teardown,
  live-canvas edit) — can't be driven headlessly. Out of scope (→ 3b/3d): per-canvas
  undo, Studio Mode, nudge, scrollbars, source-list context menu. Unblocks 3b.
- 🔭 **3b — Per-canvas Studio Mode.** Make preview/program staging + transitions
  work per canvas, not Default-only. Superset of 3a (adds per-canvas transition +
  program staging on top of an editable surface); the hardest item. Do after 3a.
- ✅ **3c — MOOT (superseded by the Phase 4/7 frontend).** Both questions below were
  about the QMainWindow **central widget**; Phase 4 replaced Qt with the CEF/Svelte UI
  and Phase 7 replaced the workspace with the nav-rail page model, so there is no
  central widget to gate or reparent — CSS layout removes the whole constraint class
  (the stated "Why" of Phase 4 at the section below). Original questions kept for
  history:
  - When the Default canvas is output-gated (disabled), should the central
    "Preview Disabled" placeholder *collapse* (`hide()` it — Qt's `display:none`
    equivalent) so the space is reclaimed, accepting that surrounding docks reflow
    into it (Scenes/Sources may balloon — the point-4 "starved layout" risk)? Left
    as today's in-place placeholder for now; **decide later**, ideally with a quick
    reversible `hide()` experiment to judge the reflow in a real layout.
  - **"First-enabled canvas wins the center" — shelved.** Letting any canvas occupy
    the central editable stage via runtime reparenting + a primary-canvas state
    machine. Rejected as invasive surgery for marginal gain; the real need (editing
    a non-default canvas) is better served by 3a's editable docks. Default-in-center
    + additionals-floating is the shipped model.
- ✅ **3d — Canvas-dock editor parity (scenes & sources lists) — IMPLEMENTED
  (build-green + reviewed, GUI acceptance owed) 2026-06-20.** Commits `873846694`
  (add-source core gains optional `targetScene`), `6f1e99ec1` (`OBSBasicSourceSelect`
  3-arg ctor + canvas-aware undo — `SetCurrentScene` gated behind `!isCanvas`),
  `3537d61f` (dock `+` opens the full picker scoped to the canvas scene), `69f011d82`
  (`OBSBasicTransform` derives its scene from the item, not `GetCurrentScene`;
  item-scoped Reset; canvas detection via owned `obs_source_get_canvas` refs),
  `570a4268c` (source + scene right-click menus on `CanvasDock`), `c624fdca7`
  (extracted shared `ResetSceneItemTransform` into `frontend/utility/`). Plan:
  `docs/superpowers/plans/2026-06-20-canvas-dock-editor-parity.md` (gitignored).
  Reuse-not-rewrite: threaded an optional target scene through OBS's existing
  add-source/transform dialogs (program path byte-for-byte unchanged) and
  reimplemented only the context-menu shells, all canvas-scoped via 3a's
  `SelectedSceneItem()` / `GetCanvasCurrentScene(canvas)` seam. Holistic review =
  SHIP (no program-scene leak — every `GetCurrentScene`-family hit is a program
  fallback or `!isCanvas`-gated; multi-dock isolation sound; teardown safe). **GUI
  acceptance owed** (headless-undriveable): two-dock isolation, canvas add undo/redo,
  Edit-Transform-open-then-destroy-dock, full type-picker attaches to canvas scene.
  Excluded by design (main-window-only): Multiview, projectors, grouping/multi-select,
  copy/paste, interact, grid mode, Studio Mode. Original scope below:
- ✅ **3d (original scope) — SUPERSEDED by the shipped ✅ 3d entry directly above**
  (add-source type-picker, source + scene context menus, Edit Transform all landed
  there; the scene-menu's Multiview / Projector items were **excluded by design** as
  main-window-only, per that entry's exclusion list). Kept for history. The canvas dock's
  scene/source lists are stripped down vs the main window's; bring them to parity:
  - **Add Source** — the dock's `+` (`CanvasDock::AddSource`) only lists *existing*
    global input sources to attach (Model 1: shared sources), so it shows however
    many inputs already exist. The main window's `+` opens the source-*type* picker
    to *create* a new source. Add a "create new source" path to the dock (likely
    reuse `OBSBasicSourceSelect`, scoped to the canvas scene) so new sources can be
    made from the dock, not just attached.
  - **Source-list context menu** — the dock's `sourceList` has no
    `CustomContextMenu` wired at all (only `sceneList` does), so right-click is dead.
    Add a context menu reaching parity with the main Sources tree: Edit Transform /
    reset / fit / center / flip, order, scale filtering, blending, deinterlacing,
    Filters, Properties, Rename, Remove. Most of this is tied to the main window's
    current scene today, so it shares 3a's parameterization work.
  - **Edit Transform** — numeric transform dialog wired to the dock's selected
    scene item (complements 3a's drag-to-transform in the preview).
  - **Scene-list context menu** — the dock's scene menu is only Add/Rename/Remove;
    the main Scenes menu adds Duplicate, copy/paste filters, order, Filters,
    Projector, Show in Multiview, etc. Bring to parity where it makes sense per
    canvas.
  Sequence after 3a (the editable preview is the foundation; the context menus and
  add-source reuse the same main-scene-decoupling).
- ✅ **3e — Stats dock: multi-stream monitoring — SHIPPED.** See "Stats dock (3e) —
  DONE 2026-06-25" under Phase 4 → Remaining work; the open question below resolved
  as **build a new multistream-stats panel** reading `MultistreamEngine::
  StatsSnapshot()` rather than extending stock `OBSBasicStats`. Original brief: OBS's stock Stats
  dock reports a single output (the main stream/recording). With the encode-once
  fan-out engine, the user wants to monitor **all** outputs at once — per-binding
  bitrate / dropped frames / reconnects / CPU-GPU, grouped by canvas, plus the
  shared per-canvas encode stats. **Open design question (no approach yet):**
  whether to extend/replace the stock `OBSBasicStats` dock or build a new
  multistream-stats panel reading `MultistreamOutput`'s per-`obs_output` handles
  (it already tracks them); how to present N outputs compactly; and what libobs
  per-output stats (`obs_output_get_total_frames`/`_frames_dropped`/congestion) map
  cleanly per binding. Brainstorm before planning.

---

## Phase 4 — Frontend rewrite: CEF-hosted Svelte UI 🔧 MVP SHIPPED + CUTOVER

The new Svelte/CEF frontend is the default build (`USE_LEGACY_FRONTEND=OFF` at
cutover); it boots, previews, edits, and multistreams. Core parity + the UI
redesign + theme editor + global audio + scene/source persistence + source
filters + scene transitions + the medium parity gaps + low-priority items
(About, projectors, hotkeys) + the Stats dock are done. The frontend is
feature-complete for streaming/multistream; the remaining items are
deferred-by-choice (Studio Mode, undo/redo, recording, multiview, obs-websocket)
or verification owed (GUI acceptance + a live broadcast) — see "Remaining work".

Replace the Qt Widgets desktop frontend with a web UI (**Svelte**) hosted in
**CEF as the whole application shell**. libobs and the fork's native-multistream
engine (`CanvasManager` / `MultistreamOutput` / `OutputBinding`) stay; only the
UI layer changes.

**Why:** Qt Widgets' layout model is the flexibility ceiling — e.g. the preview
is the `QMainWindow` central widget and can't be hidden/reflowed (no
`display:none` equivalent; see 3c). A web UI removes that whole class of
constraint (CSS layout; freely hide/move/overlap panels), and on the chosen
criteria — lightweight, future-proof, scalable, reliable, flexible — beats the
alternatives.

**Stack decided (after a research pass; full rationale → forthcoming spec):**

- **Shell: CEF as the whole UI** — not Electron, Tauri, or QML. Reuses the
  `libcef` + obs-browser CEF integration already shipped for browser sources →
  one Chromium runtime for UI *and* sources; no second runtime, no Node, no
  native addon. Lowest incremental footprint; in-process C++↔libobs.
- **UI framework: Svelte** — compiled, no vDOM runtime; fits the real-time
  control-surface nature; Svelte 5 + Vercel backing = future-proof.
- **Preview: native-window (HWND/NSView) passthrough** — libobs renders its GPU
  surface into a host-owned window handle (the Streamlabs-proven mechanism;
  macOS via IOSurface).
- Rejected: Tauri (mandatory CEF cancels its lightness), Electron (two
  Chromiums), QML (single-vendor, smaller ecosystem), Sciter/Ultralight (second
  runtime), Flutter (Windows GPU embed broken / desktop archived), Slint (GPLv3
  + immature for desktop chrome).

**Make-or-break first step — de-risking spike (gate before committing): ✅ DONE
2026-06-21 — verdict GO.** A throwaway standalone CEF-host exe (`spike/cef-host/`,
gitignored) proved all four gate items: libobs boots and runs at a steady 60 fps
under the CEF message loop with **no Qt**; `obs_display` renders `monitor_capture`
into a child HWND alongside the embedded browser; a `CefMessageRouter` JS↔C++
round-trip returns the libobs version; clean warning-free teardown (a deferred-
destroy cascade race was fixed via single-ownership + a `while(obs_wait_for_
destroy_queue())` drain, mirroring `OBSBasic::ClearSceneData`), leak count a
constant static 3. Independently reproduced 4/4. Spec/plan/RESULTS:
`docs/superpowers/{specs,plans}/2026-06-20-cef-host-spike*` + `spike/cef-host/
RESULTS.md` (all gitignored — read directly). The mechanism is proven; do not
re-spike it.

**⚠ Biggest 4.1 risk the spike re-scoped (NOT the mechanism):** the spike loaded
only a 2-module allowlist (`image-source`, `win-capture`) because
`obs_load_all_modules()` **aborts headless** — many plugins (encoders, services,
outputs, frontend-tools, *-output-ui) call into `obs-frontend-api` callbacks that
**only the Qt frontend currently installs**. Replacing Qt therefore requires a
**headless-safe `obs-frontend-api` shim** (or curated+patched module loading) to
run the full plugin set. This is unscoped, non-trivial, and plausibly the single
largest chunk of 4.1 — validate it with a cheap throwaway test BEFORE sinking 4.1
effort. Also: the full-app teardown must replicate `ClearSceneData`'s
enum-remove-all-then-drain, not the spike's single-scene minimal version.

**✅ De-risking spike 4.0b — headless `obs-frontend-api` shim — DONE 2026-06-21,
verdict GO.** A throwaway console exe (`spike/frontend-api/`, gitignored) registered
a non-Qt `HeadlessFrontendCallbacks : obs_frontend_callbacks` (all 95 methods —
mostly typed-zero stubs, ~10 real bodies: scene getter, 3 config getters, event/
save/preload registries with fan-out, locale passthrough) via
`obs_frontend_set_callbacks_internal()` before a curated module load. Result
(verified from `run.log`, exit 0): **19 plugins loaded** (incl. obs-x264,
obs-ffmpeg, obs-qsv11, rtmp-services, obs-outputs, obs-browser, obs-webrtc,
win-capture, win-dshow, win-wasapi, obs-filters/transitions), **all 4 functional
probes created OK** (`obs_x264` encoder, `rtmp_custom` service, `rtmp_output`
output, `browser_source`), clean teardown (`bnum_allocs()=3`). **Sized: only 2 of
95 callbacks were actually invoked at load** (`add_event_callback`, `on_event`) —
so the real 4.1 API-layer surface is small (~10–15 live bodies; the rest stay
typed-zero stubs until a UI feature needs them). **One real finding (the Qt-coupling
blocker we were watching for): `obs-websocket` hard-crashes at load**
(`STATUS_STACK_BUFFER_OVERRUN` → `QWidget: Must construct a QApplication before a
QWidget`) — Qt-coupled plugins need either a `QApplication -platform offscreen`
stood up before load, or exclusion. Denylist for 4.1 = `frontend-tools`,
`decklink-output-ui`, `decklink-captions`, `aja-output-ui`, **`obs-websocket`**
(until offscreen-QApplication or a native reimplementation). Spec/plan/RESULTS:
`docs/superpowers/{specs,plans}/2026-06-21-frontend-api-shim-spike*` +
`spike/frontend-api/RESULTS.md` (all gitignored — read directly). The headless
approach is proven; do not re-spike it.

**Scope note:** large multi-sub-project effort (the Qt frontend is ~85K LOC of
UI + app logic), decomposed into its own spec/plan cycles. Remaining Phase 3
items — **3b** (per-canvas Studio Mode) and **3e** (Stats dock) — and any further
canvas-editing parity are **deferred and rebuilt in the new frontend** rather
than written in throwaway Qt. 3a/3d (already shipped in Qt) inform the new
design but their widgets are superseded.

**Migration decided (Approach A — parallel big-bang):** move the current Qt
frontend to `frontend_old/` (kept for reference/parity-checking, not the active
build), build the new Svelte/CEF frontend fresh in `frontend/`, flip the default
at cutover (4.6). A CMake flag (e.g. `USE_LEGACY_FRONTEND`, default ON until the
new frontend reaches MVP) keeps `frontend_old` buildable through the transition
so a working app is always available; only one frontend ever builds at a time.

### Build progress — DONE ✅

Spikes + the full build pipeline + UI redesign + post-redesign features, all
build-green, headless-smoke clean (leaks 2 baseline), and pushed.

- ✅ **4.0 / 4.0b spikes** (GO) — CEF-as-shell + libobs-in-process proven; headless
  non-Qt `obs_frontend_callbacks` shim proven (curated module load; `obs-websocket`
  + other Qt-coupled plugins denylisted pending offscreen-QApplication).
- ✅ **4.1 — shell + bridge + frontend-api layer** — migration scaffold
  (`frontend`→`frontend_old`, `USE_LEGACY_FRONTEND` flag); CEF UI exe serving an
  offline `app://` bundle; single-`CefInitialize` obs-browser coexistence
  (`modbender/obs-browser` fork, env-gated CEF-deferral patch); 20-plugin curated
  load via the non-Qt shim; typed JS↔C++ bridge (`window.obs.call`/`.on` + event
  push); Svelte 5 + Vite + TS shell (bun-built, offline).
- ✅ **4.2 — preview embedding** — UI-positioned native `obs_display` child-HWND
  overlay z-ordered above CEF, per-canvas surfaces keyed `(windowId, canvasUuid)`,
  letterbox + drag/select/stretch/crop editing, multi-surface isolation.
- ✅ **4.3 — core parity + properties** — Scenes / Sources / Preview docks on the
  global channel-0 path; generic `obs_properties` renderer (most types); Add-Source
  type picker; source defaults fixed (descriptor-built, default-aware values).
- ✅ **4.4 — multistream UI** — per-canvas composite `CanvasDock` (preview + scenes
  + sources, output-gated reconciler), Multistream dock (per-output status, toggles),
  Settings tabs Canvas / Streams / Outputs over the existing native engine.
- ✅ **UI redesign (locked design spec, mocks-as-acceptance) P1–P5** —
  P1 shell + token theming (4 axes, 0-radius); P2 core docks on Dockview; P3 canvas
  composites + reconciler + floating tear-out (host-driven detach/redock, per-window
  preview surfaces); P4 Audio Mixer (faders + live dB meters); **P5 cutover**
  (`USE_LEGACY_FRONTEND=OFF`). Layout + theme persisted (`layout.json`/`theme.json`).
- ✅ **Post-redesign features** —
  right-click context menus on scene/source/canvas rows (+ `sources.rename`,
  `scenes.duplicate`); right-click in the native preview overlay (`WM_RBUTTONUP` →
  `preview.contextMenu` → DOM menu over a suspended overlay); theme editor (edit
  every token + custom-theme save/delete, live + persisted, opaque JSON blob);
  **global audio device management** (seed Desktop Audio + Mic on first run, persisted
  to `audio_devices.json`, Settings → Audio device pickers — fixes the empty mixer,
  verified against a real device).

### Remaining work 🔭 (priority order)

**High — core usability gaps:**

- ✅ **Scene / source persistence (global / Default canvas) — DONE 2026-06-23.**
  `scene_collection.json` save/load via `obs_save_sources_filtered` + `obs_load_sources`
  (round-trips scenes WITH full item layout). Loads on boot (placeholder fallback when
  absent); saves on every global scene/source mutation, on drag-end, and on clean exit.
  Excludes the channel 1-6 global-audio sources (restored from `audio_devices.json`) and
  additional-canvas-scoped sources. Note: Default/global scenes live on libobs's
  `main_canvas`, so the filter keeps `canvas == obs_get_main_canvas()` + null-canvas
  inputs, drops additional-canvas scenes. Round-trip + `leaks: 2` verified. **Follow-ups:**
  per-canvas scene persistence (additional canvases still rebuild empty); audio
  volume/mute persistence (only the device persists today). Multiple named scene
  collections — **done in Phase 6a.**
- ✅ **Source filters — DONE 2026-06-24.** `filterTypes.list` + `filters.list/add/
  remove/setEnabled/reorder/rename` + a `"filter"` property kind (resolve a filter by
  uuid → reuse the generic `obs_properties` renderer). Filters dialog (list + add
  type-picker + reorder/enable/remove/rename, with the selected filter's properties via
  `PropertyForm kind=filter`) reachable from the Sources / Canvas / preview source
  context menus AND the audio-mixer rows (mic noise suppression). Filters persist
  automatically with their parent source via the scene collection. Dup default names
  auto-suffix. **Follow-up:** in-dialog live preview (the dialog suspends the overlay
  while open, so effects show in the main preview after closing, not live in-dialog).
- ✅ **Scene transitions — DONE 2026-06-24.** Channel 0 now holds an active
  transition source (Fade by default, 300 ms) wrapping the current scene; a
  `Transitions::GetProgramScene()`/`SetProgramScene()` seam unwraps it at every
  channel-0 read/write (scenes.list current flag, preview surface, scene-save,
  setCurrent → `obs_transition_start`, remove-fallback). Type + duration persist to
  `transitions.json` (excluded from the scene collection, like global audio);
  re-sized on `obs_reset_video`; duration clamped to 20 s. Bridge `transitionTypes.list`
  + `transitions.getCurrent/setCurrent/setDuration`; real TransitionsDock (type
  dropdown + duration ms). Default/global program path only; additional-canvas
  switches stay direct (→ 3b). Holistic review = SHIP; build clean, smoke green
  (unwrap returns the scene name not "Fade", leaks 2). **GUI-owed:** the visual fade
  animation (headless-undriveable). **Follow-up:** transition-source properties
  (Fade-to-Color / Luma Wipe / Stinger) use defaults — no properties UI yet;
  per-canvas transitions (→ 3b).
- ⏸ **Studio Mode (3b) — DEFERRED (decided 2026-06-25).** Preview/program staging +
  transition, per canvas. Menu item stays disabled. Deferred as low-value for this
  fork's use case (it's a live-switching-production workflow); revisit if that need
  arises. Transitions already cover the global program path; Studio Mode would add an
  off-air preview scene + a program surface + the preview→program transition action.
- ✅ **Stats dock (3e) — DONE 2026-06-25.** `stats.get` bridge + `MultistreamEngine::
  StatsSnapshot()` (per-live-output counters under `liveMutex`). A Stats dock (opened on
  demand via the Docks/View menu) polls ~1×/s: general perf grid (CPU/memory/FPS/avg
  frame time/render-lag/encoder-skip, warn-colored over threshold) + per-output rows
  (state dot, profile→canvas, bitrate via byte-delta cache, dropped+%, congestion,
  duration). Build-clean /W4 /WX, selftest verifies the shape, `leaks: 2`. Per-output
  live numbers (bitrate/drops) need a real broadcast to verify (GUI/live-owed).
- ✅ **Scene linking (non-default canvas ↔ main-scene activation sync) — IMPLEMENTED
  (build-green + per-task & holistic review; GUI acceptance owed) 2026-06-27.** A
  non-default canvas scene can *follow* one or more Default (main) scenes: switching
  the program scene auto-switches every linked canvas to its mapped scene (activation
  sync, not content mirror; names need not match). Right-click a canvas-dock scene →
  "Link to ▸" checkable submenu of main scenes; linked rows show a 🔗 badge + tooltip.
  The pre-existing `CanvasSceneLink` type (uuid-keyed → rename-robust) is now wired
  end-to-end: new `SceneLinkStore` persists per scene-collection to
  `scenes/<slug>.scene_links.json` (mirrors `OutputBindingStore`);
  `ObsBootstrap::ApplyCanvasSceneLinks` + 3 prune helpers; bridge
  `sceneLink.list/set/clear` + `sceneLink.changed`; hooks on program-scene switch
  (apply), scene/canvas/canvas-scene delete (prune), and collection-switch + boot
  (re-apply the restored program scene's links). `ContextMenu` gained one-level
  checkable flyout submenus (shared component; flat menus unchanged). Spec/plan:
  `docs/superpowers/{specs,plans}/2026-06-27-scene-linking*` (gitignored). Subagent-driven
  exec, per-task spec+quality reviews + a holistic final review = SHIP_WITH_MINOR (the
  one Important finding — links not re-applied on boot/collection-switch — was fixed).
  **GUI-owed** (headless-undriveable): toggle link/unlink + 🔗 indicator, activation
  sync on a real switch, persistence across restart, prune on delete. **Known
  invariant** (documented in the spec): activation sync is hooked only on the bridge
  program-scene-switch path — there is no scene-switch hotkey in this frontend today,
  so a future such hotkey must also call `ApplyCanvasSceneLinks`.

**Medium — parity gaps:**

- ✅ **File-dialog / Browse — DONE 2026-06-24.** Native `dialog.openFile` bridge
  method (modern `IFileDialog` COM — open/save/directory, OBS filter-string parsing,
  parented to the main host window, COM-balance/leak-reviewed = SHIP); `PathControl`'s
  Browse button wired. File-based sources can now pick paths.
- ✅ **Property types font / editable_list / frame_rate — DONE 2026-06-24.** Replaced
  the "Editing not yet supported" fallback with real controls (FontControl
  face/size/bold-italic-underline-strikeout; EditableListControl add/remove/reorder +
  Add Files via the dialog; FrameRateControl common-rate dropdown + numerator/
  denominator). Serializer enriched with editable_list (type/filter/default_path) +
  frame_rate (fps_options/fps_ranges) metadata; the generic obs_data JSON write path
  round-trips the values. Registered in the data-driven control registry.
- ✅ **Edit → Transform — DONE 2026-06-24.** `sceneItems.getTransform/setTransform/
  transformAction` (position/scale/rotation/crop/alignment/bounding-box + reset/center/
  fit/stretch/flip; per-canvas base res; round-trip + center math runtime-verified in
  the boot self-test). Numeric Transform dialog reachable from the Sources/Canvas/
  Preview source context menus + the Edit→Transform menu item.
- ✅ **Undo / Redo — SHIPPED as OBS-parity backlog Item 1** (see "OBS-parity backlog
  (Items 0–17) ✅ COMPLETE 2026-06-28" under Phase 7): `UndoManager`, `undo.*` bridge,
  Ctrl+Z/Y, apply-target-state keyed on source uuid, add/remove snapshot-recreate.
  Supersedes the 2026-06-24 deferral (recorded then as: no undo infrastructure exists;
  a from-scratch cross-cutting subsystem, not a quick parity fix).
- ⏸ **Recording / Replay buffer — KEPT DORMANT (decided 2026-06-24).** The app stays
  streaming-only; recording remains hidden/inert (no bridge). Revisit only if the need
  arises.

**Low / likely out of scope (confirm):**

- ✅ **About dialog — DONE 2026-06-24.** Help→About modal (product name, tagline,
  libobs version, GPLv2+, fork note); offline.
- ✅ **Projectors + Fullscreen Preview — DONE 2026-06-24.** Native `ProjectorManager`
  + borderless-fullscreen / windowed projector windows rendering a target
  (program/scene/source/canvas) into their own `obs_display`, letterboxed, closing on
  Esc/window-close; teardown ordered before canvas mixes. `display.listMonitors` +
  `projector.open/close/list`; per-monitor + windowed entries in the Sources/Scenes/
  Preview/Canvas context menus + the View menu (program). Selftest + monitor-enum
  verified; on-screen rendering/fullscreen placement is GUI-owed. (Fullscreen Preview =
  a program projector.) Recordings list is moot — recording is dormant.
- ✅ **Multiview — SHIPPED as OBS-parity backlog Item 10** ("Multiview projector"; see
  the backlog section under Phase 7). Was: a grid render of all scenes in one window,
  needing a custom multi-scene grid render rather than a single-target `obs_display`.
- ✅ **Hotkeys settings — DONE 2026-06-24.** Enumerate libobs hotkeys, bind key combos,
  persist (`hotkeys.json`, keyed by name), restore on boot. `hotkeys.list/set/clear`
  (UI sends DOM `KeyboardEvent.code` + modifiers → a data-table maps to `obs_key`);
  Start/Stop Streaming frontend hotkeys registered + wired to the engine. **Bound
  hotkeys fire globally** via libobs's existing `GetAsyncKeyState` poll thread (verified
  against source — no keyboard hook needed). Hotkeys tab in Settings (grouped by
  registerer, click-to-capture + clear + filter). Selftest round-trips a binding;
  real-keypress firing is GUI-owed.
- 🔭 **obs-websocket** — excluded (Qt-coupled, hard-crashes headless). Needs an
  offscreen-`QApplication` stood up before load, or a native reimplementation, if
  remote control is wanted. (Note: Phase 5 OBS-MCP would give AI control via the
  bridge instead — may reduce the need for obs-websocket.)

**Cleanup / infra:**

- ✅ **`frontend_old` retirement — DONE.** The shared pieces moved into `frontend/`
  (`frontend/api` + `frontend/api-impl`, the Qt-free models in `frontend/utility/`),
  then the whole Qt tree was **deleted outright** — commit `9aa092af3` ("chore: remove
  dead legacy Qt frontend (frontend_old)"). Nothing references `frontend_old/` now.
- 🔭 **Cross-platform preview** — Windows-only today (HWND + D3D11). macOS
  (NSView + IOSurface) and Linux (X11/GL) embedding are later efforts.
- 🔧 **Live broadcast test** (owed since Phase 2) + **GUI acceptance** of the recent
  menu / theme / preview / audio work (none driveable headlessly).
- ⏸ Minor: `SettingsModal.load()` routes device-fetch errors to the `videoError`
  label (cosmetic).

---

## Phase 5 — OBS MCP: AI control surface 🔧 MVP SHIPPED (2026-06-25)

Expose the app's OBS control to AI agents over the **Model Context Protocol (MCP)**,
so an assistant can drive the production — switch scenes, add/configure sources, edit
transforms, manage canvases / stream profiles / output bindings, start/stop the
multistream, and read live state — through a typed, discoverable tool surface.

**MVP DONE (embedded HTTP, decided topology):** an embedded localhost HTTP MCP server
(`frontend/src/mcp/{HttpServer,McpServer}`), opt-in (`mcp.json enabled=false` default,
nothing listens until turned on), token-auth, `127.0.0.1`-only. Speaks the MCP
request/response subset (`initialize`/`tools/list`/`tools/call`/`ping`); every
`tools/call` marshals onto the CEF UI thread (15s-timeout future, shutdown-guarded) and
reuses the existing `Bridge::Dispatch` over `g_methods`. **20 curated tools**
(list/switch/create scene, scene-item visibility+transform, source create/rename/remove,
transitions, canvas/profile/output lists, multistream status + start/stop, get_stats)
+ a generic **`obs_call`** escape hatch. **Capability gating:** Read always; Mutate
(`allowMutations`); GoLive (`allowGoLive`, default off) — start/stop_output are GoLive.
A Settings **"AI Control"** tab (enable, port, copyable endpoint + token, regenerate,
the two capability toggles). Self-test round-trips initialize/tools.list/curated +
generic calls + go-live gating; `leaks: 2`, clean thread join at teardown. **Owed:**
a real external MCP client (Claude Desktop/Code) driving OBS over the socket end-to-end
(GUI/live; the self-test proves the protocol in-process). Spec:
`docs/superpowers/specs/2026-06-25-phase5-obs-mcp-design.md` (gitignored).

**Later (post-MVP):** SSE / server-initiated notifications (push obs events as MCP
notifications), MCP **resources** (scenes/canvas/stats as pull resources), per-tool
confirmations, schema auto-generation from an annotated `g_methods`, remote/auth
hardening.

**Why this is cheap to build here:** the CEF bridge is already a **data-driven method
registry** (~77 operations in `g_methods`: `scenes.*`, `sources.*`, `properties.*`,
`filters.*`, `transitions.*`, `sceneItems.*` incl. transform, `audio.*`, `canvas.*`,
`streams.*`, `outputBindings.*`, `multistream.*`, `dialog.*`, `window.*`). MCP tools map
almost 1:1 onto these. The work is a transport + schema-generation layer over the
existing registry, **not** a reimplementation of OBS control — the same envelope
(`{method, params} -> result`) that `window.obs.call` uses is what an MCP tool would
invoke. Events (`EmitEvent`) can surface as MCP notifications/resources for live state.

**Open design questions — ✅ ALL RESOLVED by the shipped MVP above (2026-06-25).**
Topology → (a) embedded, localhost HTTP. Tool granularity → curated set (20 tools) +
the generic `obs_call` escape hatch. Safety/gating → `127.0.0.1`-only + token auth +
the Read/Mutate/GoLive capability split (`allowGoLive` default off). Schema generation
→ hand-authored for the MVP; annotating `g_methods` stays a post-MVP DRY item. State
exposure → deferred with SSE/resources to "Later (post-MVP)". Original text kept for
the reasoning:

- **Topology** — (a) MCP server *embedded in the app process* exposing `g_methods`
  directly over stdio/HTTP, vs (b) a *sidecar* process that relays to the app. (a)
  reuses the in-process registry with zero IPC and is the natural fit; (b) decouples
  lifecycle but needs a wire protocol (and `obs-websocket` is excluded here, Qt-coupled).
  Lean (a).
- **Schema generation** — derive each MCP tool's input schema from the bridge method's
  params. Either hand-author a manifest, or (better, DRY) annotate `g_methods` entries
  with a param schema and generate the MCP tool list from the same registry that drives
  `window.obs.call` — one source of truth.
- **Tool granularity** — one MCP tool per bridge method (discoverable, large list) vs a
  few coarse tools (`obs.call` passthrough + a `list_methods`). Probably a curated set of
  high-value tools (scene/source/stream control) plus a generic escape hatch.
- **State exposure** — scenes/sources/canvas/stream status as MCP *resources* (pull) and
  the existing push events as *notifications*, so an agent can observe, not just act.
- **Safety / gating** — local-only by default; if remote, auth + a capability split
  (read-only vs mutating vs streaming-control); confirmations for irreversible/outward
  actions (go-live). Mirror the app's own "outward actions need confirmation" posture.

**Scope note:** new sub-spec/plan cycle when Phase 4 is wrapped. Build on the *existing*
bridge registry; do not fork a second control path. This is additive (a new interface to
the same engine), independent of the remaining Phase-4 UI items.

---

## Phase 6 — Multiple scene collections + OBS Studio importer

Two parts: **6a** multiple named scene collections (OBS-parity the Phase-4 rewrite
dropped — and the prerequisite for the importer), then **6b** a read-only importer
that pulls a real OBS Studio install's config into this fork.

### Phase 6a — Multiple scene collections ✅ COMPLETE (2026-06-26)

Add multiple named scene collections (list / create / rename / delete / switch,
persisted, surviving restart). **Merged to `master` (fast-forward, tip `75a73e8ac`);
`scene-collections` branch deleted.**

**Architecture:** scene collections are a **frontend** feature — libobs has *zero*
scene-collection concept (verified: 0 references in `libobs/`; the engine only offers
`obs_save_sources`/`obs_load_sources`). OBS's old Qt frontend owned it; the Phase-4
rewrite simply never reimplemented it. Built in the C++ bridge (`frontend/src/`), NOT
libobs.

**Model (locked):** *per-collection* = scenes/sources (main-canvas scenes + plain
inputs, the set `scene_persistence` already captures) **+ output bindings** (each show
routes to its own destinations). *Global, shared* = canvases (reusable encode targets)
and stream profiles (credentials never re-entered per show).

- ✅ **Registry + per-collection persistence + migration** — `scene_collections.
  {cpp,hpp}` (`{id,name,sceneFile}` index in `basic/scene_collections.json`, slugged
  `scenes/<slug>.json` per collection); `scene_persistence` generalized to path-based
  Save/Load + `ClearCurrent` (reuses the *same* `SaveFilter` as Save, so the
  save/teardown boundary can't drift). First-run migration points the existing
  `scene_collection.json` + `output_bindings.json` at the first "Untitled" collection
  (in-place, zero data loss).
- ✅ **Switch mechanism** — `Switch(id)` = save outgoing scenes+bindings → teardown
  drain (the proven shutdown drain: unbind ch0 transition → enum+remove → wait for
  destroy queue) → flip+persist index → load incoming scenes+bindings → re-establish
  the ch0 transition → emit `collections/scenes/transitions/outputBinding/multistream
  .changed`. **Refuses while live** (`Multistream().AnyLive()`, before any mutation);
  `Remove(active)` switches away first. Corrupt-index hardening: mutating bridge ops
  refuse when both index `.json` + `.bak` are unparseable (won't clobber on-disk
  scene files); reads still work.
- ✅ **Per-collection output bindings** — `OutputBindingStore` made path-based;
  bindings travel with the active collection (sibling `scenes/<slug>.output_bindings
  .json`); migrated from the legacy global file; zero-binding new collection doesn't
  crash (Default canvas keeps its central preview placeholder).
- ✅ **Switcher UI** — a "Scene Collection ▾" menu-bar dropdown (active radio-checked
  → switch; New… / Rename… / Delete via a reused zero-radius `CollectionDialog`;
  Delete disabled at the last collection; refresh on `collections.changed`); backend
  errors (switch-while-live, delete-last, dup-name) surfaced, not swallowed.

Subagent-driven (4 tasks + per-task reviews + a holistic review = SHIP_WITH_MINOR, all
addressed). Build green /W4 /WX, `bun run check` 0/0, smoke `leaks: 2`. **GUI/runtime
acceptance owed** (headless-undriveable): the switch round-trip (create B → switch →
add scenes → back to A intact → B's scenes present), leak-hygiene across
boot→switch→switch→shutdown, and the while-live / corrupt-index rejection paths.

**Scope boundary (intentional):** only *main-canvas* scenes are per-collection;
additional-canvas scenes remain global (the pre-existing per-canvas-persistence gap,
unchanged). Output bindings *are* per-collection.

### Phase 6b — OBS Studio data importer ✅ COMPLETE 2026-06-28 (as Phase-7 backlog Item 17)

A read-only importer: detect a real OBS Studio install (`%APPDATA%/obs-studio`) and
recreate its data inside this fork (`%APPDATA%/braidcast`), **never modifying the
original OBS data**. Built on the 6a multi-collection foundation.

> **Shipped on `ui-redesign`** (backend `3738f09a0` `frontend/src/obs_importer.{cpp,hpp}`
> + `importer.scan`/`import`; wizard `642f237db` `ImporterDialog.svelte` in Settings →
> General → Importer). Read-only invariant audited clean (every obs-studio access is
> `obs_data_create_from_json_file` / `config_open(...EXISTING)`; writes only to fork dirs).
> Per-collection **and** per-scene selection with dependency closure; service / video /
> audio mapped from the **active per-profile** dir (`user.ini` → `ProfileDir`); live-guard
> + dedup. Open decision: active-profile-only (a profile picker is an optional follow-up).
> A real scan/import round-trip is GUI-owed.

**Design decisions (locked with the user 2026-06-26, as built):**

**Decisions (locked with the user 2026-06-26):**
- **Scope: everything** — scene collections, stream destinations (service + keys),
  video settings, audio settings.
- **A selective wizard window** — a dedicated CEF dialog that lets the user toggle
  *what* to import, down to **per-collection and per-scene** granularity (pick a whole
  collection or a single scene out of it), plus per-profile toggles for
  Destination / Video / Audio.
- **Leave destinations unwired** — import collections, canvases, and stream profiles as
  independent pieces; the user wires destinations→canvases (output bindings) afterward.
  Honest to OBS's model (OBS has no per-collection binding concept).
- **Launch from the File menu** — "Import from OBS Studio…" (explicit, re-runnable).

**Mapping (from the 6b research pass — accurate field-level map exists):**
- **OBS scene collection → fork scene collection** *(strong — libobs-native)*: OBS
  `basic/scenes/*.json` is the *same* serialization the fork loads; its `sources` array
  feeds `obs_load_sources` verbatim (filters/transforms/groups for free). OBS has **no
  index** — scan the dir, read the display name from each file's `"name"` key, exclude
  `.bak`. **Per-scene import** = parse the `sources` array, take the selected scenes +
  their **dependency closure** (referenced inputs + nested scenes/groups), write a fork
  collection with that filtered array (the one genuinely new piece of logic).
- **`service.json` → stream profile** *(strong, near 1:1)*: OBS `"type"` → fork
  `serviceId`; `"settings"` blob passed verbatim; profile name → label.
- **`basic.ini` `[Video]` → canvas** *(strong)*: `BaseCX/CY`/`OutputCX/CY` + the
  `FPSType/FPSCommon/FPSInt/FPSNum/FPSDen` encoding (NTSC fractions table known) →
  CanvasDefinition (Default canvas updated; extra profiles → additional canvases).
  Decision pending: `OutputCX/CY` (encode size) vs `BaseCX/CY` when OBS downscales.
- **`basic.ini` `[Audio]` → global audio** *(strong)*: `SampleRate` + `ChannelSetup`
  (lowercase the value) → `settings.setAudio`.

**Caveats (carry into the spec):** global audio *channels* embedded as top-level keys
in OBS scene JSON are dropped by the fork's loader (separate import step if wanted);
encoder id is split between `basic.ini [AdvOut]` and `streamEncoder.json` (read both);
Simple-output mode infers the encoder from `[SimpleOutput]`; the dup-profile guard
(key/name) means re-import must skip or rename; missing-plugin sources are skipped with
a warning by `obs_load_sources`; "OBS not found" must fail gracefully (hence the Browse
fallback). Full research + the field-by-field map live in the session transcript /
forthcoming 6b spec.

**Next step when resumed:** finalize the wizard mock (the per-profile-vs-global
Video/Audio toggle shape was the open UI question) → spec → plan → subagent-driven
build (C++ scan/apply bridge methods via super-languages; the wizard via super-frontend).
Reuses the libobs load path + the three-layer canvas/profile/binding model; do not
invent a parallel persistence format.

**Follow-on — partial / à-la-carte imports (not started, requested 2026-07-11).**
The importer today only imports *into a new scene collection*: every toggle is
gated on selecting a collection, so pulling just the global-audio devices+filters
(`importGlobalAudio`) or just a stream credential still creates/switches a
collection as a side effect. Add a **partial import** path that applies a chosen
artifact to the *current* setup with **no collection creation/switch**:
- **Global audio devices + their filters** — graft OBS `DesktopAudioDevice*` /
  `AuxAudioDevice*` source blobs (incl. `filters`) onto the fork's global channels
  via `GlobalAudioChannels().Persist()` + `AudioMonitor().Rebuild()`. (This is the
  need that forced the 2026-07-11 hacky hand-recovery of lost mixer filters.)
- **A single stream credential** into `streams.json` without touching scenes.
- **Video / audio settings** onto the current config.
Same OBS parse path, but the apply targets live global stores instead of a new
collection. Decouple the wizard's per-artifact toggles from the
collection-selection loop so each artifact imports independently. Small, separable
from the Phase-12 Studio Profiles work.

---

## Phase 7 — Full UI redesign: nav-rail multi-page app 🔧 FEATURE-COMPLETE (GUI acceptance + merge owed)

> All six phases (7.0–7.5) built + reviewed (final holistic = SHIP_WITH_MINOR) and **merged to `master`** (the `ui-redesign` branch was fast-forwarded in and deleted — see "Branch history"). **GUI acceptance is still owed** — the merge didn't consume it. Two behavior changes to confirm: existing users must **Reset Layout** once to see the studio reorg (a saved `layout.json` restores the old arrangement); **Settings now live-applies with no Cancel/undo** (the modal's transactional revert, incl. the issues.md-C1 output-binding revert, was intentionally dropped for the page model).


A ground-up reconception of the frontend IA, driven by a Claude Design mock the user
authored ("OBS fork multistream redesign"). Rationale: the prior UI (incl. the Phase-4
rewrite + settings redesign) was an *enhancement of OBS's original layout*; the fork's
real goal is **more control + multistream**, so the UI is rebuilt around that. The mock
is the acceptance criterion ("mock IS the spec"); it's an intentionally incomplete
reference — menu-bar items are rehomed by us. Reference + analysis (gitignored):
`docs/superpowers/design-redesign/{OBS-MultiStream.dc.html.json,ANALYSIS.md}`. Branch
`ui-redesign`.

**The shift:** from a top **menu bar + single Dockview workspace + modal Settings** →
a **70px left icon nav rail** switching **six full-page views**: Studio · Stream
(Destinations) · Schedule · Monitor (Stats) · AI · Settings.

**Tokens:** Geist + Geist Mono, the mock's dark palette (bg `#0a0a0b`, accent amber
`#e7a338`), zero border-radius; axes theme dark|light, accent (amber/blue/violet/
emerald/rose), density (comfortable|compact).

**Decisions locked (2026-06-26):** (A) **Schedule = UI shell only** this rewrite
(calendar + New-Stream modal on local state; persistence + auto-go-live later). (B)
**Orphan menu items → nav-rail footer** (Scene Collection switcher + About) **+ Studio
"⋯" overflow** (projector launchers); transforms/filters/projectors stay in context
menus. (C) **Appearance = keep the full per-token theme editor AND add presets**
(accent swatches + light/dark + density). (D) **Docking = keep Dockview, restyle** (not
the mock's bespoke float/dock system); Studio is a re-skin + re-IA.

**Phasing (plan `docs/superpowers/plans/2026-06-26-phase7-shell.md`):**

- ✅ **7.0 — Tokens + nav-rail shell + view router — SHIPPED** (commits `a312a0c` tokens,
  `5bddaa061` nav-rail+router, `c71a2e39d` studio page; holistic review = SHIP; now on
  `master`). Studio Dark default palette + Geist offline +
  accent/mode axes (theme editor kept); 70px NavRail (6 views + footer scene-collection
  switcher + About) replacing the menu bar (MenuBar kept, unrendered); page router;
  `StudioPage` (DockHost lifecycle a verbatim move, kept mounted + `display:none`, not
  `{#if}`); CANVASES chip bar (eye-toggle ↔ Dockview show/hide, restore chips, Reset
  Layout) + bottom GO-LIVE bar (live state + per-binding start/stop + stats perfRow, all
  real bridge data); explicit `suspendPreview()` overlay gate hiding the native preview +
  CanvasDock off-Studio. `bun run check` 0/0, build green, smoke `leaks: 2`. **All visual
  fidelity GUI-owed.** Subagent-driven; caught + fixed a Critical `--accent` CSS-var
  collision in review.
- ✅ **7.1 — Studio reorg — SHIPPED** (see the Update note below this list). Preview-row over bottom-docks-row layout, Dockview restyle to
  the mock's dock chrome (headers w/ float/close, tokens, zero-radius), embedded
  scenes/sources in additional-canvas docks, full CANVASES-bar + GO-LIVE-bar fidelity.
  Carry-ins from 7.0: gate the stats poll on `page==='studio'`; focused-canvas (not
  all-outputs) live state; persistent per-canvas hide; a real add-canvas flow (`+`
  currently routes to Settings).
- ✅ **7.2 — Stream + Monitor + AI pages — SHIPPED** (`06a0bd8b4`). Re-IA the MultistreamDock / StatsDock /
  McpTab content into full pages.
- ✅ **7.3 — Settings page + Appearance — SHIPPED** (`52d6654ae`). Full page w/ 196px left sub-nav (Canvases /
  Stream Profiles / Outputs / Audio / Hotkeys / Appearance); Appearance hosts the accent/
  mode/density presets + the existing per-token theme editor (re-exposes the Theme Editor,
  which is unreachable in the 7.0–7.2 interim — theme still applies).
- ✅ **7.4 — Schedule (UI shell) — SHIPPED** (`06036fb1d`). Calendar grid + Upcoming + New-Stream modal on local
  state (no backend per Decision A).
- ✅ **7.5 — Orphan homes + polish + holistic review — SHIPPED** (`7893013`). Studio "⋯" overflow (projectors),
  remaining Edit/View/Docks menu actions, final fidelity pass, then merge to `master`.

> **Update:** 7.1–7.5 all SHIPPED (commits 7.2 `06a0bd8b4`, 7.3 `52d6654ae`, 7.4 `06036fb1d`,
> 7.5 `7893013`, cleanup `b68467213`) + a GUI-acceptance fix batch (2026-06-27). **Merged to
> `master`** with the rest of `ui-redesign`; **GUI acceptance still owed.**

### OBS-parity backlog (Items 0–17) ✅ COMPLETE 2026-06-28

After the redesign, an autonomous backlog closed the OBS feature-parity the CEF rewrite had
dropped. Items 0–17 (control doc gitignored) shipped on `ui-redesign`; the C++↔JS bridge grew
to **140 methods**. Each item = backend + frontend waves with per-task and a final holistic
review (SHIP_WITH_FIXES → all fixed; tip `0b4f22e2e`).

- **0** scale-filter + scene-collection duplicate. **1** Undo/Redo (`UndoManager`, `undo.*`,
  Ctrl+Z/Y, apply-target-state keyed on source uuid, add/remove snapshot-recreate). **2**
  source-type picker + add-source. **3** Advanced Audio Properties dialog (vol dB / mono /
  balance / sync offset / tracks / monitoring). **4** Virtual Camera (`VirtualCamManager`).
  **5** source Interact window. **6** canvas output-resolution / downscale / fractional FPS.
  **7** General settings (snapping, projectors, tray, multiview, importer prefs — descriptor
  table) + list search + scenes grid. **8** Advanced settings (process priority, stream
  delay, reconnect, bind-IP, etc. — descriptor table). **10** Multiview projector. **11**
  system tray. **12** browser / custom docks (iframe-based, bridge-persisted). **13** F11
  fullscreen + scenes grid mode. **14** source extras — missing-file relink, per-source
  deinterlacing, scene-item color tag + row tint. **15** screenshot (program + per-source,
  WIC PNG, `%APPDATA%/braidcast/screenshots/`, Ctrl+Shift+S, toast). **16** session log
  writer + Log Viewer + `shell.revealPath`/Open Folder. **17** = **Phase 6b** OBS-Studio
  importer, read-only (see below).

- ⏸ **Item 9 DEFERRED** — per-binding output overrides (stream delay/reconnect/bind-IP per
  output) conflict with the encode-once fan-out architecture; revisit as its own design
  effort. Also still deferred: per-canvas **Studio Mode** (Phase 3 3b), **Auto-Config
  wizard**, and three visual redesigns (Canvas modal, Appearance tab, Stream tiles).

- **GUI acceptance owed** for the whole backlog (headless can't drive the bridge): undo/redo,
  virtual cam, interact, screenshots + PNG correctness, deinterlace/color round-trips,
  missing-file relink, log viewer, and especially a real importer scan/import round-trip.

### Nav IA restructure + single Go Live ✅ 2026-06-28

The redesigned nav was reorganized so the three-layer model gets first-class homes, and all
live control collapsed to one button (spec/plan under `docs/superpowers/`). On `ui-redesign`,
frontend-only (backend already had `streaming.start`/`stop` = StartAllEnabled/StopAll).

- Nav rail: Studio · **Canvases** · **Streams** · Schedule · Monitor · AI · Settings (was
  "Stream"/Destinations). **Canvases** = master-detail page (left canvas list; right =
  `CanvasEditor` extracted from the old Settings→Canvases tab + that canvas's destinations as
  **toggle-only** rows with read-only live status). **Streams** = the promoted Stream Profiles
  manager. Settings dropped both tabs (now General/Audio/Hotkeys/Browser Docks/Appearance/
  Advanced).
- **Single source of truth for live:** one Go Live button in the Studio bar →
  `streaming.start`/`stop`; live state from the global `streaming.changed`. Removed ALL
  per-canvas/per-binding/per-stream start/stop (Studio per-focused loop, Stream page
  Start/Stop/Retry, Multistream dock Start — now a read-only monitor). Toggles
  (`outputBinding.setEnabled`) only arm; the canvas renders when ≥1 enabled binding.
- 5 tasks subagent-driven (super-frontend) + holistic review (super-quality = SHIP, 0
  Critical/0 Important; 3 Minors fixed). Deleted dead `StreamPage.svelte` + `CanvasesTab.svelte`.
  Commits `41ef6a6d0`..`2666ced4a`. check 0/0, build EXIT=0, leaks 2. **GUI acceptance owed:**
  nav switch, canvas edit + destination-toggle persistence, single Go Live starts/stops all.

---

## Phase 8 — Platform integration: OAuth + stream metadata + Go Live modal 🚧 MVP (8a–8e) DONE; 8f deferred

**Goal:** A "Stream Information" modal that opens when the single **Go Live** button is clicked
(classic-OBS style), letting the user set per-destination **title / category / tags / thumbnail**
and push them to the platform via its API before the stream starts. Platforms: **Twitch, Kick,
YouTube** (minimal metadata). OAuth is the enabler — not for streaming itself (RTMP stream keys
already work), but for editing live metadata the way stock OBS does.

**Why OAuth, not just stream keys:** streaming works today via RTMP keys. OAuth adds *managing*
the broadcast — title, game/category, tags, thumbnail — from inside the app.

**Core requirement — extensibility (a provider registry, not per-platform branching).** Adding a
new provider must be ~one module + one registry line, with the modal and engine untouched. This
is a data/registry design, not a `switch (platform)`:

- **`StreamProvider` interface** (C++ bridge) per platform: `id`, `displayName`, an **auth
  strategy** reference, `authConfig` (endpoints/scopes/cred-env-var names), a **capability
  descriptor** (which fields it supports + their constraints), `searchCategories(token, query)`,
  `applyMetadata(token, profile, fields)`, and optional broadcast-lifecycle hooks
  (create/bind/transition) for providers that need them (YouTube, Facebook).
- **`ProviderRegistry`** = `map<id, StreamProvider>` populated at boot. Bridge methods are
  generic and dispatch through it: `oauth.providers` (returns every provider's capability schema
  as JSON), `oauth.connect{providerId}` / `disconnect` / `status`, `streamMeta.searchCategories`,
  `streamMeta.get`/`set{profile, fields}`.
- **Auth strategies are a small reusable set** providers pick from — a new provider almost always
  reuses one rather than writing flow code: `device-code` · `pkce-loopback` · `auth-code+secret`
  (loopback or embedded webview) · (optional `proxy` for secret-required providers). Secret
  handling is a strategy concern (embed-obfuscated vs proxy), not per-provider code.
- **Capability-driven modal:** the Svelte Go Live modal renders fields *from* each connected
  destination's capability descriptor (title/category/tags/thumbnail/privacy/description as
  data). A new thumbnail-capable provider → the thumbnail field appears automatically; one
  without → hidden. **No modal edits to add a provider.**
- **Net:** adding a provider = a new `XProvider` (declares endpoints/scopes/caps + apply impl,
  reuses an auth strategy) + one registry entry + `X_CLIENTID`/`X_SECRET` env vars. Zero changes
  to the modal, the engine, or the other providers.

### Researched platform reality (2026-06-28; sources in the design analysis)

| | Title | Category | Tags | Thumbnail | Desktop auth | Notes |
|---|---|---|---|---|---|---|
| **Twitch** | ✓ | ✓ game_id | ✓ ≤10, lowercase-alnum-25 | ✗ (no API) | **Device Code Flow**, no secret | `PATCH /helix/channels` (one call). 4 h token; refresh one-time-use, 30-day inactivity expiry. |
| **Kick** | ✓ | ✓ category_id | ✓ ≤10 | ✗ (read-only field) | Auth-code **+ PKCE but secret STILL required**; no device flow | `PATCH /public/v1/channels` (one call). Official API exists (`api.kick.com`). Youngest API. |
| **YouTube** | ✓ | ✓ (on the video, separate call) | ✓ | ✓ `thumbnails.set` (2 MB, phone-verified acct) | **PKCE loopback** (Desktop client) or device flow, no secret | Full broadcast lifecycle: create→bind→set meta→thumbnail→transition. **Sensitive scope ⇒ Google app verification; 100-user cap until verified (weeks).** Quota fine. |

Key asymmetry: **Twitch/Kick = one PATCH**; **YouTube = a whole broadcast-lifecycle integration**.
**Thumbnail support: YouTube ✓, Facebook ✓, Twitch ✗, Kick ✗** (the latter two have no write API
— hide the field for them; the capability descriptor drives this automatically).
**Can't reuse `auth.obsproject.com`** (OBS's own proxy) — use direct platform flows + our own
registered OAuth clients. **Kick has no public-client path** — embed an (obfuscated) client secret
or run a thin token-exchange proxy. **RESOLVED (Phase 10.5, 2026-07-08):** built our own
token-exchange **broker** at `auth.braidcast.com` and routed **all three** providers through it
(not just Kick) so **no secret ships** in the binary; Twitch keeps only its public client id for
the Helix header. The MVP `device-code` / `pkce-loopback` strategies below were superseded by the
single `broker` strategy.

### Candidate providers beyond the MVP three (2026-06-28 research)

The registry is designed so these slot in later without touching the modal/engine.

- **Facebook Page Live — FEASIBLE, validates the thumbnail path, but highest-friction auth.**
  `POST /{page-id}/live_videos` → RTMPS ingest + a live-video id; sets `title`, `description`,
  `privacy`, `content_tags` (generic interest IDs — **no real game/category catalog** since
  Facebook Gaming was sunset). **Thumbnail:** `POST /{video-id}/thumbnails` exists (so FB is a
  second thumbnail-capable provider) — *but whether it applies before/during a live (vs only the
  saved VOD) is unverified; test before committing.* **Auth = auth-code + client-secret via an
  embedded webview** — Meta supports **no device flow, no PKCE loopback**, and its policy says
  **don't embed the secret in a binary** → this provider needs the **proxy** auth strategy (or an
  accepted-risk embed). **Meta App Review is a hard gate** (dev mode only reaches app
  admins/testers — same shape as Google's verification, possibly stricter). 60-day tokens, no
  silent refresh; Graph API has a ~2-yr-per-version deprecation cadence = ongoing maintenance.
  → Slots in as a `facebook` provider using the `auth-code+secret`/`proxy` strategy; good test of
  the thumbnail-capable + lifecycle + proxy paths. Schedule **after** the MVP three.
- **Instagram — NOT FEASIBLE via official API; do not plan a built-in.** Instagram exposes **no
  programmatic live API** for third parties: the Content Publishing API has no live CREATE path,
  and Instagram Live Producer keys are browser-only and **cannot be retrieved via API** (Meta:
  "no plans to build one"). The only path is the user manually pasting a browser-obtained stream
  key into a normal RTMP profile (which already works) — there's nothing for the app to *own*.
  Document as out-of-scope; revisit only if Meta ships a live API.

So the realistic provider set is **Twitch · Kick · YouTube · (later) Facebook Page**, spanning
all four auth strategies — which is exactly why the registry/strategy abstraction is the core of
this phase.

### Portable from `frontend_old/` (the CEF rewrite dropped it)

- `frontend_old/oauth/{Auth,OAuth,TwitchAuth,YoutubeAuth,RestreamAuth}.{cpp,hpp}` — vanilla
  OAuth2 (auth-code + refresh, loopback) — flow logic portable; Qt coupling thin.
- `frontend_old/utility/YoutubeApiWrappers.*` + `dialogs/OBSYoutubeActions.*` — the FULL YouTube
  metadata + broadcast lifecycle (insert/bind/transition, categories, `videos.update`,
  `thumbnails/set`). The richest reference; rebuild its UI in Svelte, port the API calls.
- Twitch/Restream old code = stream-key fetch only (no metadata UI). **No Kick code anywhere.**
- Build-cred pattern ALREADY present: CMake cacheVars from env (`TWITCH_CLIENTID`/`_HASH`,
  `YOUTUBE_CLIENTID`/`_SECRET`/`_HASH`, `RESTREAM_*`), XOR-obfuscated via `utility/obf.h`; CI
  already injects `secrets.TWITCH_CLIENT_ID`. Add `KICK_*` the same way.

### Proposed architecture

- **OAuth + platform API calls live in the C++ bridge**, not the JS bundle — keeps client
  secrets out of the shipped web assets, runs the loopback HTTP listener / device-code polling
  natively, and reuses the portable old OAuth logic. New bridge methods (sketch):
  `oauth.connect{platform}` / `oauth.disconnect` / `oauth.status`, `streamMeta.getCategories`
  (search/typeahead), `streamMeta.get`/`streamMeta.set{profile, fields}`. Svelte owns the modal.
- **Token store:** `%APPDATA%/braidcast/oauth_tokens.json` (ideally DPAPI-wrapped on
  Windows; at least not world-readable). Per-platform refresh handling.
- **Account ↔ profile link:** a connected OAuth account attaches to a stream profile (which
  already carries platform + key); metadata editing for a destination requires its account
  connected (inline "Connect…" otherwise).
- **Go Live modal flow:** click Go Live → modal lists each armed destination grouped by
  platform/account → platform-conditional fields (title/category/tags everywhere; thumbnail +
  privacy/description for YouTube) → confirm pushes metadata per platform, then `streaming.start`;
  partial-failure tolerant (warn, still allow going live). Also reachable mid-stream to edit.

### Proposed phasing

- **8a — OAuth foundation + first platform (Twitch). ✅ DONE 2026-06-29** (commits
  `6b0c72464`..`e613f3130`, branch `ui-redesign`). Bridge OAuth (device-code flow, no secret),
  DPAPI-wrapped per-profile token store, provider registry + `StreamProvider`/`AuthStrategy`
  interfaces, reusable `DeviceCodeStrategy`, `TwitchProvider` (Helix users/channels/search +
  `PATCH /helix/channels`, reactive-401, stream-key autofill), Streams **Connect Account / Use
  Stream Key** dual path + device-code connect modal, `streamMeta` get/set/category-search,
  `oauth.*` bridge methods. 5 tasks subagent-driven + holistic review (super-quality =
  SHIP_WITH_FIXES, 0 Critical); the 3 foundation Important items fixed (atomic token write,
  scopeVer enforcement, single-flight refresh coherence) + minors. check 0/0, build EXIT=0, smoke
  148 methods / leaks 2 / clean shutdown. **Acceptance owed (needs a real Twitch client id + GUI):**
  live connect round-trip, scopeVer-refusal, concurrent-refresh coherence, DPAPI write/read — all
  compile-clean + logic-reviewed but unexercised in portable smoke (`provider registry booted: 0`).
- **8b — Go Live modal. ✅ DONE 2026-06-29** (`a45973a07`,`1bbaf21bb`). Added a deferred-callback
  **async bridge dispatch lane** (`g_asyncMethods`/`RunAsyncMethod`/`DispatchAsync`) and moved
  `streamMeta.get/searchCategories/set` onto it (JS `await` contract unchanged; the shared lane
  8c/8d reuse). `GoLiveModal` renders entirely from capability descriptors via one
  `GoLiveFieldInput` (all 8 field types); shared-defaults block = union of every shareable field;
  per-destination override cards (empty inherits, filled overrides) per the mock; Simple/Advanced.
  Confirm pushes resolved metadata per armed+connected destination (partial-failure tolerant), then
  `streaming.start`. General toggle "Ask for stream info on Go Live" (default on) + Edit-stream-info
  button (works mid-stream). Review SHIP_WITH_FIXES (0 Crit): fixed primary-enabled-before-load,
  collapsed triplicated rendering into one descriptor-driven path, and a same-account token clobber
  reopened by going concurrent (ensureFresh is now the sole token writer).
- **8c — Kick. ✅ DONE 2026-06-29** (`3d22ebb9f`). Reusable **PKCE-loopback** strategy (127.0.0.1
  ephemeral Winsock listener, S256, CSRF state) + a generalized strategy-agnostic `authorize()`
  connect flow (`oauth.connectProgress {phase}`; device-code & browser-wait UX). `KickProvider`:
  `channel:read/write`+`user:read`, `PATCH /public/v1/channels`, `/public/v2/categories` typeahead,
  identity+channel-read prefill; redirect host `localhost` (Kick's Next.js rewrites `127.0.0.1`).
  Review SHIP_WITH_FIXES (0 Crit): reactive-401 now force-refreshes through the persisting
  single-flight path (Kick refresh tokens rotate), worker try/catch, category-id string wire type.
- **8d — YouTube. ✅ DONE 2026-06-29** (`033f45a2b`). PKCE-loopback (no secret; `access_type=offline`
  +`prompt=consent` via `Config.extraAuthParams`) + create-per-go-live lifecycle: liveBroadcasts.
  insert → liveStreams.insert → bind → videos.update (category/tags) → thumbnails.set, then writes
  the RTMP ingest URL+key into the linked profile (UI-thread `ingest_writeback`, completes before
  `streaming.start`). **enableAutoStart always true** so YouTube auto-transitions when the encoder
  connects (no manual transition hook); advanced toggle controls only `enableAutoStop`. 11-field
  descriptor (privacy/description/thumbnail + YouTube-only advanced). Review SHIP_WITH_FIXES (0 Crit;
  writeback sync + videos.update merge affirmed correct). Also standardized all providers'
  enum/labelset options on `{value,label}` (fixed Twitch language/content-labels rendering as
  `[object Object]`). **`YOUTUBE_CLIENTID` must be a Google "Desktop app" OAuth client** (Web clients
  reject loopback redirects). **Google app-verification paperwork (sensitive scope) still owed.**
- **8e — Polish. ✅ DONE 2026-06-29** (`406fc9f56`). Thumbnail preview + remove + drag-drop + 2 MB
  hint; **reconnect UX** for stale-scope tokens (distinct "⚠ Reconnect" state in Streams; excluded
  from the modal push + shown as "authorization expired" — still streams via key); human
  platform-named partial-failure toasts. (Mid-stream edit + debounced typeahead already shipped in
  8b.) Frontend-only; check 0/0, build EXIT=0.
- **Phase 8 MVP (8a–8e) COMPLETE + merged to `master`.** Acceptance owed
  (live, can't headless — needs the registered apps + creds): real connect round-trips per platform,
  metadata push, YouTube create-per-go-live + auto-transition + thumbnail + ingest writeback, Kick
  variable-loopback-port acceptance (RFC 8252), refresh/reconnect, DPAPI read/write. NOTE: portable
  smoke shows clean `[obs] shutdown complete` (leaks 2) but an intermittent **CEF-exit segfault**
  appears in the headless shell (reproduces with the pre-8e bundle; subagents smoke the same binary
  clean; outside OBS's crash handler) — a teardown/GPU artifact, not a Phase 8 regression; confirm
  close behavior in a real GUI session.
- **8f — Facebook Page (optional, post-MVP). ⏸ STILL DEFERRED — but the reason changed.** A
  `facebook` provider on the **broker** strategy: `live_videos` create + title/description/
  privacy + `thumbnails` upload (test the live-timing gap first). **The token-exchange-proxy
  blocker is gone** — 10.5 shipped `auth.braidcast.com` (deployed, all three providers brokered),
  so adding Facebook is a `providers.ts` + provider-class addition, not a new service. **The
  remaining gate is Meta App Review** (+ Business Verification + a data-deletion callback, which
  needs the 10.2 website) — start that paperwork before building. **Instagram is excluded** — no
  official live API.

### Open decisions — ✅ ALL RESOLVED (by the shipped 8a–8e MVP + the 10.5 broker)

1. **Thumbnail YouTube-only** — ✅ yes, accepted; shipped YouTube-only (8d/8e).
2. **Kick secret** — ✅ **DEAD QUESTION.** Neither horn was taken: the **10.5 OAuth broker**
   (`auth.braidcast.com`) brokers code-exchange + refresh for **all three** providers, so **no
   secret ships in the binary at all** — Kick/YouTube ship nothing; only Twitch's *public*
   client id stays baked (obfuscated) for the Helix `Client-Id` header. See 10.5.
3. **YouTube model** — ✅ create-per-go-live, as recommended (shipped in 8d).
4. **OAuth in the C++ bridge** — ✅ confirmed; lives in `frontend/src/oauth/`.
5. Sequencing **Twitch → Kick → YouTube** — ✅ confirmed; shipped in that order (8b→8c→8d).
6. **Facebook Page** — ✅ scoped as the later 8f provider, still deferred. The proxy question is
   answered: `auth.braidcast.com` exists and is deployed (10.5), so the remaining gate is Meta
   App Review, not proxy willingness. See 8f above.
7. **Instagram** — ✅ confirmed out-of-scope (no official live API; manual key paste only).

### Risks

- Google verification gate (sensitive scope) — weeks; 100-user cap until done. Start early.
- Kick API youngest/least-documented; no device flow; secret required. Build defensively (backoff,
  undocumented rate limits).
- Refresh-token lifecycle differs per platform (Twitch one-time-use + 30-day inactivity).
- Secret handling: embedded secrets are extractable — obfuscate + accept, or proxy.

---

## Phase 9 — Creator engagement layer: multichat, viewer count, alerts & widgets 🚧 9.0–9.3 DONE; rest planned

**9.0 (Multichat + Aggregate Viewer Count) ✅ DONE 2026-06-30** (commits `0b1725b24`,`c016fe362`,
`7bcccb808` on `ui-redesign`; spec `docs/superpowers/specs/2026-06-30-phase9-multichat-viewer-count-design.md`).
C++ chat transport layer on the Phase 8 registry: a libcurl-WebSocket client, a `ChatTransport`
interface + `ChatHub` (one worker per connected account, normalized messages → `chat.message`/
`chat.state` events, `chat.send` routing, start-on-go-live/stop-on-stop), and three transports —
Twitch IRC-over-WS, YouTube `liveChatMessages` long-poll, Kick Pusher (reverse-engineered) read +
REST send. Aggregate viewer count via a `ViewerPoller` + per-provider `viewerCount()` hook →
`viewers.changed` (Monitor card + Studio chip). Svelte virtualized multichat dock (native
emotes/badges, escaped text, all/per-platform send box). Chat scopes added (Twitch
`chat:read`/`chat:edit` + future EventSub `user:read:chat`/`user:write:chat`; Kick `chat:write`/
`events:subscribe`) with scopeVer bumps. Holistic review = SHIP_WITH_FIXES (0 Critical; 4 Important
fixed: Twitch reauth-budget reset, host-side fallback message id + client-seq dock key, Kick
teardown-timeout, Monitor/YouTube clear-on-stop). Also fixed the **`.env` credential wiring** (CMake
now auto-loads a CR-tolerant repo-root `.env` under the `.env`/CI names → Twitch/Kick/YouTube boot).
check 0/0, build EXIT=0, smoke 3 providers / leaks 2 / clean OBS shutdown. **GUI/credential
acceptance owed** (headless can't drive real chat): per-platform connect/read/send, viewer counts,
esp. the reverse-engineered Kick Pusher path (app key/`/api/v2` chatroom id/slug casing) + a >4h
Twitch session (reauth) + an empty-id message. Merged to `master`.

**9.1 (Third-party emotes — Twitch) ✅ DONE 2026-07-01** (`ui-redesign`). 7TV + BetterTTV +
FrankerFaceZ emotes in the Twitch multichat feed, host-side only (the dock already renders
emote fragments). On connect the channel's numeric id is resolved via Helix, then six
best-effort GETs load each provider's global + channel sets into one code→url map (channel
over global; 7TV > BTTV > FFZ); a post-pass over `BuildFragments` swaps whole-word
case-sensitive matches in text fragments for emote fragments, leaving native Twitch emotes
untouched. Fetches are non-fatal, poll the cancel token before each request (5s timeout), and
resolve only https URLs. Runtime substitution owed a live-session check.

**9.2 (Alerts / Events feed) ✅ DONE 2026-07-01** (`ui-redesign`; spec + plan under
`docs/superpowers/`). A persisted, de-duplicated, cross-platform live-events feed on the
account-connect lifecycle (always-on, not go-live-gated) with an Events dock. `EventHub` mirrors
the chat layer (worker per account, generation-flag cancel, alive-guarded emit, boot sweep for
already-connected accounts); `EventStore` persists a capped per-id-deduped ring to `events.json`;
`events()` provider hook; bridge `events.list`/`events.clear` + `events.new`/`events.backfill`
(full store snapshot, newest-first, at UI-dispatch time). Transports: **Twitch** EventSub WS
(follow/sub/resub/gift/cheer/raid; graceful per-sub scope degradation; scope bump v2→v3 needs a
one-time re-auth; followers backfill; revocation re-subscribes), **YouTube** REST backfill+90s
poll (Super Chats/Stickers + recent subscribers) with a live-state gate + content-derived id so
the live chat forward and REST never double-count, **Kick** best-effort reverse-engineered Pusher
(sub/gift/host; nameless follows dropped) sharing `kick_pusher` with chat. EventsDock: per-type
icons/colors, summary registry, Intl currency formatting, platform filter. Each subphase
per-phase-reviewed (super-quality, all Critical/Important fixed). **Live acceptance owed** across
all three (headless can't drive real follows/subs/cheers/superchats) + the reverse-engineered Kick
names may drift.

**9.3 (Overlay widgets — alert box) ✅ DONE 2026-07-01** (`ui-redesign`; spec + plan under
`docs/superpowers/`). A StreamElements-style overlay layer: v1 ships the full infrastructure plus
one widget type (alert box). A NEW loopback-only (`127.0.0.1`) winsock HTTP server (`frontend/src/
overlay/overlay_server.*`, distinct from `mcp/HttpServer` — it adds GET routing, static serving off
the on-disk web bundle, and long-lived concurrent **SSE**) serves an assembled widget document +
the compiled `OBSOverlay` runtime + user assets; the user copies a widget URL into an OBS Browser
Source. Widgets persist to a global `overlays.json` (`overlay_store`, crash-safe tmp+bak, mirrors
`event_store`); `overlays.*` bridge methods (list/get/create/update/duplicate/delete/url/test/
serverInfo/uploadAsset/addToScene). The single sink `EventHub::Ingest` fans each `NormalizedEvent`
to both the UI and `Overlay::Server().Broadcast` (test alerts inject via `BroadcastTo`, never the
store). Clean native widget API (`OBSOverlay.onLoad/onEvent/fields/playSound` + `obs:load`/`obs:event`,
EventSource over SSE) built on the existing `NormalizedEvent` shape — the runtime is emitted as a
classic IIFE (`window.OBSOverlay`, no ESM) so a plain `<script src>` runs it in the Browser Source.
Svelte "Overlays" nav page (master-detail): CodeMirror 6 HTML/CSS/JS panes, a Fields designer +
values panel (text/number/color/dropdown/checkbox/slider/image-upload/sound-upload/font), an iframe
preview with per-event-type test buttons, Copy-URL + add-to-scene. Port: persist-first (default
43000) with a scattered-band fallback scan (43000/47000/51000/55000/59000 × 50) so a Hyper-V/WSL/
Docker port reservation can't kill the feature; `portChanged`/`!listening` surfaced in the UI.
Group-1 (socket server) got a dedicated deep review (fixed I1 shutdown-hang on untimed header recv,
I2 fd-reuse wrong-close via shutdown()-not-close sole-closer, I3 mutex-across-blocking-send) + a
final holistic review (fixed a Critical: uploaded asset field URLs rewritten to
`/w/<id>/assets/<file>?t=<token>` at serve time in `AssembleDocument`). check 0/0, build EXIT=0, an
over-a-real-socket self-test (GET doc + SSE delivery + wrong-token 403) plus overlays create/list/
persist/delete all green, clean shutdown. **GUI acceptance owed** (headless can't drive a real
Browser Source): live editing round-trip, image/sound upload→render, test-button alerts, two-instance
token isolation, delete-cleans-assets, restart-URL-stability.

**9.1b (Third-party emotes — Kick + YouTube) ✅ DONE 2026-07-02** (`ui-redesign`, commit on
`origin/ui-redesign`). Generalized the Twitch-only 9.1 resolver with a per-platform config table
(globals + 7TV/BTTV channel slugs; FFZ stays Twitch-only) and moved the word-substitution pass into a
shared `Chat::ApplyThirdPartyEmotes`. **Kick** = 7TV global + 7TV/BTTV `kick/<channelId>` (over the
already-resolved channel id; native `[emote:…]` still wins); **YouTube** = 7TV+BTTV global +
`youtube/<channelId>` off the connected account's UC id. Both fetch once on the transport worker and
cache. Build clean /W4 /WX, smoke `leaks: 2`. **Live rendering GUI-owed** (headless can't drive real
Kick/YouTube chat).

**9.3b (Chat box overlay widget) ✅ DONE 2026-07-02** (`ui-redesign`, commit on `origin/ui-redesign`).
A second overlay widget type rendering live multichat as a browser source, reusing the 9.3 overlay
infra + 9.0 chat hub — this also covers "chat-as-source". Chat rides the existing per-widget SSE
stream as a **named `chat` event** (`OverlayServer::BroadcastChat` via a shared `BroadcastFrame`,
fanned from `ChatHub::RouteEmit` on `chat.message` only; alert-box `data:`-only frames unaffected).
Runtime gains `OBSOverlay.onChat` + a `chat` SSE listener; a `default-chatbox` template
(html/css/js/fields) renders author color + opt-in platform tag/badges + emote fragments, XSS-safe
(`createElement`/`textContent`), with max-messages / per-message lifetime / entry-animation fields.
The Overlays editor create flow now has a widget-type picker (alert box / chat box). Build clean /W4
/WX, `bun run check` 0/0, smoke `leaks: 2` + overlay SSE selftests green. **Live render in a real
Browser Source GUI-owed.**

**9.3c (Event ticker overlay widget) ✅ DONE 2026-07-02** (`ui-redesign`, commit on `origin/ui-redesign`).
Third overlay widget type: a continuously scrolling ticker of recent events (follows/subs/gifts/
cheers/raids/super chats) as a browser source. **Web-only** — events already broadcast over the widget
SSE stream and the runtime already has `OBSOverlay.onEvent`, so no backend/runtime change. A
`default-ticker` template runs a JS conveyor over a capped rolling buffer (append-at-back/drop-at-front
off-screen so appends never stutter); RTL = a CSS `scaleX(-1)` mirror + counter-flip (one code path
both directions). Event phrasing/emoji/money-format/palette ported from EventsDock; XSS-safe. Fields:
font/size/colors, scroll speed, direction, separator, platform tag, max items, idle text. Editor
type picker gains it. check 0/0, build green, smoke `leaks: 2`. **Live Browser-Source render GUI-owed**
(esp. the RTL mirror).

**9.3d (Goal bar + Labels overlay widgets) ✅ DONE 2026-07-02** (`ui-redesign`, commit on
`origin/ui-redesign`). Final two widget types, both **web-only** via `OBSOverlay.onEvent`. **Goal Bar**
counts matching events since load toward a target (followers/subs/gifted-subs/bits/donations, per-type
increments + a `startCurrent` seed) → progress bar. **Labels** shows the latest actor for an event
type OR a running session count via a `{name}/{count}/{amount}` format template. Event-type strings
match the transports (`follow`/`sub`/`resub`/`subgift`/`cheer`/`raid`/`superchat`/`supersticker`);
XSS-safe. Editor type picker gains both. check 0/0, build green, smoke `leaks: 2`. The overlay widget
family is now complete: **alert box · chat box · ticker · goal bar · labels**. **Live render GUI-owed.**

**Remaining Phase 9 (planned):**
- **Moderation** — per-platform mod actions in multichat (timeout/ban/delete-message) where OAuth
  scopes allow. Needs a design pass (scopes per platform + a compact moderation UI on chat rows).
- ✅ **Pre-live chat — DONE 2026-07-02** (`ui-redesign`, commit on `origin/ui-redesign`). Chat was
  live-gated; `ChatHub` now has `EventHub`'s always-on account lifecycle: `Chat::Hub().Start()` on
  boot (connected-account sweep, `obs_bootstrap.cpp`) and on account connect/disconnect
  (`bridge.cpp`), so multichat + chat-box/overlay widgets work before/after streaming. Go-live still
  re-Start()s so **YouTube** picks up its live-broadcast `liveChatId` (Twitch/Kick connect pre-live;
  YT no-ops until live); go-live-stop re-resolves instead of stopping (drops each provider's
  active-broadcast target first → YT falls back to no-op, Twitch/Kick persist). Hub still stopped at
  shutdown. Build clean, smoke shows `[chat] hub started` at boot, `leaks: 2`. **Live multi-account
  acceptance GUI-owed.**

**Goal:** the Streamlabs/StreamElements-style live layer the fork lacks — unified **multichat**
(read+send across every connected platform in one pane), **aggregate viewer count** (sum of live
viewers across destinations), and the broader **alerts / events / overlay widgets** family
(follows, subs, donations, chat-as-a-source, viewer goals). This is the natural payoff of the
Phase 8 OAuth foundation: the same provider registry + per-profile accounts now also carry the
**chat + events** scopes (Kick `chat:write`/`events:subscribe`, Twitch chat/EventSub, YouTube
liveChat), so engagement features ride the existing connection rather than a new auth stack.

**Why it builds on Phase 8 cleanly:** the `StreamProvider` registry is already the extensibility
seam. Phase 9 adds, per provider, optional **chat** + **event** capability hooks (connect to the
platform's chat transport, normalize messages/events to a common shape) — adding a platform's chat
stays ~one module, mirroring how metadata providers slot in. Enable the extra OAuth scopes now
(Kick form: Write to Chat feed + Subscribe to events; Twitch: chat scopes; YouTube: youtube
scope already covers liveChat) so accounts connected today are forward-compatible.

**Candidate sub-features (to brainstorm/sequence later):**
- **Multichat dock** — one merged, platform-tagged feed; send-to-all or per-platform; moderation
  actions where scoped. Per-platform chat transports: Twitch IRC/EventSub, Kick (chat:write +
  events:subscribe / Pusher), YouTube liveChatMessages.
- **Aggregate viewer count** — poll/subscribe each live destination's viewer count; show total +
  per-platform breakdown (a Monitor-page card + a Studio bar chip).
- **Alerts / events feed** — follows / subs / gifts / (Kick) Kicks / (YT) superchat → a normalized
  event stream driving on-screen alert widgets + a Studio events ticker.
- **Overlay widgets as browser sources** — alert box, chat box, viewer-goal, recent-events — local
  widget pages the user adds as browser sources (and/or a built-in overlay compositor on a canvas).
- **Chat as a source** — render the merged chat into a scene (browser source or native).

**Open questions for the Phase 9 brainstorm:** widget hosting (local HTTP server for browser-source
widget pages vs native overlay render); event transport per platform (poll vs websocket/EventSub);
whether to compose overlays on a dedicated canvas; donation/tip integrations (likely third-party,
e.g. StreamElements/Streamlabs tip APIs) and whether those are in scope. Formalize after Phase 8
ships and the platform accounts/scopes are proven.

---

## Phase 10 — Account entity, public website & OAuth compliance 🔧 in progress (2026-07-04)

Hardening the platform-integration layer for a real (verifiable) public launch:
a first-class Account entity, a public marketing/legal website, an **OAuth
token-exchange broker** (10.5, built 2026-07-08 — moves all provider secrets out
of the binary), and the OAuth verification groundwork. Prompted by an
entity-independence audit + the Google OAuth verification question. Brand
**resolved to Braidcast** (10.4); the broker + site live in a separate repo
(`braidcast-website`) under the public **`braidcast` GitHub org**.

### 10.1 — First-class Account entity 🔧 Phase 1 done
Replaces the `profileUuid`-as-account-identity conflation (audit finding: three
subsystems keyed the same account three ways; a 2nd same-platform account
evicted/deadlocked). `accountId = "providerId:userId"` becomes the single identity
across a renamed `AccountStore`, all three hubs (chat/events/viewers), and
per-account transport instances; stream profiles reference an account by id, which
also delivers **connect-once account reuse** (deferred 10e). Spec + plan:
`docs/superpowers/{specs,plans}/2026-07-04-account-entity*` (gitignored).
- ✅ **Phase 1** — entity + `AccountStore` re-key + `ensureFresh` cleanup +
  `StreamProfile.accountId` + bridge connect/status/disconnect rework
  (`6d10529e1`, build-green + smoke `leaks: 2`, spec-review SPEC-COMPLIANT).
- 🔧 **Phase 2+3** — provider transport factories (`makeChat`/`makeEvents` per
  account, hub-owned `shared_ptr` shared with the worker) + accountId-keyed hubs +
  YouTube per-account `BroadcastState`.
- 🔭 **Phase 4** — `oauth.accounts`/`linkAccount` + Streams reuse picker +
  disconnect-with-refs confirm. **Folds in a required fix:** `streamMeta.get/set`
  must take `accountId` directly (they currently read the unguarded profile store
  off the async worker lane — a latent UAF; see the spec-review).

### 10.2 — Public website + broker repo 🔧 scaffolded (`braidcast-website`)
Lives in a **separate repo `braidcast-website`** (not `website/` in the fork; not a
monorepo) with two halves: `broker/` (the 10.5 Worker) and `web/` (the static site).
The site's jobs: (1) the **OAuth homepage + privacy policy + terms** URLs Google/Meta
verification require (a plain-HTML stub exists — `web/index.html` + `web/privacy.html`;
a richer marketing site, e.g. Astro, is a follow-on); (2) later, host the **Facebook
Login** flow (Meta needs a public privacy-policy + data-deletion callback URL). Deploys
as a **Cloudflare Pages** project on `braidcast.com`. Brand is resolved (10.4 → Braidcast),
so the earlier brand-agnostic-constant caveat is moot.

### 10.3 — OAuth verification & compliance 🔭 research
- **Google/YouTube:** one **sensitive** scope (`youtube.force-ssl`) — **not**
  restricted, so **no CASA security audit** ever. Nothing owed while the OAuth
  consent screen stays in **Testing** mode (≤100 testers; note the 7-day
  refresh-token expiry). Full sensitive-scope verification (homepage + privacy
  policy + verified domain + demo video) owed only at public launch. **The Google
  client secret is now broker-held (10.5)** → the **Desktop** client had to be replaced
  with a NEW "Web application" client (app type is immutable) whose redirect URI is
  `https://auth.braidcast.com/v1/youtube/callback` — **✅ DONE**, see 10.5 "DEPLOYED LIVE
  (2026-07-08)": providers registered with YouTube on a Web-app client.
- **Twitch / Kick:** app-registration requirements captured in the research doc.
- **Facebook/Meta (8f, still deferred):** App Review + Business Verification +
  data-deletion callback — needs the website first.
- Grounded research: `docs/research/oauth-verification-requirements.md`.

### 10.4 — Brand / trademark clearance ✅ RESOLVED → **Braidcast** (2026-07-04)
**"OBS" is the OBS Project's trademark; GPLv2 licenses the code, not the name.** A
public product / OAuth app **cannot** ship as "OBS MultiStreamer" (OBS trademark
policy + Google/Meta OAuth app-name/brand rules). **Resolved:** rebranded to the
distinctive standalone brand **Braidcast** (exe/target/config/app-id/repo all
`braidcast`; app icon = the amber "rope" mark), describing itself as "built on OBS
Studio" (nominative fair use) in body copy only. Domain `braidcast.com` bought;
GitHub org `braidcast` created; fork repo `braidcast/braidcast-studio`; on-disk folder
renamed `obs-multistream` → `D:\Projects\gui\braidcast\braidcast-studio` (2026-07-08).
Grounded research: `docs/research/branding-trademark.md`. (Rebrand detail is in the
`obs-rebrand` project memory.)

### 10.5 — OAuth token-exchange broker ✅ BUILT 2026-07-08 (local; deploy + live-test owed)

A **stateless Cloudflare Worker** at `auth.braidcast.com` (mirrors OBS's
`auth.obsproject.com`) that holds the platform client **secrets** and proxies the
OAuth **code-exchange + refresh** so the desktop app ships **no secrets** — closing
the Phase-8 seam (a leaked YouTube/Google client id inherits OBS's verified elevated
quota with zero verification). **All three providers brokered.** Two work streams,
both build-green, holistic review = **SHIP** (3 minors fixed), **nothing pushed**:

- **Broker + site** (new repo `braidcast-website`, TypeScript + Hono on Cloudflare
  Workers, vitest, `bun`) — routes `/v1/:platform/{start,callback,token}`;
  **HMAC-signed state** (`{p,port,nonce}` + HMAC-SHA256); `/callback` 302s only to a
  **hardcoded** `http://127.0.0.1:{validated-port}/` (no open-redirect); `/token`
  injects `client_id`+`client_secret` from **env only** (ignores client-supplied
  creds), grant-type allowlisted, response passed through verbatim; PKCE forwarded
  only for `pkce:true` (kick/youtube; twitch `pkce:false`). Stateless — **no DB**
  (refresh tokens stay client-side). 20/20 vitest; `wrangler deploy --dry-run` clean.
  Commits **local** `b682e22..33b8bf8`.
- **Client migration** (`obs-multistream`, branch `ui-redesign`, `eef013605..25ee8c256`
  + `e53cb5d36`) — new `frontend/src/oauth/broker_strategy.*` (`BrokerStrategy :
  AuthStrategy`) + `loopback_listener.*` (dual-stack Winsock, extracted from the old
  pkce-loopback); all three providers rewired (`capabilityJson` `strategy:"broker"`,
  `needsSecret:false`); dead `device_code.*` + `pkce_loopback.*` deleted; `provider_creds`
  trimmed to Twitch-only; `BRAIDCAST_BROKER_URL` compile-time macro (env-overridable,
  default `https://auth.braidcast.com`). **KEY INVARIANT:** Twitch keeps its **PUBLIC**
  client id baked (obfuscated) for the Helix `Client-Id` header (broker proxies only
  OAuth, not Helix); Kick/YouTube ship **nothing** and register **unconditionally** in
  `BootProviders()`, Twitch stays gated on `TwitchConfigured()`.

Spec/plan: `docs/superpowers/{specs,plans}/2026-07-05-oauth-broker*`; research
`docs/research/oauth-broker-obs-mechanics.md`. **Handover for the next thread:**
`docs/superpowers/sessions/2026-07-08-oauth-broker-handover.md` (deployment checklist,
folder-move costs, memory-migration steps, auto-update options).

**DEPLOYED LIVE (2026-07-08):** fork → `braidcast/braidcast-studio`, broker → `braidcast/braidcast-website`
(both public). Worker on custom domain `auth.braidcast.com` + Pages site on `braidcast.com`;
all 7 secrets set; verified end-to-end (all 3 `/start` 302 to the right host with a populated
`client_id`). Secret naming unified to `*_CLIENTID`/`*_CLIENTSECRET` (+ `STATE_HMAC_KEY`) across
`.env` build var + broker + CF. Providers registered (YouTube = Web-app client; Twitch = Confidential).
**Owed:** (1) ~~**Twitch rebuild** — the current binary bakes the OLD, now-deleted Twitch app~~
**RESOLVED 2026-07-15 — the claim was false.** The id in `.env` is live: Twitch's token endpoint
returns `403 invalid client secret` for it (a deleted app returns `400 invalid client`, as a bogus
control id does). No rebuild owed. Still unverified: CI bakes `secrets.TWITCH_CLIENT_ID`, which
can't be read back, so confirm it matches `.env`; (2) a real
connect→token→refresh round-trip per provider from the app; (3) a per-IP rate-limit rule (CF
dashboard); (4) confirm no Kick/YouTube creds in the binary. Handover:
`docs/superpowers/sessions/2026-07-08-oauth-broker-handover.md`.

### 10.6 — Auto-update 🔭 NOT built (design pending)
CI cuts a **Windows-x64 draft GitHub Release on `v*` tags**; there is no update-check /
feed / in-app updater yet. Options (recommend the MVP first): **(a) GitHub Releases
check** — poll the Releases API on launch, toast + open the installer download (reuses
what CI produces; no delta patches); **(b) WinSparkle appcast** on braidcast.com (signed,
background install); **(c) OBS's native updater** (signed manifest + delta patches — its
own project: signing key, CDN/R2, manifest gen in CI). Release assets are free/unlimited
on public repos (separate from the 500 MB Packages quota).

### 10.7 — Packaging, install & release gates ✅ SHIPPED

- ✅ **Boot-smoke CI gate** (`214ba9817`) — `.github/scripts/Smoke-Package.ps1` asserts the
  packaged **zip** *and* the **NSIS installer** each contain the web bundle, the CEF runtime,
  and every dependency DLL, plus a best-effort launch; wired into the `windows-build` job.
  Closes the "CI green didn't prove the package launches" gap that shipped three gutted
  packages (rundir `POST_BUILD` copies without matching `install()` rules).
- ✅ **NSIS installer** (`9bc5ec366`) — `CPACK_GENERATOR "ZIP;NSIS"` + `CPACK_MONOLITHIC_INSTALL`;
  releases now ship a portable **zip** *and* an **installer**.
- ✅ **Portable / isolated config + single-instance guard** (`8b555a1f5`) —
  `StorePaths::BraidcastConfigDir()` is the single seam every store resolves through: a
  `braidcast_portable.txt` marker next to the exe redirects config to `<exe_dir>/config`,
  else `%APPDATA%\braidcast`. The dev rundir auto-isolates via a rundir-only marker (the
  package omits it), so dev runs can't touch the user's real config. `main.cpp` holds a named
  mutex keyed on the resolved config dir — two portable copies coexist, two instances on one
  config don't.
- ✅ **Subsystem release changelog** (`987a163c9`, `2599e8597`) — changelogen renders release
  notes grouped by the module prefixes `CONTRIBUTING.md` mandates (`libobs:`, `frontend:`,
  `cmake:`, `CI:`, …); handles the first-release (no previous tag) case and caps body length
  under GitHub's 125k limit.

---

## Backlog & deferred decisions ⏸

- 🔭 **Theme packs (full style selector)** — today's Appearance only varies accent/
  mode/density on ONE fixed visual language (0-radius, current card/chip chrome —
  locked design decision from 7.0). Add a top-level **Theme** selector alongside it:
  each pack bundles corner-radius, shadow/elevation treatment, card and pill/chip
  shape, and spacing rhythm as one named preset — accent/mode/density still apply
  *within* whichever pack is active, they don't pick the pack. Ship with 2 packs at
  launch: **Studio Dark** (current look, becomes the default pack) and a new
  **rounded/soft-card pack** seeded from a mockup the user liked (`unified.png` —
  dark bg, amber accent, rounded-xl cards/buttons, pill badges, softer modal
  chrome) — reusing the mock's spirit, not a literal pixel port. Needs: (a) a token
  schema extension beyond today's per-token color/mode axes — radius/shadow/shape
  become pack-level, not user-editable per-token like color is; (b) decide whether
  the existing custom per-token theme editor becomes "customize within a pack" or
  stays pack-agnostic; (c) Appearance page UI for the pack picker (Phase 7.3's
  Settings → Appearance is the natural home). Design-level only — not spec'd or
  planned yet.
- 🔭 **Smart canvas settings** — today, creating a canvas leaves resolution/FPS/
  encoder entirely to the user (`CanvasEditorDialog`). Add an optional "auto"
  mode that proposes sane defaults instead: resolution/FPS inferred from the
  canvas's intended destinations (e.g. a canvas bound only to a 1080p60 output
  shouldn't default to 4K30), and encoder/bitrate ballpark inferred from local
  hardware (GPU encoder availability via existing NVENC/QSV/AMF capability
  probing, CPU core count for x264 fallback). Proposal only, never silently
  overrides an explicit user choice — surfaces as a suggestion in the canvas
  editor, not a hidden default. Needs: (a) decide the signal source for
  "intended destinations" (an output binding may not exist yet when the canvas
  is first created — chicken/egg with the Outputs tab), (b) a hardware-capability
  → bitrate/preset heuristic table (or reuse whatever auto-config logic
  upstream OBS already ships, if any, rather than inventing one), (c) whether
  this lives in `CanvasEditorDialog` itself or a separate "New canvas" wizard
  flow. Design-level only — not spec'd or planned yet.
- 🔧 **Render-thread efficiency — inert idle canvases** — RCA (2026-07-12) of a
  live frame-drop incident (`2026-07-11 22-49-38.txt`): rendering-lag and
  encoding-lag tracked ~1:1 at 25.6% with zero network drops, i.e. the encoders
  were *starved by the compositor*, not saturated. Root cause: the graphics
  thread composites EVERY canvas mix each tick regardless of any consumer
  (`obs-video.c:547` composites unconditionally; the `raw/gpu_active` gate only
  skips download/encode). A destination-less/disabled runtime canvas (e.g. the
  idle 4K "51k AV1") still pays a full composite → render-thread overload →
  dropped *stream* frames + laggy preview (they share the one graphics thread).
  **Tier 1 (✅ 2026-07-12, verified in `2026-07-12 23-05-12.txt`):** runtime
  canvases go fully inert (mix removed from `obs->video.mixes`) when they have no
  enabled destination AND no open preview; rebuilt on destination-enable or
  preview-open. Makes destination-toggling actually disable a canvas everywhere,
  not just its encoder. Together with **preview-composite dedup** (per-canvas
  preview docks now blit the already-composited texture instead of
  re-compositing), a live 4-canvas session went from the 25.6% / 48.2% incident
  range to ~0.47% frame loss. **Tier 2 (✅ 2026-07-12, pending live black-screen
  test):** the Main/Default canvas composite is now gated on having a consumer
  via a libobs refcount (`obs_inc/dec_main_render_needed`, checked in
  `output_frame`), held by the central preview, program projector, DeckLink main
  output, and program screenshot. Gated per-mix `[render-debug]` composite
  timing confirmed Main burned 5.1%-frame GPU with zero consumers — OBS always
  composites the main canvas, so this makes Braidcast *beat* OBS on idle cost.
  **↩️ Tier 2 REVERTED (2026-07-13, commit `ec09070b0`).** The Main gate was
  built for a *render-lag* diagnosis that proved wrong: the render lag the user
  actually hit is **focus-dependent** (Braidcast fully visible on a 2nd monitor,
  lagging only while a game holds the foreground on the 1st — refocusing stops
  it), which is **Windows background power throttling** (EcoQoS + `timeBeginPeriod`
  revocation on foreground loss), NOT compositor starvation. Fixed in `main.cpp`
  with a `SetProcessInformation(ProcessPowerThrottling)` opt-out clearing
  EXECUTION_SPEED + IGNORE_TIMER_RESOLUTION (commit `0a997b78e`). The Main gate
  saved compositing only while *fully idle* (never during streaming — an output
  keeps Main composited) and added ref-balance black-screen fragility, so it was
  reverted to stock OBS unconditional Main composite. **Tier 1 (inert non-default
  canvases) is unaffected and stays** — it fixed a genuine idle-canvas composite
  cost. The perf-repro self-test was retargeted off the starvation theory to guard
  the throttle opt-out (commit `d1149dc6b`). Note: upstream OBS has NO main-process
  throttle opt-out either (verified), so *why* stock OBS doesn't show this lag is
  unconfirmed — live focus-loss test owed.
  Still open: a **preview-fps cap + real occlusion detection** — per-canvas
  previews currently render at full output fps, unthrottled, and only stop on
  DOM-hidden, not OS-window occlusion (`previewSurface.ts` / `render_displays`).
- 🔭 **Adaptive compositor — load-aware parallelism** — **DEPRIORITIZED
  (2026-07-13).** Partly motivated by the render-lag incident that turned out to
  be Windows background throttling (fixed — see the inert-idle-canvases entry
  above), not compositor cost. The per-canvas composite cost is real but only
  bites under heavy multi-canvas high-res load, which has not been measured as the
  ceiling on any real session since Tier 1 shipped. Do NOT build this speculatively;
  revisit only if measured multi-canvas load proves composite time is the bottleneck.
  Design retained for reference. The multi-canvas model composites once *per canvas
  per frame* on a single serial graphics thread (upstream OBS composites once), so
  compositor cost scales with canvas count. Were this pursued, the ceiling lift is
  a **two-layer** design, gated on measured load — NOT a static GPU tier:
  - **Always-on (every GPU, no gate):** work reduction — inert canvases +
    preview dedup (Main gate reverted, see above), plus **shared/derived compositing**
    (render the scene once, derive the 16:9 / 9:16 / … variants by crop/scale
    instead of a full re-composite per canvas). Pure win, and the *biggest*
    lever for weak GPUs since it cuts total fill work rather than spreading it.
  - **Adaptive (measured-headroom gate):** composite canvases concurrently and
    overlap composite↔encode (separate silicon), enabled only when there is
    spare frame-time margin and falling back to serial when saturated. Gate on
    *measured* per-frame margin + encoder-starvation + GPU util (the
    `[render-debug]` signals), NOT a small/med/large lookup: a mid GPU on a
    heavy scene is saturated too, and hardware detection (naming / driver /
    iGPU / laptop power) rots. Self-tunes as the scene changes mid-stream; ships
    with a manual override (auto / force-serial / force-parallel) and the margin
    surfaced for debugging.
  Rationale: parallelism only reclaims *idle* GPU time, so it helps GPUs with
  headroom (the 5070 Ti incident: ~50% util, NVENC idle), not a genuinely
  saturated small GPU — those win from the always-on reduction + shared-composite
  track. Design-level; sequence work-reduction first (mostly shipped), the
  adaptive layer later.
- 🔧 **Windows process-init parity** — audit (2026-07-13, prompted by the
  background-throttle miss) of what the from-scratch CEF `wWinMain` never
  replicated from upstream's `frontend/obs-main.cpp` process/thread setup.
  **✅ Shipped:** (a) **sleep/display inhibit around output start/stop** —
  `os_inhibit_sleep_*` was defined in libobs but never called from `frontend/`, so
  Windows could blank the display or sleep the machine mid-broadcast; now wired
  into `MultistreamEngine` output start/stop. (b) **main-process crash handler /
  minidump** — `obs_init_win32_crash_handler` was never called, so a main-process
  crash produced no dump; now called in `wWinMain`. (c) **audio-ducking opt-out** —
  `DisableAudioDucking` (`IAudioSessionControl2::SetDuckingPreference`) + an
  Advanced-tab toggle, so capturing audio no longer force-ducks other apps (default
  off, matching OBS). (d) **process-hardening preamble** in `wWinMain` — mitigation
  policy (`PreferSystem32Images`), `SetErrorMode`, safe DLL-search
  (`SetSearchPathMode` + `SetDefaultDllDirectories`, reconciled with the app's
  loader via `AddDllDirectory`, boot-tested), `SetProcessShutdownParameters`,
  `RtwqStartup`/`RtwqShutdown` (MF hardware-encoder thread scheduling), and
  `load_debug_privilege` (`SE_DEBUG` + `SE_INC_BASE_PRIORITY_NAME`, the privilege
  the `GPU_PRIORITY_VAL`-gated D3D11 GPU-priority path needs). **🔭 Deferred:** the
  **DLL-blocklist hook** (`install_dll_blocklist_hook`) — a `LoadLibrary` interposer
  with a maintained blocklist of known-bad DLLs; not in-tree (would need to port
  upstream's detour), disproportionate for a controlled private fork, low return.
  Non-gaps confirmed present-and-correct or matching
  upstream: `timeBeginPeriod(1)` (fires via libobs `DllMain`), DPI awareness,
  process priority class, MMCSS (audio-thread-only, same as upstream), COM init +
  hotkey thread (libobs-internal). Note: upstream OBS is NOT ahead here in a way a
  merge would fix — the fork tracks upstream closely (merge-base 2026-06-09,
  upstream +3 CI-only commits since); these are from the CEF entry rewrite, not
  from lagging upstream.
- 🔧 **Modern audio-plugin host (VST3 + CLAP)** — `obs-vst` is in-tree and
  maintained upstream (the archived standalone repo was folded into the
  obs-studio monorepo). It was **VST2-only** and **Qt-coupled**, so it couldn't
  load in the CEF build (Qt6 not deployed) — the VST filter was dead.
  **✅ Near-term (B) — SHIPPED (Windows), verified 2026-07-15:** the VST2 editor
  is hosted in a native Win32 `HWND` instead of a `QWidget`, so the module links
  **no Qt** and loads in the CEF build — full VST2 incl. its editor GUI = backward
  compatibility for existing plugins. Verified in-tree: `plugins/obs-vst/
  CMakeLists.txt` takes an `OS_WINDOWS` branch that adds `win/EditorWidget-win.cpp`
  + `win/VSTPlugin-win.cpp` and links only `OBS::libobs` + `bcrypt` — `find_package(
  Qt6)`/`Qt::Widgets` and the AUTOMOC/AUTOUIC properties are confined to the `else()`
  (macOS/Linux) branch, which is **still Qt-coupled**. `EditorWidget-win.cpp` is a
  real Win32 host (`WNDCLASSEXW` + `CreateWindowExW` + `wndProc`, zero Qt includes),
  and `obs-vst` is **absent from `kDenylist` in `frontend/src/obs_bootstrap.cpp`**
  (`{frontend-tools, decklink-output-ui, decklink-captions, aja-output-ui,
  obs-websocket}`), so the curated full-set loader loads it. **Still owed:** the
  macOS/Linux editors remain `QWidget`-based (cross-platform Qt-free port), and a
  GUI pass loading a real VST2 plugin + opening its editor. **Forward
  (surpass-OBS):** add a **VST3 + CLAP** host — OBS being VST2-only is a real
  weakness to leapfrog; modern plugins ship VST3/CLAP. Together = backward
  (VST2) + forward (VST3/CLAP) coverage, ahead of stock OBS. Needs: SDK
  integration (VST3 SDK license/GPL-compat check; CLAP is permissive), plugin
  scanning + ideally out-of-process sandboxing, cross-platform native editor
  hosting (Win32/NSView/X11), parameter persistence. **B is done on Windows** (the
  Win32 editor host also de-risks the VST3/CLAP editor-hosting piece); VST3/CLAP
  remains design-level.
- ✅ **Cross-canvas scene duplicate** (2026-07-09) — `scenes.duplicateToCanvas`
  bridge method deep-copies a scene (scene filters + every item's source, incl.
  that source's own filters, via `OBS_SCENE_DUP_COPY`) from any canvas onto any
  other canvas, reparented via `obs_canvas_move_scene`. Grouped undo/redo (one
  Ctrl+Z removes the whole duplicate; redo restores from captured snapshot data,
  uuid-preserving). Scene-link mapping copied to the destination when the source
  scene is itself a link target. "Duplicate to canvas ▸" submenu in both
  `CanvasDock.svelte` and `ScenesDock.svelte`. Headless self-test
  (`RunSceneDuplicateSelfTest`) passes clean (real independent copy, undo, redo,
  uuid-stable restore). Along the way, found and fixed a real pre-existing
  use-after-free in `libobs/obs-canvas.c`'s `obs_canvas_move_scene`/
  `obs_canvas_insert_source` (a moved scene's `context.mutex` stayed pinned to its
  prior canvas indefinitely, dangling once that canvas was destroyed — reachable
  via source destroy AND scene rename; fixed at both the write site and
  defense-in-depth at the destroy site). Two known narrow follow-ups logged in
  `docs/issues.md` #5 (nested-group grandchildren not captured on redo;
  duplicate-uuid collision when the same source is used twice in one scene) —
  non-blocking, don't trigger on ordinary scenes. **GUI acceptance owed**
  (submenu rendering/visual placement, real drag-independence check between
  original and duplicate, undo/redo via actual Ctrl+Z/Y keypress rather than the
  self-test's direct `Undo()`/`Redo()` calls, scene-link copy visually confirmed
  in the Scene Link UI).
- ✅ **Transform UX fixes: rotate-out-of-bounds, resize snapping, outline/aspect
  investigation** (2026-07-09) — three bundled fixes to canvas-item transform.
  (1) **Rotate-about-center**: `obs_sceneitem_set_rot` pivots around the item's
  alignment-anchor corner, not its visual center, so rotating flung items off
  into unreachable space. `RepositionForCenterPivot` (`bridge.cpp`) now measures
  the item's AABB before/after rotation via `GetSceneItemBox` and corrects `pos`
  to keep the visual center fixed, wired into both `sceneItems.setTransform` and
  the rotate quick-actions — gated off when the caller supplies an explicit `pos`
  in the same call (paste-transform, MCP `set_item_transform`) so it never
  overrides an intentional move. (2) **Bounds-safety clamp**: `ClampItemToCanvas`
  nudges `pos` post-transform so at least ~32px of the item's AABB stays
  overlapping the canvas, applied to `setTransform` and every transform
  quick-action (reset/flip/rotate90*) — explicitly excluded from the undo/redo
  path, which restores exact prior state. (3) **Resize-drag snapping**: edge/
  center snap (already wired for move-drag via `CanvasSnapOffset`) extended to
  `StretchItem` using a "probe box" technique — collapses the fixed anchor edge
  onto the live moving edge per axis, reuses `CanvasSnapOffset` unchanged, and
  converts the returned canvas-space delta back to item-local space via a
  rotation-only inverse matrix. Headless self-tests
  (`RunTransformPivotSelfTest`, `RunRotationBoundsSelfTest`) pass clean:
  zero center-drift across both rotation entry points, clamp recovers an
  off-canvas item, and AABB width/height swap correctly across a 90° rotation
  for both `OBS_BOUNDS_NONE` and `OBS_BOUNDS_SCALE_INNER`. **Outline/aspect
  mismatch investigation outcome: no bug found.** The user-reported "resize
  outline stays sized for the pre-rotation aspect" was investigated at both the
  native geometry layer (`update_item_transform` in `libobs/obs-scene.c` — no
  divergence between bounds and non-bounds branches) and the Svelte/TS UI layer
  (no cached width/height/rotation state exists client-side; `DrawSelection`
  redraws from a live `obs_sceneitem_get_box_transform` read every frame) —
  both are provably correct across every code path a rotation can take. Live
  GUI visual confirmation was attempted but aborted for safety (the app loads
  the user's real, connected production scene collection with live
  destinations; no safe isolated repro window was available) — **if the outline
  lag still reproduces in practice, it needs precise repro steps (source type,
  whether Fit-to-screen was used first, which canvas) and ideally a screen
  recording**, since nothing in the codebase explains it. Resize-drag snapping
  has no bridge-callable entry point for a headless self-test — **GUI
  acceptance owed** (drag-resize near an edge/center and confirm it snaps;
  confirm rotate no longer flings items unreachable; confirm an
  extreme-position transform recovers visibly on-canvas).
- ⏸ **GoLive / Multitrack Video** — currently dormant. It's Twitch Enhanced
  Broadcasting (many quality renditions → one Twitch ingest), orthogonal to our
  many-platforms goal. Decision at the multi-destination phase: delete the
  Twitch-specific `GoLiveAPI`/config, or harvest `MultitrackVideoOutput`'s
  multi-output/encoder-sharing plumbing for our fan-out handler.
- ⏸ **WHIP simulcast encoder-id divergence** — conscious deferral from 4b.
- ✅ **Frontend source reorg** (`11fc0e97f`, `4a5ee2abe`) — flat source dirs grouped by
  domain, no behavior change. `frontend/web/src/lib` → api/stores/dialogs/settings/menus/
  ui/utils/canvas + `dock/`→`docking/`, with a `$lib` alias replacing deep relative
  imports; `frontend/src` → windowing/settings/scene/util, with `creds` folded into
  `oauth/`. Makes the "where does this belong" answer structural.
- 🔭 **Code-health deferrals (2026-07-10)** — surfaced during the centralization/
  dedup sweep; each deferred as a separate task to keep that sweep surgical:
  (a) **Scenes/Sources embedded-row fork** — `CanvasDock.svelte`'s inline mini
  scene/source lists (`es-*` classes) duplicate the standalone `ScenesDock`/
  `SourcesDock` (`dock-*` classes) but with divergent behavior (Sources adds
  dblclick-properties, Scenes a grid layout). Sharing needs a skinnable row
  component merging two class namespaces — visual-regression risk, not a
  surgical extraction. (b) **`MonitorPage.svelte` inline bitrate** diverges from
  `format.ts` `fmtBitrate` (drops the sub-1000 → `kb/s` branch, always `Mb/s`);
  routing it through the shared helper would change rendered sub-Mbps output —
  needs a display decision before unifying. (c) **`AudioMonitor.cpp` /
  `preview_window.cpp` by-uuid finds** not centralized onto entity accessors
  like the rest (compound `(windowId,uuid)` keys, mutex-guarded bodies, an
  iterator needed for `erase`) — a targeted follow-up, not the generic
  find-by-uuid pass. (d) **Stats per-output color model** (2026-07-10) — summary
  strip uses `grade` (green at 0), rows use `elevated` (neutral→yellow→red), and
  congestion severity band is `[30,60]%`; chosen so healthy rows stay calm and
  the errored overlay is the loudest signal — **GUI acceptance owed** to confirm
  the thresholds feel right in practice.
- ✅ **C1 — output-binding edits now honor Settings → Cancel.** Edits still commit
  live (previews react immediately), but the dialog snapshots the bindings on open,
  reverts on Cancel/Esc, and re-baselines on Apply/OK — mirroring the Stream tab.
  See `docs/issues.md` #3 C1.
- ✅ **Canvas-editor input clamping** — FPS num/den (`setRange(1, …)`), SDR white
  level (`setRange(80, 480)`), and the `WxH` parse (`cx > 0 && cy > 0`) already
  floor every numeric input above 0 through the dialog; no separate guard needed.
- ✅ **Branch history** — `canvas-foundation` (Phases 1–3) fast-forward-merged to
  `master` + pushed 2026-06-21; `frontend-rewrite` (Phase 4) fast-forward-merged to
  `master` 2026-06-25; both branches deleted (local + origin). **`ui-redesign` (Phase 6+
  — importer, Phase 7 UI redesign, Phase 8 OAuth, Phase 9 engagement, Phase 10 broker)
  is likewise fast-forward-merged to `master`, and the branch is deleted (local +
  origin).** Everything through Phase 10 is on `master` (tip `2599e8597`); **no merge is
  owed anywhere.** Remote branches are now exactly **`master` + `dev`**; ongoing work
  flows **`dev` → `master`**. `upstream/*` OBS branches untouched.
  > **Note on `ui-redesign` mentions elsewhere in this doc:** every remaining reference
  > (e.g. "shipped on `ui-redesign`", "commit on `origin/ui-redesign`") is **historical
  > provenance only** — it records where the work was authored. That branch no longer
  > exists; resolve any such commit on `master`. Where an entry still says GUI/live
  > **acceptance is owed, that is current and unrelated to the merge** — acceptance was
  > never a merge gate in the end.

---

## Phase 11 — Debug logging + deep app audit 🔭 PLANNED

**Motivation.** Debugging is currently near-blind: native `HostLog` writes to
`OutputDebugStringA` (invisible without an attached debugger) and CEF `console.*`
routes to the same sink, so neither reaches the session log file. Pinning the
preview-suspend double-count race (2026-07-11) took several build round-trips of
*temporary* `blog()` instrumentation plus hand-routing `console` → `blog`. A
permanent, gated debug channel would have made it a single trace read. That race
was also **latent in every modal** (each opener double-suspended on top of
`Modal.svelte`) — the archetype of a bug class that is invisible until you can
watch the app narrate itself.

**Part 1 — gated debug logging ✅ SHIPPED** (`82468650b` + WS2; per-category filtering
`fe4f302a6` 2026-07-15). Design spec: `docs/superpowers/specs/2026-07-12-debug-logging-design.md`.
Delivered: the `DBG()` macro, `LogCat` (16 categories, one X-macro table), the `HostLog`→`blog`
fold, `Client::OnConsoleMessage` `[L][cat]` parsing, env/.env/persisted boot seed,
`DiagnosticsSettings` + `diagnostics.*` bridge methods + Settings toggle, and the web logger
(`lib/utils/log.ts`). `BRAIDCAST_DEBUG` takes a category list; `1`/`true`/`on`/`all` enable all.
**Known trap for Part 2:** `diagnosticsStore.start()` seeds via an async `diagnostics.get`, so web
`log.dbg` calls before that resolves are silently dropped (its comment claims otherwise). Also:
tag-only greps can't prove gating — pre-existing always-on `HostLog` lines share the `[cat]`
convention and the session log stores no level prefix. Original spec below.
- A `DEBUG` flag, **default OFF**, overridable by env (e.g. `BRAIDCAST_DEBUG=1`),
  Django-style; ideally also a runtime toggle (Settings switch / hotkey) so a live
  session can flip it without a relaunch.
- When ON: route native `HostLog` **and** CEF `console.*` into the session log via
  `blog(LOG_INFO, …)` so everything lands in one readable file. Tag by **category**
  (`preview`, `gate`, `oauth`, `chat`, `events`, `bridge`, `engine`, `canvas`, …)
  with optional per-category filtering.
- **Zero-cost when OFF:** gate before building the message string (no formatting,
  no `blog`), so shipping with the calls in place costs nothing in the default path.
- Instrument the seams that bit us and will bite again: the preview gate ref-count
  (suspend/release balance), preview `SetRect`/`Hide`/display create-destroy, bridge
  method dispatch, OAuth/token lifecycle, chat/events transport start-stop, and the
  multistream engine start/stop.

**Part 2 — deep audit (with DEBUG on).** Drive **every window** (main, detached
canvas docks, projectors, settings) and **every feature** (go-live + edit-info,
chat/events, reorder/splitters, schedule, sources/filters/transform, virtual cam,
screenshots, importer, …), capture the traces, and hunt latent bugs the way the
suspend race surfaced. Deliverable: a windows × features coverage matrix with the
observed traces, and filed issues for every anomaly.

**Part 3 — config/data self-heal on launch.** Bundled data files and their
per-user caches drift silently, and OBS prefers the user cache
(`obs_module_config_path`) over the bundled copy (`obs_module_file`) — so a
stale cache, or a stale *bundled* copy, wins invisibly. Live example
(2026-07-11): the rtmp_common Service picker dropped **Kick** and
**YouTube - HLS** because the read-path `services.json`
(`bin/64bit/rtmp-services/services.json`, the obs2_update package copy) was an
old 84-service/5-common snapshot while source already had the 85-service/
7-common list. Diagnosis meant hand-tracing four candidate paths; recovery was a
manual file copy + a `.stale-vNNN.bak` rename. Build a startup reconciliation
that does this automatically:
- A registry of **managed canonical data files** (start: `rtmp-services/services.json`;
  generalize to any bundled data with a version/format stamp).
- On boot, validate each cache + bundled copy: parse-check, `format_version`
  match, and version/hash compare against the app's shipped canonical. If a cache
  is missing/older/unparseable/format-mismatched → evict it so the bundled copy
  wins; if the bundled read-path copy is older than the shipped canonical →
  refresh it. Rule of thumb for this fork (we don't run OBS's updater): **shipped
  canonical wins unless a cache is strictly newer AND valid.**
- Log every decision through the Part 1 DEBUG channel (`config` category), so the
  next drift is a single trace read, not a four-path hand-hunt.
- **Non-blocking:** run on a background thread at launch — the app opens
  immediately. The work is parse + version-compare + an occasional file copy (a
  few ms). Caveat: managed files are read lazily, not at splash
  (`services.json` loads when the profile editor builds the Service list), so the
  heal must either beat first read (it fires at boot; the user takes seconds to
  reach the editor) or invalidate + reload the affected view if it lands after.

**Part 4 — proactive credential health (dead-refresh-token detection).**
Access-token expiry is already handled well: `ensureFresh` refreshes in a skew
window before each authed call plus a reactive 401 retry, single-flight and
store-coherent — silent to the user. The gap is **refresh-token death** (password
change → revoke, 30-day inactivity, manual revoke). Today that path is invisible
until it bites:
- The connected gate is `isTokenScopeCurrent && !refresh.empty()`, and a revoked
  token is still a non-empty string, so the account shows **green until first
  real use** (go-live apply / metadata / chat connect), where `refresh()` fails
  with the raw broker error.
- `AccountNeedsReconnect` trips only on **scope version**, never on a refresh
  *rejection* — so a dead token doesn't even self-classify as needs-reconnect.
  `provider.hpp` documents "invalid_grant means the caller must force re-auth" but
  **no caller wires it up** — declared, unimplemented.

Note the hard limit: a revoked refresh token **cannot** be silently re-granted —
re-auth requires user consent in the browser. So the goal is *detect early +
prompt cleanly to relink*, not auto-fix. Work:
- Distinguish `invalid_grant` (dead credential, needs relink) from a transient
  network/5xx refresh failure (retry, don't relink) in the broker refresh path,
  and surface that classification up to the account status.
- Extend the needs-reconnect signal to include "refresh rejected", so the UI flips
  the account to the amber relink state instead of a raw mid-action error, and the
  go-live modal prompts relink before the user commits.
- Optionally, a lightweight **background health check** (piggybacking the Part 3
  launch thread and/or a pre-go-live probe) that validates each connected
  account's refresh token and pre-flags stale ones — so relink is prompted at a
  calm moment, not mid-stream. Non-blocking, same threading discipline as Part 3.

**Part 5 — stream-credential (ingest) self-heal + the Kick provisioning gap.**
The OAuth connect flow provisions streaming credentials per platform, and the gaps
leave an rtmp_common profile unstreamable — `obs_output_start` bails deep in libobs
(empty `last_error`) so the UI only ever showed the generic "the output failed to
start". Review 2026-07-12 (a live incident where Twitch + Kick failed while YouTube
streamed) mapped it:
- **Twitch** — `TwitchProvider::fetchStreamKey` supplies the key and the connect
  flow seeds `server="auto"` (bridge.cpp ~8130). But profiles connected *before*
  that line are **stale** (key present, `server` absent) and can't go live until a
  reconnect. rtmp_common resolves the Twitch ingest only when `server=="auto"`.
- **Kick** — `KickProvider::fetchStreamKey` reads the key from
  `GET /public/v1/channels` (`stream.key`, gated by `streamkey:read`). Kick's ingest
  is a **fixed global IVS endpoint** in `services.json` (not per-broadcaster), so
  `server="auto"` resolves it and only the key needs the API — same seed as Twitch.
- **Shipped alongside this review:** a pre-flight guard in
  `MultistreamEngine::StartOutput` that fails an rtmp_common binding with a precise,
  platform-named reason ("<platform> has no stream key / ingest server — reconnect")
  instead of the generic downstream error.

Self-heal work (background, launch + pre-go-live, same discipline as Parts 3/4):
- For a keyed rtmp_common profile linked to a **connected** account with a missing
  `server` or `key`, re-run the provider's credential fetch and write it back via
  `ingest_writeback` — heals the stale-Twitch case with no manual reconnect.
- Minimal seed: set `server="auto"` for any rtmp_common profile that has a key but
  no server.

Kick streamkey provisioning — **SHIPPED 2026-07-13**, pending a live confirm:
- Broker (`auth.braidcast.com`) added `streamkey:read` to the Kick scope list
  (`providers.ts`) and deployed; the Kick OAuth app has the scope approved.
- `KICK_SCOPE_VERSION` bumped 2 → 3 so existing Kick tokens re-consent via
  `AccountNeedsReconnect`.
- `KickProvider::fetchStreamKey` reads `stream.key` from `GET /public/v1/channels`;
  the existing connect flow writes the key + seeds `server="auto"`, which
  rtmp_common resolves to Kick's fixed IVS ingest (`services.json`).
- **Unconfirmed until a live probe:** whether Kick's public API actually returns
  `stream.key` in practice (docs define the scope + field, but the channels endpoint
  is documented to expose only `stream.url`). If withheld, the fallback stays a
  manual paste from kick.com/dashboard/settings/stream.

---

## Phase 12 — Studio Profiles + global destinations 🏗️ PLANNED (design-first, largest task)

**Motivation.** Output bindings (destinations) are the only canvas/profile-level
entity stored **per scene-collection**, and that coupling is arbitrary: a binding
pairs a **canvas** (global) with a **stream profile** (global) — no scene is
involved, unlike scene links (scene → canvas), which are legitimately
per-collection. The visible cost (2026-07-11): switching to or importing a
scene-collection resets destinations for every canvas and empties the canvas bar,
because canvas-chip/preview visibility is gated on the active collection's
bindings (`AnyEnabledForCanvas`, `bridge.cpp:4883`; `StudioPage.svelte:540`).
No data is lost — canvases are global (`canvases.json`), and each collection's
bindings are saved to its own sibling file before a switch — but the model
surprises the user and makes a valid collection look broken.

Stock OBS separated two orthogonal switchers: **Scene Collections** (content —
scenes/sources) and **Profiles** (config — output/encoding/service, and in OBS
also video/audio/hotkeys). Braidcast dropped the Profile concept. This phase
brings back a Braidcast-native equivalent and fixes the coupling in one coherent
model.

**Part A — decouple destinations from collections (routing is global).**
- Move output bindings from per-collection sibling files
  (`scenes/<slug>.output_bindings.json`) to a **global** store, like `streams.json`
  and `canvases.json`. Routing (canvas → destination) becomes collection-independent.
- Keep **scene links** (scene → canvas) per-collection — those genuinely involve
  scenes. Clean split: **routing global, content per-collection**. Switching or
  importing a collection then changes only *what each canvas shows*, never *where
  it streams* or *whether canvases exist*.
- One-time **migration** of existing per-collection bindings (consolidate; resolve
  the case where the same canvas is bound differently across collections). Drops
  per-show routing — a speculative capability that appears unused.

**Part B — Studio Profiles (a switchable setup bundle).**
- A **Profile** is a named, switchable bundle that carries **destinations**
  (the now-global output bindings), the **canvas set**, and the **studio layout**
  (dock positions/sizes/visibility, splitter ratios) — independent of scene
  collections, exactly as OBS's Profile is independent of its Scene Collection.
- **Terminology hazard — disambiguate first:** "stream profile" already means a
  *destination credential* (platform + key, `streams.json`). The OBS-style Profile
  here is a *setup/config bundle*. Pick a distinct name (e.g. **Studio Profile** /
  **Setup**) before writing any code, or the two will be conflated everywhere.
- **Open design questions (resolve in a design pass before implementing):**
  - Canvas scope — canvases are global today; do they become **profile-scoped**
    (each profile its own canvas set) or stay global with the profile only owning
    the *bindings*? This is the crux and shapes everything else.
  - How Profiles compose with Scene Collections — two orthogonal switchers (OBS
    model), or one merged concept? What does switching each do?
  - Exact contents of a Profile — bindings + canvases + dock layout at minimum;
    possibly video/audio/output settings too.
  - Dock-layout persistence — where the dockview layout lives today and how to
    scope it per profile so each remembers its window arrangement.

**Sequencing.** This is the **largest** item on the roadmap and is **design-first**:
brainstorm + a written spec (data model, migration, terminology, the canvas-scope
decision) before any implementation. Part A (global destinations) can ship ahead
of Part B and already removes the empty-canvas-bar surprise; Part B is the full
Profiles system. A small **interim** option exists if the empty canvas bar needs
relief before this lands: stop gating the canvas bar on bindings (show canvases
regardless) — not started, separable from the phase.
