# OBS MultiStreamer — Roadmap

Native multistreaming built into this OBS Studio fork via **canvas / output
multiplexing**: canvases (resolution + fps + encoder settings) become the unit
of streaming, each wired to one or more destinations. Built on OBS's existing
`obs_canvas_t` + output primitives rather than a rewrite, to keep the capture
and plugin ecosystem.

This is a **private fork**, not submitted upstream. The app is rebranded
"OBS MultiStreamer."

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

- 🐛 **"Use Default resolution" toggle stretched full-width** in the canvas
  editor — the idian `ToggleSwitch` fills its layout cell and reads like a
  slider, though it behaves as a toggle. Fix deferred pending the Phase 2
  editor/dock decision (the control may relocate).
- ⏸ **Pre-existing `verticalLayout_35` AUTOUIC name-collision warning** —
  cosmetic; uic auto-renames. Not introduced by this work.

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
- 💭 **3c — Default-preview / canvas-in-center layout (decision deferred).** Two
  related questions about the QMainWindow **central widget** (which dock widgets
  can't occupy):
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
- 🔭 **3d (original scope).** The canvas dock's
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
- 🔭 **3e — Stats dock: multi-stream monitoring (needs design).** OBS's stock Stats
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
filters + scene transitions are done. The big remaining gaps are **studio mode**
and **the stats dock** — see "Remaining work" at the end of this phase.

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
  volume/mute persistence (only the device persists today); multiple named scene
  collections (single collection for now).
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
- 🔭 **Stats dock (3e).** Multistream per-output monitoring (bitrate / dropped frames
  / reconnects / CPU-GPU, grouped by canvas) reading `MultistreamEngine`'s per-output
  handles. Menu item disabled. Needs a design pass.

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
- ⏸ **Undo / Redo — DEFERRED (decided 2026-06-24).** No undo infrastructure exists;
  it's a from-scratch cross-cutting subsystem (every mutation needs paired undo/redo
  actions + an action stack), not a quick parity fix. Tackle as its own planned effort
  later (after Studio Mode / Stats). Edit→Undo/Redo stay disabled until then.
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
- 🔭 **Multiview** — a grid render of all scenes in one window. Larger: needs a custom
  multi-scene grid render (not a single `obs_display` of one target like projectors).
  Own scoping pass before building.
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

- 🔭 **`frontend_old` retirement** — the new build still references `frontend_old/api`
  + the utility model `.cpp`s (`CanvasDefinition`/`OBSCanvas`/`CanvasSceneLink`/
  `OutputBinding`/`StreamProfile`, see `frontend/CMakeLists.txt`) + the cpack license.
  Relocate those into `frontend/`, then delete the old Qt tree.
- 🔭 **Cross-platform preview** — Windows-only today (HWND + D3D11). macOS
  (NSView + IOSurface) and Linux (X11/GL) embedding are later efforts.
- 🔧 **Live broadcast test** (owed since Phase 2) + **GUI acceptance** of the recent
  menu / theme / preview / audio work (none driveable headlessly).
- ⏸ Minor: `SettingsModal.load()` routes device-fetch errors to the `videoError`
  label (cosmetic).

---

## Phase 5 — OBS MCP: AI control surface 🔭 PLANNED (design first)

Expose the app's OBS control to AI agents over the **Model Context Protocol (MCP)**,
so an assistant can drive the production — switch scenes, add/configure sources, edit
transforms, manage canvases / stream profiles / output bindings, start/stop the
multistream, and read live state — through a typed, discoverable tool surface.

**Why this is cheap to build here:** the CEF bridge is already a **data-driven method
registry** (~77 operations in `g_methods`: `scenes.*`, `sources.*`, `properties.*`,
`filters.*`, `transitions.*`, `sceneItems.*` incl. transform, `audio.*`, `canvas.*`,
`streams.*`, `outputBindings.*`, `multistream.*`, `dialog.*`, `window.*`). MCP tools map
almost 1:1 onto these. The work is a transport + schema-generation layer over the
existing registry, **not** a reimplementation of OBS control — the same envelope
(`{method, params} -> result`) that `window.obs.call` uses is what an MCP tool would
invoke. Events (`EmitEvent`) can surface as MCP notifications/resources for live state.

**Open design questions (resolve before building):**

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

## Backlog & deferred decisions ⏸

- ⏸ **GoLive / Multitrack Video** — currently dormant. It's Twitch Enhanced
  Broadcasting (many quality renditions → one Twitch ingest), orthogonal to our
  many-platforms goal. Decision at the multi-destination phase: delete the
  Twitch-specific `GoLiveAPI`/config, or harvest `MultitrackVideoOutput`'s
  multi-output/encoder-sharing plumbing for our fan-out handler.
- ⏸ **WHIP simulcast encoder-id divergence** — conscious deferral from 4b.
- ✅ **C1 — output-binding edits now honor Settings → Cancel.** Edits still commit
  live (previews react immediately), but the dialog snapshots the bindings on open,
  reverts on Cancel/Esc, and re-baselines on Apply/OK — mirroring the Stream tab.
  See `docs/issues.md` #3 C1.
- ✅ **Canvas-editor input clamping** — FPS num/den (`setRange(1, …)`), SDR white
  level (`setRange(80, 480)`), and the `WxH` parse (`cx > 0 && cy > 0`) already
  floor every numeric input above 0 through the dialog; no separate guard needed.
- ✅ **`canvas-foundation` branch** — Phases 1–3 merged to `master` (fast-forward) +
  pushed to origin 2026-06-21. Phase 4 (frontend rewrite) continues on the
  `frontend-rewrite` branch (off `master`).
