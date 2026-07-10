# Known Issues & Investigations

A running log of non-trivial issues hit during development, what was ruled
out, and how they were resolved. Newest first.

---

## #6 — Full code-review findings (2026-07-10)

**Status:** MOSTLY RESOLVED (2026-07-10) — a 5-agent read-only review of the fork-specific
code (multistream engine, OAuth/token, chat/events threading, bridge surface, Svelte UI).
Each finding was verified against source; confidence flagged where it is inference. Ranked
by severity.

**Fixed 2026-07-10** (commits on `ui-redesign`; all build-green, **live/GUI acceptance owed**
for the streaming/auth/WS-timing paths that can't be verified headless): both HIGH bugs
(stale-encoder UAF, binding-CRUD live reconcile), and every MEDIUM — WsClient recv
starvation, YouTubeChat `runMutex_`, EventHub atomic start, async-dispatch spawn barrier,
`ensureFresh(force)` peer short-circuit, browser-dock restore position, Modal focus trap —
plus the GAPS: `ProfileEnabledElsewhere` wired, `StartOutput` failure reasons surfaced +
failed output no longer counts live, the missing `settings.audioChanged` / `streamMeta.changed`
subscriptions, `streamMeta.get/set` refresh-present gate + refuse persisting a refresh-less
record (deliberate no-scopeVer carve-out preserved), the four stores' in-flight refresh guard,
`oauthStore.whenReady`, and the `channelsStore` audience/viewer prune.

**Low / enhancement resolved 2026-07-10** (commits `frontend: remove dead obs.event/interact
forwards`, `frontend: dispose onDidLayoutChange with drop handlers`): `flightLocks_` prune on
disconnect (`ForgetAccount`); removed the consumer-less `obs.event` per-frontend-event forward
and the dead `interact.changed` emit (plus the now-purposeless `FireSceneChanged` re-fire that
only existed to prove that chain); `projector.changed` typed `{ opened?, closed? }`;
`audio.setAdvanced` `tracks` now all-or-nothing (rejects a short array instead of zeroing
trailing mixers); `onDidLayoutChange` disposer pushed to `dropDisposers`.

**Verified non-issues:** the per-canvas dot `updateParameters` write-amplification — traced
dockview-core 6.6.1: `updateParameters` never fires `onDidLayoutChange`, so no `layout.json`
churn (nothing to fix). `settings.videoChanged` / `window.opened` emitted with no consumer —
left as-is (rare, cheap; kept for potential surfaces).

**Deliberately not done (net-negative / low-ROI):** provider raw-body → UI/log disclosure —
provider error bodies are non-secret (verified: not tokens) and useful for debugging; redacting
to generic messages hurts UX for ~zero security gain, so kept. DPAPI secondary entropy — a
fixed app-embedded entropy is extractable from the binary (theater); real value needs
per-install entropy under separate ACLs, and a same-user attacker defeats DPAPI regardless
(matches OBS norms). Revisit only if a concrete threat model justifies the per-install design.

Original finding detail retained below.

### High — real bugs, action first
- **Stale-encoder UAF after a video reset.** `settings.setVideo` and `settings.restore`
  reset the global video pipeline (`obs_reset_video` via `ApplyGlobalVideo`,
  `bridge.cpp:480/550/7157`) but never call `InvalidateCanvasEncoders(Default)`. The
  engine keeps the Default canvas's cached video encoder across `StopAll` and never
  re-binds it on reuse (`MultistreamEngine.cpp:117-121,364-365`), so the next
  `streaming.start` after a resolution change reuses an encoder bound to the freed
  `video_t` → UAF/crash. Every canvas-runtime reset path (`CanvasService::Update`)
  already invalidates; these two bridge paths are the omission. Fix: invalidate the
  Default canvas encoders inside `ApplyGlobalVideo` (after every global `obs_reset_video`).
- **Binding CRUD doesn't reconcile a live output.** `outputBinding.remove/update/setEnabled`
  (`bridge.cpp:5599/5652/5680`) mutate/delete the persisted binding with no `IsLive`
  check, unlike canvas CRUD (`MethodCanvasRemove` refuses while live). Remove-while-live
  → orphaned stream still hitting RTMP but gone from the UI + `IsCanvasLive` stuck true
  (blocks canvas edits) + leaked refs until shutdown. Update changing canvas/profile →
  streams the old pair while config claims the new. Disable → canvas stops rendering but
  the output keeps encoding a frozen frame. Fix: refuse, or stop/restart the live output,
  in each handler.

### Medium
- **`WsClient::recv` waits on `select()` before `curl_ws_recv`** (`ws_client.cpp:140-153`,
  PLAUSIBLE). libcurl buffers decoded frames above the socket; when two frames share a TLS
  segment the second isn't read until fresh socket activity, so on quiet channels a
  chat/EventSub/Pusher message is delayed until the next keepalive. Affects all four WS
  transports. Fix: call `curl_ws_recv` first, `select()` only on `CURLE_AGAIN`. (Related
  low: the `sock_ < 0` path calls recv with no timeout → cancellation can hang.)
- **YouTubeChat missing `runMutex_`** (`youtube_chat.cpp`). The only transport without
  generation-serialization; an overlapping re-Start can invert the `ActiveGuard` ordering
  and leave provider `LiveChatActive` stuck false → youtube_events REST-polls superchats
  the whole stream (wasted quota). Fix: add the `runMutex_` its three siblings use.
- **Async-lane throw escapes the exception barrier** (`bridge.cpp:9319`, `async_task.cpp:46-48`).
  `DispatchAsync` runs before `OnQuery`'s try; a `std::thread` ctor throw under
  thread/FD exhaustion → process terminate, plus a leaked worker count (5s shutdown block)
  and a hung JS promise. Fix: wrap the async spawn in the same try/Failure shape; decrement
  the count if the spawn throws.
- **`ensureFresh(force)` never short-circuits on a peer's fresh token** (`broker_strategy.cpp:280`).
  Under two concurrent 401s the forced refresh re-refreshes even after the store re-read
  shows a peer just rotated; the redundant rotation can 401 the other caller's in-flight
  retry → spurious "re-authentication required" + an extra Kick rotation. Fix: after the
  re-read, skip the refresh if `access` changed since the caller's 401.
- **Restored browser docks snap to a default position on restart**
  (`browserDockReconciler.ts:71`, `StudioPage.svelte:165`). The reconciler runs against an
  empty store before `load()` resolves, deleting the panels `layoutStore.restore` just
  re-created, then re-adds them right-of-preview. Canvas docks are safe (they await
  `canvas.list`). Fix: only reconcile inside `load().then(...)`.
- **`Modal.svelte` has no focus trap / initial focus / focus-return** (a11y). Every modal
  (GoLive, OAuthConnect, Filter, Transform, …) lets keyboard/AT users Tab into the obscured
  page behind the dialog. Fix centrally in Modal: move focus in on open, trap Tab, restore
  on close.

### Gaps — missing behavior / stale-UI / dead events
- **`ProfileEnabledElsewhere` is defined but unwired** (`OutputBinding.cpp:67`). The same
  profile enabled on two canvases is allowed; at start the engine silently refuses the
  second (`blog` only) → it shows Idle with no reason. Fix: call it in setEnabled/create/update.
- **`StartOutput` failure reasons never reach the UI** (`MultistreamEngine.cpp:207-222`).
  Bare `false` for every distinct cause; the row's status has no `lastError`. Fix: return/emit
  a reason into the per-output status.
- **Emitted events with no subscriber → UI goes stale on external (MCP/2nd-window) mutation:**
  `streamMeta.changed`, `settings.audioChanged`, `settings.videoChanged`, `window.opened`
  are fired by C++ but no `obs.on` consumes them (`bridge.ts` documents a refresh that never
  happens). `interact.changed` and `obs.event` are fully dead — and `obs.event` fires an
  `ExecuteJavaScript` round-trip on *every* OBS frontend event to zero listeners (pure
  overhead). Fix: subscribe in the stale surfaces (AudioTab/video tab, stream-info UI), or
  drop the dead emits + gate `obs.event`.
- **`streamMeta.get/set` skip `IsAccountConnected`** (`bridge.cpp:8196/8290`) and the connect
  flow can persist a refresh-less record (`broker_strategy.cpp:204`, `bridge.cpp:7959`). Not a
  cross-account authz break (same-user desktop), a gate-consistency gap. Fix: route through
  the connection gate; reject the `Put` when `acct.refresh` is empty.

### Low / enhancements
- Provider raw response bodies echoed into UI error strings + `HostLog` (API-internals
  disclosure, not tokens — verified) — map to body-free messages, log raw at debug.
- DPAPI blob uses no secondary entropy (`account_store.cpp:72`) — matches OBS norms; add
  per-install entropy as defense-in-depth.
- `flightLocks_` (broker) and `channelsStore` `#audience`/`#viewers` maps never prune removed
  accounts — bounded memory growth.
- `event_hub.cpp:44` `StartAccount` non-atomic stop-then-insert — worker leak only if ever
  called off the UI thread.
- Store `refresh()` has no in-flight guard (`multistreamStatusStore`/`canvasStore`/
  `outputBindingStore`/`streamProfileStore`) — latent last-writer-wins on event bursts.
- `oauthStore.whenReady` comment/behavior mismatch (one-shot `#ready`, not a subscription) —
  stale-read risk only if the permanent `channelsStore` sub is removed.
- `projector.changed` payload (`{opened}`) disagrees with its `Record<string, never>` type.
- `audio.setAdvanced` `tracks` shorter than 6 silently disables the trailing mixer tracks
  despite being a documented partial patch.
- Per-canvas header-dot `updateParameters` on every status push rewrites `layout.json`
  (write amplification); `onDidLayoutChange` disposer in StudioPage discarded (harmless —
  relies on `api.dispose()`).

### Verified clean (no action)
Dispatch is exception-safe on both lanes; per-element JSON type guards consistent; all 40
`EventNames` have a C++ producer and no orphan TS consumer; async completions are alive-guarded
+ drained; ref-counted store `subscribe()`/unsubscribe are balanced (no leaks); PKCE/CSRF/
loopback binding, no token logging, no bare-`refresh()` bypass, secrets hygiene (only public
client-id/signing-key committed). Engine locking/teardown/encode-once sharing is sound.
(The "errored output stays in the live set" bug is tracked separately at 4.4.4 below.)

---

## #5 — Cross-canvas scene duplicate undo/redo: nested-group and duplicate-source-usage edge cases

**Status:** OPEN (known, non-blocking follow-up) — 2026-07-09, `scenes.duplicateToCanvas`
(`frontend/src/bridge.cpp`, commits `6e6ce4ea3`..`4330bcef7`).

Four review rounds on the new cross-canvas scene-duplicate undo/redo closed every bug that
reproduces on any ordinary scene (redo not loading items; restored sources destroyed before
the scene could resolve them; nested scenes/groups not getting their own `.load` call; undo
deleting unrelated shared sources for nested-scene/`OBS_SOURCE_DO_NOT_DUPLICATE` items). Two
narrower gaps remain, found on the final pass, scoped to less common scene compositions:

- **Nested group's own children lost on redo.** `obs_scene_enum_items` only enumerates one
  level, so the capture loop serializes a group item itself but never the group's own
  children. A group's genuinely-duplicated grandchildren (e.g. a webcam+overlay grouped
  together, duplicated to another canvas) get destroyed on undo along with the group
  (harmless) but never restored on redo (`scene_load`'s internal `obs_get_source_by_uuid`
  lookup for the missing grandchild silently fails via a `blog` warning the bridge never
  surfaces).
- **Same source used twice within one duplicated scene** produces two identical
  `{uuid, sourceData}` entries in the undo/redo state (per-item, not per-source, capture);
  restoring both on redo would create two `obs_source_t` objects claiming the same uuid,
  making later uuid lookups ambiguous. Libobs's `obs_context_data_init_wrap` never enforces
  uuid uniqueness, so this doesn't crash, just corrupts the source registry's uuid index.

**Why not blocking:** both require a specific scene composition (a group, or a source
referenced twice within the same scene) rather than triggering on any duplicate. The common
case — any scene, any number of distinct non-grouped sources — is fully correct.

**Fix direction (not yet built):** capture per-*source* (deduped by uuid) rather than
per-*item*, and recurse one level into any captured group's own item list, serializing its
children the same way the top-level scene's are serialized today.

---

## #4 — Additional canvas previews are view-only (no drag layout editing)

**Status:** RESOLVED (implemented, build-green + reviewed; runtime GUI acceptance
owed) — Phase 3a, 2026-06-20, commits `69f4d59e`..`cbf227c5`. The canvas dock now
hosts an editable `OBSBasicPreview` bound to its canvas (Approach A:
`targetCanvas` parameterization, null = unchanged central preview). Sources in a
canvas dock are now draggable / selectable / transformable, scoped to that
canvas's scene. Plan + design below remain for context. Still owed: the manual
GUI acceptance pass; the related context-menu / create-new-source parity
(roadmap **3d**) is now also IMPLEMENTED (2026-06-20, commits `873846694`..
`c624fdca7` — full add-source picker, source/scene right-click menus, canvas-scoped
Edit Transform, all with canvas-aware undo; build-green + holistic review = SHIP,
GUI acceptance owed).

**Original status:** OPEN (design decision / future work) — surfaced 2026-06-16
from an edge-case question: "what if a user mainly uses an additional canvas, with
the Default canvas purely for inheritance?"

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
is non-trivial and must be scoped before committing. Tracked as **Phase 3a** in
`docs/roadmap.md`.

**Related deferred decision (roadmap Phase 3c).** When the Default canvas is
output-gated, the central preview currently shows an in-place "Preview Disabled"
placeholder (occupies space — CSS `visibility:hidden`-style). Whether to instead
`hide()` it (Qt's `display:none` equivalent) so the space is reclaimed is left for
later: the catch is it's the QMainWindow central widget (dock widgets can't occupy
it), so hiding makes surrounding docks reflow/balloon into the gap — the point-4
"starved layout" risk. Worth a quick reversible `hide()` experiment to judge the
reflow before committing.

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
  the same over-broad warning before; it's now gated the same way.) The editor
  dialog also disables the resolution/FPS inputs (and the inherit-resolution
  toggle) while the canvas is live, with a "locked while live" hint, so the change
  can't be attempted in the first place; the `ApplyCanvasEdit` guard remains as a
  backstop.

- **C1 (design decision) — RESOLVED: routed through apply/cancel.** Output-binding
  edits still commit immediately (so the canvas previews/docks react live while
  editing), but the dialog now snapshots `OutputBindings` on open
  (`outputBindingsBackup`), restores it on Cancel — both the Cancel button
  (`RejectRole`) and the Esc/close path (`reject()`), mirroring the Stream tab —
  and re-baselines the snapshot on Apply/OK. `RevertOutputBindings()` is a no-op
  when nothing changed (guarded by `OutputBinding::operator==`). Note: canvas
  add/remove remains immediate-commit by design (a management action), so a Cancel
  that follows a canvas removal won't resurrect the canvas, only its bindings.

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
   (worked); post-W1 it loads a fresh `%APPDATA%\braidcast`. W1 only
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

---

## Phase 4 (new frontend) — deferred tracking notes

### 4.4.4 fan-out engine — errored output stays in the live set (deferred)

`MultistreamEngine::OnOutputStop` sets a dropped/failed output's state to
`Error` but leaves its `LiveOutput` in the `live` vector; only an explicit
`StopOutput`/`StopAll` erases it. Consequence: after a stream errors out,
`IsCanvasLive`/`AnyLive` still report true, so the canvas video/audio settings
guard keeps refusing edits until the user manually stops that output. This is
faithful to the legacy `MultistreamOutput` and arguably safe-by-default (the
encoder is still bound to the canvas mix), but the guard intent ("refuse while
streaming") and behavior ("refuse while a LiveOutput exists, including errored")
diverge. Revisit when wiring the live status UX: either auto-reap errored
outputs from `live` or surface a "clear" affordance. Low priority; not a
correctness bug.

### 4.4.4 `Statuses()` cross-thread store read — RESOLVED

Fixed in `frontend: build multistream status off the UI thread`: the off-thread
output signal handlers no longer build the status array inline (which read the
UI-thread-mutated canvas/profile/binding stores); `EmitMultistreamChanged`
defers to `TID_UI` so `Statuses()` always reads the stores on the thread that
writes them.

### 4.4.5b multi-canvas panels — all-outputs-disabled first-run empty state (revisit before cutover)

The new uniform canvas panels are output-gated: a panel shows only when
`AnyEnabledForCanvas(uuid)` (the Default canvas included -- this is the deliberate
"hide the Default panel when disabled" capability the Qt central widget could
never do). Consequence: a fresh scene collection with zero output bindings shows
NO canvas panels at all -- the entire center editing area collapses to the empty
state ("No active canvases - enable an output in Settings -> Outputs"), and the
shared Sources panel shows "No canvas focused". A brand-new user must discover
Settings -> Outputs before anything is editable. This is the literal approved
behavior, not a bug; flagging as a pre-cutover UX decision: keep as-is, OR keep
the Default panel visible when NOTHING is enabled (gate only additional canvases
on first run), OR add an "enable default output" CTA in the empty state. Design
call -- not changed unilaterally.
