# Session summary — Phase 2 multistream, autonomous run (2026-06-15)

Unattended run finishing Phase 2 of the native-multistream work on branch
`canvas-foundation`. Order executed: 2c (verify) → 2d → 2e → 2b → holistic
review (5.5) → this summary → shutdown. All work committed locally; not pushed.

## What shipped, by phase

### 2c — Streams tab (GUI-verified this run; code pre-existing)
Commits `117cad93e`, `5f02c03b5`, `738a9e4a9`, `16787ed86`, `9b1dc2b87`,
`dbf1752f5`. Global reusable stream profiles in `streams.json`; master-detail
Streams settings page; primary-only mirror into OBS's single `obs_service`.
GUI-verified: per-profile isolation, persistence across restart, primary mirror
to `service.json`, and edit-then-Cancel discards (the `dbf1752f5` Cancel-button
fix was found and fixed during this run's GUI test).

### 2d — Outputs tab (implemented + GUI-verified)
Commits `a686e134a` (OutputBinding type), `26b9768bb` (per-scene-collection
`output_bindings` persistence), `e4f97bfe4` (Settings → Outputs routing page),
`2689e149e` (nav-refresh fix found in GUI test). An output = (stream profile ×
canvas) + `enabled`, grouped by canvas with a searchable profile dropdown and a
single-key in-use guard. GUI-verified: add/toggle, in-use block, persist across
restart in the collection JSON.

### 2e — Multistream engine + dock (implemented + verified to the failure path)
Commits `108a7bd4e` (engine), `0be82b481` (OBSBasic ownership), `b0fc93ddd`
(dock), `76355c446` (mutex fix), `51fbdd28c` (Error-state surfacing), `21ca6f993`
(verification notes). Encode-once-per-canvas fan-out; one `obs_output` per
enabled binding driven by its profile's `obs_service`; handler-level single-key
guard; Multistream dock with per-canvas groups, status dots, Go Live / Stop All.
Review caught a CRITICAL data race (off-thread start/stop handlers vs the Qt
thread mutating the `live` vector) — fixed with `liveMutex` (`76355c446`).
GUI-verified: real fan-out (log-confirmed encoders + RTMP connect), dummy-key
connection failure → **Error** state, single-key guard refusal, Stop All, no
crash through start/fail/stop cycles. **Owed:** a real sustained broadcast
(needs live platform creds) — see `docs/issues.md` #2.

### 2b — Dockable canvas panels (implemented + GUI-verified)
Commit `26334c592`. One dock per additional canvas: a canvas-scoped scene list +
an `OBSQTDisplay` of that canvas's mix, splitter auto-orienting from dock shape,
layout persisted by canvas uuid. Review caught a CRITICAL teardown UAF (signals
not disconnected before destruction) and an Important borrowed-pointer-across-
modal bug — both fixed before commit. GUI-verified: the dock appears for an added
canvas, scene-add is scoped to that canvas (default untouched), the added scene
and dock position survive restart, and shutdown is clean (0 memory leaks).

### 5.5 — Holistic canvas/multistream review
Commits `e39a3b7ac` (fix), `8de5a9860` (findings log). Subsystem-wide review
(data model, managers, engine, docks, settings, persistence). Refcounting,
save/load symmetry, and thread/teardown ordering came back correct. One real
latent bug fixed: the per-canvas encoder cache was never invalidated, so a
canvas resolution/encoder edit (which frees the old video mix) left the next Go
Live reusing an encoder bound to freed memory — fixed with
`InvalidateCanvasEncoders` wired into the canvas edit-apply paths and
`RemoveCanvas` (`e39a3b7ac`). Remaining findings logged to `docs/issues.md` #3
(notably H1: editing a canvas while a multistream output is live on it is an
unguarded crash hazard; C1: Outputs-tab edits ignore Settings Cancel — a design
decision left for the user).

## GUI-verified vs owed
- **Verified this run:** 2c, 2d, 2e (failure path), 2b — all via the portable
  build + windows-mcp, in the isolated `build_x64\rundir` sandbox only.
- **Owed (documented, not blockers):** a real live broadcast for 2e
  (`issues.md` #2); the H1/C1 decisions from the review (`issues.md` #3).

## Notes
- All builds via `mage build`, RelWithDebInfo, clean link each time.
- Tested ONLY in portable mode; the user's real OBS profiles were never touched.
- Branch `canvas-foundation`, local only — **not pushed**.
- Specs/plans under `docs/superpowers/` and `.superpowers/` are gitignored.
