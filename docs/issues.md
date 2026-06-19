# Known Issues & Investigations

A running log of non-trivial issues hit during development, what was ruled
out, and how they were resolved. Newest first.

---

## #4 — Additional canvas previews are view-only (no drag layout editing)

**Status:** OPEN (design decision / future work) — surfaced 2026-06-16 from an
edge-case question: "what if a user mainly uses an additional canvas, with the
Default canvas purely for inheritance?"

**Finding.** `CanvasDock`'s preview is a plain `OBSQTDisplay` — it renders the
canvas mix but has **no** mouse/transform interaction (verified: no
`mousePressEvent` / `obs_sceneitem_set_pos` / selection handlers in
`CanvasDock.cpp`). Only the central `OBSBasicPreview` (a subclass) implements
source select / drag / resize / transform. Consequence: a scene owned by an
additional canvas can be **populated** (the dock's Add Source → `obs_scene_add`)
but its sources **cannot be visually positioned by dragging** — there is no
in-dock layout editing. Numeric Edit-Transform isn't wired into the dock either.

**Why it matters.** Model 1 gives each additional canvas independent scenes with
independent per-item layout. The intended north-star (landscape + vertical
simultaneously, per-platform layouts) *requires* laying out the vertical canvas.
For a user whose primary surface is an additional canvas, that surface is
currently un-editable except by full-canvas placement.

**The inheritance half already works.** Per-field audio/video/resolution/color
inheritance from the Default canvas (Phase 1 4a `ToVideoInfo`) is implemented, so
"Default purely for inheritance, different resolution per canvas" is supported at
the encode level — it's only the *visual editing* of the dependent canvas that's
missing.

**Recommended fix (not the shelved step 3).** Make canvas-dock previews
**editable** — give each dock an `OBSBasicPreview`-class surface bound to its own
canvas, so the Default stays central/editable and every additional canvas is
fully editable in its own (floating) dock. This is preferable to the rejected
"first-enabled-wins-center" reparenting (a primary-canvas state machine + runtime
reparenting of the main preview — invasive, marginal gain). **Needs a spike:**
`OBSBasicPreview` assumes the main program canvas in several places
(`obs_get_video()`, scene-item resolution), so binding it to an arbitrary canvas
is non-trivial and must be scoped before committing.

---

## #3 — Canvas/multistream holistic review: deferred findings

**Status:** OPEN (logged for decision/follow-up) — from the Phase-2 wrap-up review.
**Date:** 2026-06-15

A full review of the canvas + multistream subsystem (data model, managers,
engine, docks, settings tabs, persistence). Refcounting, save/load symmetry, and
the thread/teardown ordering all came back correct. One real latent bug was
fixed in commit `e39a3b7ac` (the never-invalidated per-canvas encoder cache).
The items below were deliberately **not** auto-fixed and are recorded here.

- **H1 (potential crash) — RESOLVED.** Editing an additional canvas's resolution
  while a multistream output was live on it called `obs_canvas_reset_video`
  unguarded, freeing the video mix under a live encoder (UAF). Added
  `MultistreamOutput::IsCanvasLive(canvasUuid)` and guarded **both**
  `obs_canvas_reset_video` sites on additional canvases in `ApplyCanvasEdit`:
  (1) the non-default branch now refuses the edit when the canvas is live —
  mirroring the Default branch's `obs_video_active()` guard: it reverts the
  resolution/FPS to the live values and shows `…Editor.ActiveResolution`;
  (2) the Default branch's live-follow loop skips any follower canvas that's live
  (leaving it at its current resolution) and shows `…Editor.ActiveFollower` once.
  Chose **refuse** over auto-stop for consistency with the existing Default-branch
  behavior (don't tear down a running broadcast from a settings edit). Site 2 was
  reachable because a follower's output uses the canvas mix, not the main video,
  so `obs_video_active()` can be false while the follower is live. The refusal is
  scoped to resolution/FPS: the warning + revert (and the follower warning) fire
  only when the resolution/FPS actually changed, so name/encoder/color edits still
  apply while live — only the video-mix reset is withheld. (The Default branch had
  the same over-broad warning before; it's now gated the same way.)

- **C1 (design decision) — output-binding edits ignore Settings → Cancel.** Every
  mutation in the Outputs tab calls `SaveProject()` immediately, so editing
  bindings then pressing Cancel does not revert (unlike the Stream tab, which
  deliberately discards on Cancel — commit `dbf1752f5`). Canvas add/remove is
  also immediate-commit but is justified as a management action. Decide
  intentionally: document bindings as immediate-commit, or route them through the
  dialog's apply/cancel path.

- **C2 (minor UX) — RESOLVED.** A binding whose `profileUuid` is non-empty but
  resolves to no profile now reads as a distinct "profile deleted" state, separate
  from an unset "—": the Multistream dock row shows the labeled text with an
  explanatory tooltip, and the Outputs tab shows an `outputMissingBadge` next to
  the (blank) profile combo. The stale uuid still persists until the user re-picks
  or removes the output — surfacing it is the fix; auto-clearing it is not, since
  the user may want to recreate the named profile.

- **P2 (perf) — RESOLVED.** Split `MultistreamDock` refresh into structural
  (`Refresh`, full rebuild) and status-only (`UpdateStatuses`, updates the dot /
  text / toggles / cascade in place). The engine's `onStatusChanged` tick — which
  never alters the binding set — now calls `UpdateStatuses`; the binding set is
  rebuilt via a new `OutputBindingsChanged -> Refresh` connection. That connection
  also closes a latent staleness bug: editing bindings in Settings while the dock
  was visible previously left it stale until the next status tick or re-show.

- **R1 (cleanup) — RESOLVED.** Added `OBSBasic::ForEachStreamableCanvas(cb)`
  (Default canvas first, then each non-ephemeral additional canvas; yields
  uuid/name/width/height) and routed the two true-duplicate sites through it
  (MultistreamDock, Outputs tab). The SceneCollections and Stream-tab loops were
  *not* folded in — their intent has drifted (Stream tab prepends a "None" entry
  and excludes Default; SceneCollections excludes Default by design), so merging
  them would obscure that difference.

---

## #2 — Multistream engine (2e): verified failure path, live-broadcast path unverified

**Status:** OPEN (verification gap, not a defect) — engine and dock verified end-to-end
short of a real sustained broadcast.
**Date:** 2026-06-15
**Affected build:** `32.1.0-37` and later (canvas-foundation), commits `108a7bd4e`,
`0be82b481`, `b0fc93ddd`, `76355c446`, `51fbdd28c`.

### What was verified (portable sandbox, windows-mcp GUI)

- Multistream dock registers, groups bindings by canvas, shows per-row profile +
  status dot + live toggle, a per-group cascade toggle, and the Go Live / Stop All
  footer.
- "Go Live" / a row toggle drives the real encode-once fan-out: per-canvas x264
  video + AAC audio encoders are created, an RTMP service + output are created and
  started, and a connection to the destination ingest is attempted (OBS log
  confirms `Connecting to RTMP URL …` then the per-binding output name).
- With a deliberately invalid stream key the connection fails (`Connection … failed:
  -3`) and the row now shows **Error** (red dot + tooltip), via the
  `OnOutputStop` non-`OBS_OUTPUT_SUCCESS` path (commit `51fbdd28c`).
- Single-key guard: a second binding sharing one stream profile is refused while the
  first occupies it; the toggle bounces back to Idle (log: "profile already live").
- Stop All clears every live/errored output back to Idle.
- No crash through repeated start / async-fail / stop cycles — the `liveMutex`
  (commit `76355c446`) guards the off-thread start/stop signal handlers against the
  Qt thread's mutation of the `live` vector.

### The gap

A **successful, sustained broadcast** to a real destination was **not** verified —
that requires valid platform stream keys and actually pushing video to a live
service, which is out of scope for the isolated portable test (the constraint is to
never touch real accounts/credentials). The Connecting → **Live** transition
(`OnOutputStart`) is wired and the single-stream legacy path already exercises the
same libobs output mechanics, so this is a confidence gap on the last hop only, not
a known break.

### How to close it

Configure one real stream key on a throwaway account, Go Live for a single binding,
and confirm the row reaches **Live** (green) and the platform shows the feed; then
Stop All and confirm a clean return to Idle.

---

## #1 — Broken / mispositioned preview (native display) on a high-DPI display

**Status:** RESOLVED — environmental (Windows per-application DPI override), not a fork defect.
**Date investigated:** 2026-06-14
**Affected build at time of report:** `32.1.0-20-g734e6a4c4` and later (canvas-foundation branch)

### Symptom

On launch, the main program preview rendered incorrectly:

- The GPU-rendered preview (a native child window / `OBSQTDisplay`) was
  **mispositioned** — painted at the top-left while its Qt container/toolbar
  ("Scale to Window", "No source selected") sat far below it.
- Large **unpainted white/black regions** where the preview should be.
- A **stale framebuffer**: the preview area showed leftover snapshots of the
  desktop / OBS's own UI (e.g. a ghosted "File Edit View Docks…" menu) instead
  of live content.
- The native display **did not track window resizes** (stayed pinned through
  maximize / resize), and toggling Studio Mode produced equally broken
  Preview/Program displays.

The preview content itself was correct (an empty scene renders black); the
problem was purely the **native window geometry / presentation**.

### Environment

- GPU: NVIDIA GeForce RTX 5070 Ti (50-series), D3D11 backend (initialised fine
  per logs: `video settings reset: 1920x1080 @ 60`, `D3D11 loaded successfully`).
- Display: 4K (3840×2160) at **150% scale (Windows "Recommended")**, dual-monitor
  with mixed DPI between screens.
- Qt: bundled `Qt6 6.11.1` (from obs-deps `2026-06-02`); official OBS ships
  `Qt6 6.8.3`.

### What was ruled OUT (and how)

1. **Our canvas/scene code (Phase 2a) — NO.**
   A git bisect *appeared* to implicate the 2a commits, but the bisect was
   **confounded**: every "broken" run used `mage run` (a fresh portable config)
   and every "fine" run was a direct launch (a populated `%APPDATA%` config).
   The commit was never the variable.

2. **Portable mode (`--portable`) — NO.**
   Copying a populated config into the portable rundir and launching
   `--portable` rendered **fine**. Portable mode is innocent; it was simply
   always-fresh.

3. **The Qt 6.11 upgrade — NO.**
   `Qt6Core.dll` was dated 2026-06-12, i.e. Qt 6.11 was already in the build
   during a known-good livestream the day before. Decisively: **official OBS
   (Qt 6.8.3) reproduces the identical breakage** on a fresh config on this
   machine. Different build, different Qt, same bug → not Qt.

4. **Fresh vs. populated config content — INDIRECT, not causal.**
   A populated config worked and a fresh config broke, but this correlation was
   a side-effect of the real cause (below). The breakage did **not** self-heal
   after a clean `File → Exit` (which saves geometry) and reopen.

5. **W1 config separation — not the cause, only the exposure.**
   Pre-W1 a direct launch loaded the user's populated `%APPDATA%\obs-studio`
   (worked); post-W1 it loads a fresh `%APPDATA%\obs-multistream`. W1 only
   changed *which* config was loaded, surfacing the underlying issue more often.

### Root cause

A **Windows per-application "Override high DPI scaling behavior" compatibility
flag** had been set on `obs64.exe` (Properties → Compatibility → High DPI
settings → **System (Enhanced)**). The user applies this to apps that render
oddly at 4K@150%; it had been set on both this fork's `obs64.exe` and the
official OBS `obs64.exe` "some time back".

OBS already declares **Per-Monitor-V2 DPI awareness** and positions its native
display child windows accordingly. Forcing **System (Enhanced)** on top of that
overrides OBS's own DPI handling, so Windows bitmap-scales / mis-maps the native
display window's coordinates → the mispositioned, stale, non-tracking preview.

This is a per-exe **registry** setting
(`HKCU\…\AppCompatFlags\Layers`, keyed by exe path), which is why it **persisted
across rebuilds** and produced confusing, seemingly code-correlated results.

### Resolution

Uncheck **"Override high DPI scaling behavior"** on `obs64.exe`
(Properties → Compatibility → Change high DPI settings). With OBS left to its
own Per-Monitor-V2 DPI awareness, the preview renders correctly. Confirmed fix.

### Implications / fork actions

- **No code change required.** Nothing in the fork (canvas model, output
  rewire, rebrand, portable handling) causes or worsens this.
- It affects **official OBS identically** — it is an OBS/Qt + Windows-DPI
  interaction triggered only by the manual per-exe DPI override.
- **Support note for users:** if the preview is mispositioned / shows stale
  content on a high-DPI display, ensure `obs64.exe` does **not** have a manual
  "Override high DPI scaling behavior" compatibility flag set. OBS manages DPI
  itself; overriding it breaks the native preview.
- No reason to pin Qt back to 6.8.x for this issue (6.8 reproduces it too).
  A Qt-version pin for upstream *parity* remains a separate, optional decision.
