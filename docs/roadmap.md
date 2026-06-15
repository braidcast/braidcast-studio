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

---

## Backlog & deferred decisions ⏸

- ⏸ **GoLive / Multitrack Video** — currently dormant. It's Twitch Enhanced
  Broadcasting (many quality renditions → one Twitch ingest), orthogonal to our
  many-platforms goal. Decision at the multi-destination phase: delete the
  Twitch-specific `GoLiveAPI`/config, or harvest `MultitrackVideoOutput`'s
  multi-output/encoder-sharing plumbing for our fan-out handler.
- ⏸ **WHIP simulcast encoder-id divergence** — conscious deferral from 4b.
- ⏸ **Canvas-editor input clamping** — guard FPS and SDR-white-level against 0.
- 🔭 **Finish the `canvas-foundation` branch** — merge/PR/cleanup once the open
  items above are resolved.
