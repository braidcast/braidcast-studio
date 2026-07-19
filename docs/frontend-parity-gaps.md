# Frontend parity gaps — CEF/Svelte vs the old Qt frontend

Triage collection of every feature/UX-parity gap found between the **current
CEF/Svelte frontend** (`frontend/web/src/` + `frontend/src/bridge.cpp`) and the
**old Qt frontend** it replaced (deleted in `9aa092af3`; read from git history
at `9aa092af3^:frontend_old/`). This is a collection doc — no implementation,
no effort estimates.

**Method.** Qt baseline read via `git show 9aa092af3^:frontend_old/<path>`
(widgets, dialogs, `forms/*.ui` XML for provable buttons/layouts). Current
state read from the Svelte components and the `g_methods` bridge registry
(`frontend/src/bridge.cpp` — a method existing there means the capability is
wired natively; the gap is then UI-side). `docs/roadmap.md` was used to rule
out deferred-by-choice items (listed at the bottom, NOT counted as gaps). All
claims are code-read, not runtime-verified.

**Severity:** **Blocker** = core daily interaction broken or blind ·
**Major** = a feature OBS users regularly rely on is missing · **Minor-UX** =
convenience/polish/discoverability.

## Summary

| Severity | Count |
| --- | --- |
| Blocker | 4 |
| Major | 20 |
| Minor-UX | 25 |
| **Total** | **49** |

## Autonomous fix session (started 2026-07-18)

Fixing gaps one subagent at a time, Blockers first. Per-gap status is prefixed
inline on each row (**✅ Fixed `<commit>`** / 🔧 in progress / 🔴 queued /
⏸ deferred-needs-user). This table is the running summary.

| Gap | Sev | Status |
| --- | --- | --- |
| §16.1 YouTube privacy → silent public | Blocker | ✅ Fixed `acd9a432b` |
| §1.1 drag-reorder source list | Blocker | ✅ Fixed `d5722e492` |
| §4.1 Properties OK/Cancel/Restore Defaults | Blocker | ✅ Fixed `98a30814e` |
| §4.2 blind preview while editing | Blocker | ⏸ deferred — approach decision |
| §2.1 scene reorder (buttons/menu/drag) | Major | ✅ Fixed `a7d15bb7d` |
| §11.3 transform hotkeys | Major | ✅ Fixed `15f761b33` |
| §1.4 blending mode/method submenus | Major | ✅ Fixed `ba36733c9` |

Blockers cleared (3 fixed, §4.2 deferred). Now working Major gaps in severity order.

**Deferred (needs user decision when back):**
- **§4.2 — live preview while editing the Properties/Filters dialog.** The native preview is an always-on-top child HWND, so `Modal.svelte` suspends it for every modal's lifetime → property/filter edits are invisible until close. The only robust fix is an **in-dialog `obs_display` child surface** (a preview pane inside the modal, like Qt's `OBSQTDisplay` above the form) — reusing the `preview_window.cpp` machinery. Sub-scope needs a call: (a) which source kinds get a pane (Qt: any video source), (b) whether transitions get the A/B "Preview Transition" pane, (c) pane placement (above the form vs. beside it). Recommend: in-dialog pane for video sources first; transition A/B as a follow-up. Not a lighter per-modal-opt-out job — un-suspending without an in-dialog surface just lets the native window paint over the modal.

The five user-reported seed gaps are all confirmed (§1.1, §3.1, §1.7, §3.2,
§4.1) — two with refinements: sources DO have up/down reorder (drag is what's
missing), and double-click-properties exists in the standalone Sources dock
but not the canvas-dock lists.

---

## 1. Sources dock

Qt baseline: `frontend_old/components/SourceTree{,Model,Item}.{hpp,cpp}`,
`frontend_old/widgets/OBSBasic_SceneItems.cpp`, `forms/OBSBasic.ui`
(`sourcesToolbar`). Current: `frontend/web/src/lib/docks/SourcesDock.svelte`
and the embedded rows in `lib/docks/CanvasDock.svelte`.

| # | Gap | Qt did | Current | Severity | Fix lives in |
| --- | --- | --- | --- | --- | --- |
| 1.1 | **No drag-reorder in the source list** (seed 1) | `SourceTree::dropEvent` — internal-move DnD incl. multi-select drags, drop into/out of groups, one undo action (`SourceTree.cpp`) | ✅ **Fixed `d5722e492`** — HTML5 DnD on both dock source rows; `sceneItems.reorder` gained a top-first `to` index (one undo action, shares `ApplyOrder`). Multi-select drags + drop-into-groups still owed (tracked under §1.2 multi-select / groups) | **Blocker** | `SourcesDock.svelte`, `CanvasDock.svelte` (DnD) → `sceneItems.reorder` |
| 1.2 | **No multi-select** | `ExtendedSelection` on the list (`forms/OBSBasic.ui`); gates group-N-items, batch remove ("Remove N Items"), batch transform ops | **Missing** — single `selectedItemId` (`SourcesDock.svelte:42`) | Major | selection model in `SourcesDock`/`CanvasDock` + `sourceSelection` store; bridge ops loop per item |
| 1.3 | **Double-click does not open Properties in canvas-dock source lists** (seed 3) | `SourceTreeItem::mouseDoubleClickEvent` → Properties for any configurable source | **Partial** — standalone dock has it (`SourcesDock.svelte:443` `ondblclick`); the embedded `CanvasDock.svelte` source rows have **no** dblclick handler (only scene-rename at `:901`). Already flagged as list-fork drift in roadmap "Code-health deferrals (a)" | Major | `CanvasDock.svelte` embedded rows (or the shared-row extraction from the code-health deferral) |
| 1.4 | **Blending Mode / Blending Method submenus missing** | `AddBlendingModeMenu` (Normal/Additive/Subtract/Screen/Multiply/Lighten/Darken) + `AddBlendingMethodMenu` (Default/SRGB Off), per item, undo-wrapped (`OBSBasic_SceneItems.cpp`) | ✅ **Fixed `ba36733c9`** — `sceneItems.setBlendingMode`/`setBlendingMethod` bridge methods (token↔enum tables + undo, mirroring the scale-filter path); `blendMode`/`blendMethod` in `sceneItems.list`; new `blendMenu.ts` wired into both source context menus (all 3 CanvasDock call sites incl. the preview menu) | Major | `bridge.cpp`, `blendMenu.ts`, `SourcesDock.svelte`, `CanvasDock.svelte` |
| 1.5 | **Per-item Show/Hide Transitions missing** | `CreateVisibilityTransitionMenu(visible)` — per-item show & hide transition type + duration + properties, copy/paste transition settings (`OBSBasic_Transitions.cpp`) | **Missing** — no menu, no bridge method | Major | new `sceneItems.*Transition` bridge methods + entries in the source context menu |
| 1.6 | **OS file drag-drop creates nothing** (drop a file/URL → source) | `OBSBasic_Dropfiles.cpp` — extension→type table (txt→text, html/URL→browser w/ confirm, images→`image_source`, media→`ffmpeg_source`), raw-text drops | **Missing** — window-level drop is deliberately inert (navigation guard only, `App.svelte:120-128`); no drop target creates sources | Major | drop handler in `App.svelte`/`PreviewDock.svelte` → `sources.create` with per-extension settings (reuse `addSourceCategories.ts` typing) |
| 1.7 | **Copy/paste drops item state; no Paste (Duplicate)** | `SourceCopyInfo` carried transform, crop, blend, visibility, scale filter, show/hide transitions; Paste (Reference) AND Paste (Duplicate) (`OBSBasic_Clipboard.cpp`) | **Partial** — Ctrl+V / Paste = `sources.addExisting` reference only, no transform carry (`App.svelte:95-104`, `SourcesDock.svelte:242-257`); context-menu Duplicate exists (`sources.duplicate`) | Major | `clipboardStore` + paste path (capture `sceneItems.getTransform` on copy, apply on paste) |
| 1.8 | Custom color label missing | 8 presets + **Custom Color** (QColorDialog, live preview) + Clear (`AddBackgroundColorMenu`) | **Partial** — None + 8 presets only (`lib/menus/colorMenu.ts:18-33`); `sceneItems.setColor` takes the value already | Minor-UX | `colorMenu.ts` + a color-input popover |
| 1.9 | Hide-in-mixer toggle missing | "Hide in Mixer" on audio sources (`CreateSourcePopupMenu`) | **Missing** (mixer shows all audio sources; see §8) | Minor-UX | source context menu + mixer hidden-state (likely `audio.setAdvanced` or private settings) |
| 1.10 | "Resize Output (Source Size)" missing | Menu entry resizing canvas to source size | **Missing** — arguably superseded by the canvas model; decide, don't assume | Minor-UX | source context menu → `canvas.update` |

## 2. Scenes dock

Qt baseline: `frontend_old/components/SceneTree.{hpp,cpp}`,
`frontend_old/widgets/OBSBasic_Scenes.cpp`. Current:
`frontend/web/src/lib/docks/ScenesDock.svelte`.

| # | Gap | Qt did | Current | Severity | Fix lives in |
| --- | --- | --- | --- | --- | --- |
| 2.1 | **No scene reorder at all** | `SceneTree` InternalMove drag (incl. grid-mode math) + toolbar Move Up/Down + Order submenu | ✅ **Fixed `a7d15bb7d`** — ScenesDock Move Up/Down buttons + Order submenu (Up/Down/Top/Bottom) + drag-to-reorder (list & grid). `scenes.reorder` gained a `to` index; native `ReorderScene` gained top/bottom + `MoveSceneToIndex`. **Default canvas only** — additional-canvas scene reorder (`CanvasDock` scene list) still refused by the bridge; separate native gap | Major | `ScenesDock.svelte`, `scene_persistence.{hpp,cpp}`, `bridge.cpp` |
| 2.2 | **No Filters on a scene** (incl. copy/paste scene filters) | Scenes menu: Filters, Copy Filters, Paste Filters (`on_scenes_customContextMenuRequested`) | **Missing** — scene context menu is Rename/Duplicate/Duplicate-to-canvas/Remove only (`ScenesDock.svelte:168-176`). `filters.*` bridge methods resolve by source name — scenes are sources, so this is likely UI-only | Major | `ScenesDock.svelte` context menu → existing `filters.*` / `filters.copyChain`/`pasteChain` |
| 2.3 | Screenshot Scene missing | "Screenshot Scene" in scene menu | **Missing** — only per-source + program screenshots exist | Minor-UX | scene menu → `screenshot.takeSource` (scene is a source) |
| 2.4 | Show-in-Multiview toggle missing | Per-scene checkable "Show in Multiview" (private setting `show_in_multiview`) | **Missing** (multiview shipped as backlog Item 10, but no per-scene inclusion toggle) | Minor-UX | scene menu + multiview render filter |

## 3. Add Source dialog

Qt baseline: `frontend_old/dialogs/OBSBasicSourceSelect.{hpp,cpp}` +
`forms/OBSBasicSourceSelect.ui` (already reworked in this fork: type list +
thumbnail tiles of existing sources). Current:
`frontend/web/src/lib/dialogs/add-source/AddSourceModal.svelte`.

| # | Gap | Qt did | Current | Severity | Fix lives in |
| --- | --- | --- | --- | --- | --- |
| 3.1 | **Reuse is not the default when an existing source of the type exists** (seed 2) | If the type has ≥1 existing source, the first existing tile is **pre-checked** so "Add Existing" is the primed default action (`sourceTypeSelected` heuristic); otherwise focus goes to the new-name field | **Partial** — "Use existing" radio exists with a preselected candidate, but `mode` defaults to `"new"` unconditionally (`AddSourceModal.svelte:100-104`) | Major | `AddSourceModal.svelte` — default `mode = existing.length ? "existing" : "new"` |
| 3.2 | **No preview of sources in the dialog** (seed 4) | Per-source **thumbnail tiles** (`SourceSelectButton` + `ThumbnailManager`) for every existing source — you see what you're re-adding (no full live pane in Qt either) | **Missing** — text-only radio list, no thumbnails, no preview | Major | `AddSourceModal.svelte` + a thumbnail path (`screenshot.takeSource` → `file.readDataUri`, or a new `sources.thumbnail` bridge method) |
| 3.3 | "Add Visible" checkbox missing | `sourceVisible` checkbox (default checked) controls initial item visibility | **Missing** | Minor-UX | `AddSourceModal.svelte` + `sceneItems.setVisible` after add |
| 3.4 | Multi-select add-existing missing | Ctrl/shift multi-select tiles, "Add N Existing" | **Missing** — one existing source at a time | Minor-UX | `AddSourceModal.svelte` |

## 4. Properties dialog

Qt baseline: `frontend_old/dialogs/OBSBasicProperties.{hpp,cpp}` +
`forms/OBSBasicProperties.ui`. Current: `Modal` + `PropertyForm` opened from
`SourcesDock.svelte:465-467`, `PreviewDock.svelte:214-218`,
`CanvasDock.svelte:1003-1006`.

| # | Gap | Qt did | Current | Severity | Fix lives in |
| --- | --- | --- | --- | --- | --- |
| 4.1 | **No OK / Cancel / Restore Defaults** (seed 5) | `buttonBox` OK+Cancel + `defaultsButton`; Cancel restores the `oldSettings` snapshot taken at open; closing via X with changes asks Save/Discard/Cancel | ✅ **Fixed `98a30814e`** — new `PropertiesModal.svelte` wraps `Modal`+`PropertyForm` with a Restore Defaults · Cancel · OK footer; `PropertyForm` snapshots settings once on open and exposes `revert()`/`restoreDefaults()`; Cancel/Esc/X revert (merge-push the snapshot), OK keeps. New bridge `properties.defaults` = `obs_data_clear`+update. **Decision baked in:** Esc/X = discard (not the Qt Save/Discard/Cancel 3-way prompt); UndoManager capture of edits still not wired (separate) | **Blocker** | `PropertiesModal.svelte`, `PropertyForm.svelte`, `bridge.cpp` (`properties.defaults`) |
| 4.2 | **No live preview while editing — the dialog edits blind** | `OBSQTDisplay` preview pane above the form for any video source; transitions get an A/B preview + "Preview Transition" button | **Missing, and worse than absent**: `Modal.svelte:99` suspends the native preview overlay for the modal's lifetime, so the main preview is hidden too — property changes are invisible until the dialog closes. Same mechanism blinds the Filters dialog (§5.1) | **Blocker** | `Modal.svelte`/`previewGate` (per-modal opt-out of suspension) or an in-dialog `obs_display` child surface (`preview_window.cpp` machinery) |

## 5. Filters dialog

Qt baseline: `frontend_old/dialogs/OBSBasicFilters.{hpp,cpp}`. Current:
`frontend/web/src/lib/dialogs/FilterDialog.svelte`.

| # | Gap | Qt did | Current | Severity | Fix lives in |
| --- | --- | --- | --- | --- | --- |
| 5.1 | **No live preview in the dialog** | `OBSQTDisplay` preview for video sources (`DrawPreview`) | **Missing** — explicitly documented in-code as a known follow-up (`FilterDialog.svelte:16-22`); preview suspended for the dialog's lifetime | Major | same seam as §4.2 |
| 5.2 | In-dialog filter Copy/Paste/Duplicate missing | Single-filter clipboard (Ctrl+C/V in-dialog), Duplicate with name prompt | **Partial** — whole-chain copy/paste exists but only from the *source* context menu (`filters.copyChain`/`pasteChain`, `SourcesDock.svelte:267-287`); nothing inside the dialog | Minor-UX | `FilterDialog.svelte` (wire existing `filters.*` methods) |
| 5.3 | No drag-reorder of filters | Drag within list (`ReorderFilter`) + up/down buttons | **Partial** — up/down buttons only (`FilterDialog.svelte:115-122`) | Minor-UX | `FilterDialog.svelte` → `filters.reorder` |
| 5.4 | No async/effect (audio/video) list split | Two lists (`asyncFilters`/`effectFilters`) | **Present-but-different** — single list; the add-picker has Video/Audio optgroups | Minor-UX | `FilterDialog.svelte` (grouping only) |

## 6. Transform & preview editing

Qt baseline: `frontend_old/dialogs/OBSBasicTransform.*`,
`frontend_old/widgets/OBSBasic_Preview.cpp`, `forms/OBSBasic.ui`
(`transformMenu`). Current: `lib/dialogs/TransformDialog.svelte`,
`lib/menus/transformMenu.ts`, native preview overlay.

| # | Gap | Qt did | Current | Severity | Fix lives in |
| --- | --- | --- | --- | --- | --- |
| 6.1 | "Crop to Bounds" checkbox missing | `cropToBounds` in the Transform dialog | **Missing** — dialog covers pos/rot/scale/align/crop/bounds+align (`TransformDialog.svelte:169-337`) but not crop-to-bounds | Minor-UX | `TransformDialog.svelte` + `sceneItems.get/setTransform` field |
| 6.2 | Preview zoom / scaling modes missing | Scale to Window/Canvas/Output, Zoom In/Out, Reset Zoom (Edit → Scale submenu; `actionPreviewZoom*`) | **Missing** — fixed letterboxed fit | Minor-UX | preview surface (`previewSurface.ts` + native `preview_window.cpp`) |
| 6.3 | Safe areas + spacing helpers missing | `drawSafeAreas` (action/graphic/4:3 margins), `drawSpacingHelpers` overlays | **Missing** | Minor-UX | native preview overlay drawing (`preview_window.cpp`) |
| 6.4 | Lock Preview missing | "Lock Preview" toggle (blocks editing in the preview) | **Missing** — only per-item lock exists | Minor-UX | preview context menu + native hit-test gate |

## 7. Preview right-click menus (consistency)

Qt had ONE menu builder (`CreateSourcePopupMenu`) for list + preview. Current
has three divergent builders.

| # | Gap | Qt did | Current | Severity | Fix lives in |
| --- | --- | --- | --- | --- | --- |
| 7.1 | **Default-canvas preview menu is a subset of both the Sources-dock menu and the additional-canvas preview menu** | Same full menu everywhere (scale filtering, blending, deinterlace, color, screenshot, interact, copy/paste, group, projector) | **Present-but-different** — `PreviewDock.svelte:68-103` lacks Scale Filtering / Deinterlacing / Color / Screenshot / Interact / Copy-Paste / Group vs `CanvasDock.svelte:614-659` and vs the row menu (`SourcesDock.svelte:342-401`) | Major | `PreviewDock.svelte` — compose from the same `lib/menus/*` helpers (the seam already exists) |
| 7.2 | Canvas-dock preview menu omits Properties/Filters | Properties/Filters always present | **Present-but-different**, intentional per comment (`CanvasDock.svelte:611-613`, "additional-canvas private sources") — but under Model 1 sources are shared/global, so revisit the rationale | Minor-UX | `CanvasDock.svelte` `buildPreviewItems` |
| 7.3 | Empty-area preview right-click has no Add Source / New Group / Paste | `CreateSourcePopupMenu` with no selection offered Add Source, New Group, Paste | **Missing** | Minor-UX | `PreviewDock.svelte`/`CanvasDock.svelte` menu builders |

## 8. Audio mixer

Qt baseline: `frontend_old/widgets/AudioMixer.cpp`,
`frontend_old/components/VolumeControl.cpp`, `forms/OBSAdvAudio.ui`. Current:
`lib/docks/AudioMixerDock.svelte`, `lib/dialogs/AdvAudioDialog.svelte`.

| # | Gap | Qt did | Current | Severity | Fix lives in |
| --- | --- | --- | --- | --- | --- |
| 8.1 | **No per-row context menu** (Rename, Hide/Unhide, Pin, Lock Volume, Copy/Paste Filters) | `VolumeControl::showVolumeControlMenu` — Unhide All / Pin / Hide / Lock Volume / Copy Filters / Paste Filters / Rename / Filters / Properties | **Partial** — inline Mute / Filters / AdvAudio buttons only (`AudioMixerDock.svelte:153-173`); no rename, hide, pin, lock-volume, copy/paste filters, no Properties | Major | `AudioMixerDock.svelte` + `ContextMenu` (rename/filters/props reuse existing bridge; hide/pin/lock need state) |
| 8.2 | No monitoring quick-toggle on the row | Headphone button (monitor-only / monitor+output cycling) beside Mute | **Missing** — monitoring only via the AdvAudio dropdown | Minor-UX | `AudioMixerDock.svelte` → `audio.setAdvanced` |
| 8.3 | No hidden/inactive management or vertical layout | Toolbar: toggle-hidden (with count), vertical/horizontal layout, show-hidden/show-inactive/keep-last options menu | **Missing** | Minor-UX | `AudioMixerDock.svelte` |
| 8.4 | AdvAudio: no %-volume mode, no "Active only" filter | Header "%" checkbox flips dB↔% spinboxes; "Active only" (default on) hides inactive sources | **Missing** — dB only, all sources always listed (`AdvAudioDialog.svelte:116-199`) | Minor-UX | `AdvAudioDialog.svelte` |
| 8.5 | Meter decay-rate / peak-meter-type settings missing | `Audio.MeterDecayRate` / `Audio.PeakMeterType` config read by the VU meters | **Missing** (meters are live w/ peak-hold, but fixed behavior) | Minor-UX | Settings → Audio + `AudioMixerDock.svelte` meter math |

## 9. Transitions

Qt baseline: `frontend_old/widgets/OBSBasic_Transitions.cpp`,
`forms/OBSBasic.ui` (`transitionsDock`). Current:
`lib/docks/TransitionsDock.svelte`.

| # | Gap | Qt did | Current | Severity | Fix lives in |
| --- | --- | --- | --- | --- | --- |
| 9.1 | **Transition properties UI missing** — Stinger / Fade-to-Color / Luma-Wipe are stuck on defaults | Add-menu of configurable transition types + Properties/Rename per instance | **Missing** — type dropdown + duration only (`TransitionsDock.svelte`); roadmap lists this as a known follow-up of the 2026-06-24 transitions work | Major | `TransitionsDock.svelte` + `PropertyForm` (needs a transition property kind or a `properties.get` path for the transition source) |
| 9.2 | **Per-scene transition override missing** | `CreatePerSceneTransitionMenu` — None/any transition + duration, stored in scene private settings | **Missing** — one global transition, no per-scene affordance | Major | `ScenesDock.svelte` context menu + a `transitions.setSceneOverride` bridge method + the `Transitions::SetProgramScene` seam |
| 9.3 | Multiple named transition instances missing | Add/Remove/Rename configured transitions (e.g. two stingers) | **Missing** — single current transition of a type | Minor-UX | `TransitionsDock.svelte` + `transitions.*` bridge extension |

## 10. Projectors & multiview — intentionally hidden (NOT gaps)

No counted gaps in this area. The projector/multiview UI entry points are
**hidden on purpose** (user decision) — see the "Deferred by design" section
below for the full note. Recorded here only so the section numbering matches
the audit areas.

## 11. Keyboard shortcuts

Qt baseline: `forms/OBSBasic.ui` QActions + `OBSBasic_Hotkeys.cpp` +
preview-widget nudge actions (`OBSBasic.cpp:519-535`). Current: the only
global bindings are in `App.svelte:70-111` — F11, Ctrl+Z/Y (+Ctrl+Shift+Z),
Ctrl+C/V, Ctrl+Shift+S. Settings → Hotkeys exists for *libobs* hotkeys.

| # | Gap | Qt binding | Current | Severity | Fix lives in |
| --- | --- | --- | --- | --- | --- |
| 11.1 | **Delete does not remove the selected item/scene** | `Del` on `actionRemoveSource`/`actionRemoveScene` (with confirm; batch-aware) | **Missing** | Major | `App.svelte` keydown + `sourceSelection` → `sceneItems.remove` |
| 11.2 | **Arrow-key nudge missing** | Arrows = 1 px, Shift+Arrows = 10 px on the preview (`OBSBasic::Nudge`) | **Missing** — no ArrowKey handling anywhere transform-related | Major | `App.svelte` or native preview keyboard path → `sceneItems.setTransform` |
| 11.3 | **Transform hotkey set missing** | Ctrl+E edit, Ctrl+R reset, Ctrl+F fit, Ctrl+S stretch, Ctrl+D center, Ctrl+Shift+C/V copy/paste transform | ✅ **Fixed `15f761b33`** — all 7 wired into the existing `App.svelte onKeydown` chain (reset/fit/stretch/center via one `quickTransform` helper → `sceneItems.transformAction`; edit via `openTransform`; copy/paste via existing `clipboard.transform` + `sceneItems.get/setTransform`). Source Ctrl+C/V gained a `!e.shiftKey` guard so the Shift transform variants don't collide | Major | `App.svelte` |
| 11.4 | Order hotkeys missing | Ctrl+Up/Down (move), Ctrl+Home/End (top/bottom) | **Missing** | Minor-UX | `App.svelte` → `sceneItems.reorder` |
| 11.5 | F2 inline rename missing | `renameSource`/`renameScene` on F2 | **Missing** — rename only via context menu / dblclick (scenes) | Minor-UX | `App.svelte` + the docks' `beginRename` |
| 11.6 | **Per-scene switch hotkeys missing** | Per-scene select hotkeys (frontend-registered in stock OBS) | **Missing** — roadmap's scene-linking invariant note confirms "there is no scene-switch hotkey in this frontend today"; any future one must also call `ApplyCanvasSceneLinks` | Major | native `Hotkeys` store (register per-scene) + `scenes.setCurrent` path |
| 11.7 | Frontend hotkey registrations thinner than Qt | Qt registered vcam start/stop, transition trigger, screenshot-selected-source, enable/disable preview, reset stats as *rebindable* `obs_hotkey`s | **Partial** — only Start/Stop Streaming registered (roadmap); screenshot is a fixed Ctrl+Shift+S | Minor-UX | `frontend/src/multistream/Hotkeys` registrations |

## 12. Status bar / persistent indicators

Qt baseline: `frontend_old/widgets/OBSBasicStatusBar.cpp` +
`forms/StatusBarWidget.ui`. Current: Studio GO-LIVE bar
(`StudioPage.svelte:757-834`, perfRow CPU/FPS/RENDER/ENCODE/NET) + `StatsDock`.

| # | Gap | Qt did | Current | Severity | Fix lives in |
| --- | --- | --- | --- | --- | --- |
| 12.1 | No congestion indicator / dropped-frames % in the always-visible bar | 5-state congestion icon (rolling `obs_output_get_congestion`) + "N dropped (P.P%)" while live | **Present-but-different** — perfRow shows render-lag/encode-skip/bitrate; congestion + network-drop % only inside `StatsDock` | Minor-UX | `StudioPage.svelte` perfRow ← `multistream.status`/`stats.get` |
| 12.2 | Stream-delay start/stop countdown missing | `delayInfo` "Delay starting/stopping in Ns" | **Missing** (stream delay is configurable in Advanced settings) | Minor-UX | GO-LIVE bar ← engine delay state |

## 13. Settings & misc

| # | Gap | Qt did | Current | Severity | Fix lives in |
| --- | --- | --- | --- | --- | --- |
| 13.1 | Accessibility settings missing | Accessibility page (color overrides for meters/selection) | **Missing** — sub-nav is General/Audio/Hotkeys/Browser Docks/Appearance/Advanced/Diagnostics (`SettingsPage.svelte:17-25`); Streams/Outputs/Canvases intentionally moved to their own pages | Minor-UX | new Settings tab + theme tokens (may fold into Appearance) |
| 13.2 | Log upload missing | Help → Logs → Upload Current/Last Log (`LogUploadDialog`) | **Missing** — `LogViewerDialog` has Copy/Refresh/Open Folder only. May be intentional for a private fork — decide, don't assume | Minor-UX | `LogViewerDialog.svelte` (+ an upload endpoint decision) |
| 13.3 | Undo coverage of live-applied edits unverified | Qt wrapped properties edits, adv-audio fields, blend/deinterlace changes, reorder in undo actions | **Unverified** — `UndoManager` covers scene-item ops (backlog Item 1); whether `properties.set` / `audio.setAdvanced` edits are undoable needs a check — with no Cancel button (§4.1) undo is the only rescue path | (verification note, not counted) | `frontend/utility/UndoManager` + bridge write paths |

## 14. Modal / UI consistency & polish

Not Qt-parity items — internal consistency/polish defects in the current UI.
For the modal bottom bar the Go Live modal's footer is the reference look:
actions as full-height edge-to-edge blocks, like the Studio GO LIVE button.

| # | Gap | Expected | Current | Severity | Fix lives in |
| --- | --- | --- | --- | --- | --- |
| 14.1 | **Secondary/ghost footer buttons are not full-height in the modal bottom bar** | ALL buttons on a modal footer fill the bar height (match the Go Live primary treatment) | The shared shell stretches only the primary: `Modal.svelte` `.modal-foot :global(.accent), .modal-foot :global(.btn:not(.ghost))` get `align-self: stretch` + `border-left` (`frontend/web/src/lib/ui/Modal.svelte:301-309`), while `.modal-foot :global(.ghost)` (`:313-324`) is excluded — the comment even codifies it ("keeps its natural height (never stretched)"). Every ghost secondary floats centered at natural height against the 42 px bar. One shared rule, so the fix is one place; all 11 footer-snippet modals inherit it (GoLive, Schedule "New Stream", Transform, OAuthConnect, LogViewer, MissingFiles, Importer, Filter, Collection, AdvAudio, About). Plain `.btn` Close buttons DO stretch — the visible mismatch is exactly the ghost-Cancel-beside-accent modals: `GoLiveModal.svelte:841`, `CollectionDialog.svelte:78`, `ImporterDialog.svelte:292-295`, `OAuthConnectDialog.svelte:165`, `LogViewerDialog.svelte:74-76` | Minor-UX | `Modal.svelte` `.modal-foot` CSS — extend the stretch rule (padding/border-left) to `.ghost` and drop the stale comment |
| 14.2 | Duplicated ghost-button styles in GoLiveModal | One shared footer style | `GoLiveModal.svelte:1267-1278` carries a local `.ghost` block duplicating `Modal.svelte:313-324` — collapse into the shared rule when fixing 14.1 | Minor-UX | `GoLiveModal.svelte` (delete), `Modal.svelte` (keep) |
| 14.3 | Add Source modal has no footer bar at all | Confirm actions live on the modal bottom bar | `AddSourceModal.svelte` puts its `.accent` confirm inline in the body (`:205`) and passes no footer snippet | Minor-UX | `AddSourceModal.svelte` — move confirm/cancel into the `Modal` footer snippet |
| 14.4 | Canvas Destinations "NEW DESTINATION" profile picker — congested rows + clipping | Dropdown rows readable, list fully visible within the panel | The shared search-dropdown `frontend/web/src/lib/ui/ProfileSelect.svelte` (avatar + name + "{platform} - {service}" subline), rendered under `lib/canvas/CanvasDestinationsTab.svelte`'s "Search destinations…" field, renders cramped and gets cut off by the surrounding Destinations panel — last item's name hard-clips (user screenshot). Rows: `padding: 11px 12px` + `gap: 11px` (`ProfileSelect.svelte:207-210`) with a 30 px avatar and a two-line layout; scroll container `max-height: 320px` (`:188`) exceeds the space the panel gives it | Minor-UX | `ProfileSelect.svelte` — row density + dropdown max-height/positioning vs the host panel; verify name ellipsis engages instead of hard-clipping |

## 15. Account / OAuth UX

Not Qt-parity — a design-model UX flaw in the current account model. A
`StreamProfile` holds an `accountId` referencing a SHARED account record in
`AccountStore` (keyed `providerId:userId`, connect-once reuse — "One account
record here may be referenced by several stream profiles",
`frontend/src/oauth/account_store.hpp:17`).

| # | Gap | Expected | Current | Severity | Fix lives in |
| --- | --- | --- | --- | --- | --- |
| 15.1 | **No per-profile "switch/change account" — changing one profile's account forces a global disconnect** | A connected profile can be re-pointed at another account (or a fresh connect) without touching other profiles | **Dead-end** — in `frontend/web/src/lib/settings/StreamsTab.svelte` (Authentication, `:726-786`) the states are mutually exclusive: the connected branch (`connectedStatus`, `:739-751`) renders ONLY "Disconnect" (`:750`) → `oauth.disconnect` = **global** account removal unlinking ALL profiles (the confirm at `:355-364` warns as much). The "Reuse existing account" picker + "Connect a different account" (`:763-776` → `oauth.linkAccount` / `oauth.connect`) render only in the NOT-connected branch. The backend fully supports per-profile re-pointing (`oauth.linkAccount` re-points ONE profile, `bridge.cpp:8565/:9463`); there is no per-profile unlink method. Workaround today: create a new profile, use its unlinked connect flow, move output bindings over, delete the old profile | Major | `StreamsTab.svelte` connected branch — surface a "Switch account" affordance (reuse picker + connect-different, both existing bridge methods, no backend change); OR a new `oauth.unlinkProfile` bridge method clearing just this profile's `accountId` (vs global `oauth.disconnect`) |
| 15.2 | "Connect a different account" may silently re-grant the same Google account | The YouTube authorize flow lets the user pick which Google account to grant | The broker's YouTube authorize sends `prompt=consent`, not `prompt=select_account` (braidcast-website repo, `auth/src/providers.ts` youtube `extraAuthParams`) — with one active Google session the account chooser never appears, so "Connect a different account" can only re-add the same one | Minor-UX | braidcast-website `auth/src/providers.ts` — `prompt=select_account consent` (external repo; noted here so the 15.1 fix isn't judged broken when tested with a single Google session) |

## 16. Go Live / provider defaults

Not Qt-parity — a safety defect in the current Go Live provider-field model.

| # | Gap | Expected | Current | Severity | Fix lives in |
| --- | --- | --- | --- | --- | --- |
| 16.1 | ✅ **Fixed `acd9a432b`.** ~~**YouTube "Privacy" defaults to "—" (unset) and silently becomes PUBLIC on Go Live**~~ | An outward action never broadens visibility without an explicit choice — unset privacy should block Go Live or resolve to a safe value (private/unlisted, or the user's remembered last choice); never silently public | **Unsafe default** — the `privacy` enum field is declared with options public/unlisted/private but **no default / no pre-selected option** (`frontend/src/oauth/youtube_provider.cpp:184-191`), so the Go Live modal renders it as the "—" unset placeholder. On submit, `Str(fields, "privacy")` returns the empty/invalid value and the guard coerces it: `if (privacy != "public" && privacy != "unlisted" && privacy != "private") privacy = "public";` (`youtube_provider.cpp:356-358`), which is then sent as `privacyStatus` to the YouTube liveBroadcasts API (create `:557`, update `:531`). Net: a user who never touches Privacy — e.g. a quick test stream — broadcasts publicly without ever choosing "Public". The "—" never reaches the API; the silent coercion is the hazard | **Blocker** (safety — unintended public broadcast) | `youtube_provider.cpp` field declaration — give `privacy` an explicit **safe** default (remembered last choice via `StreamMetaStore`, else private/unlisted); and/or `GoLiveModal.svelte` — require an explicit privacy selection before Go Live enables. The coercion at `:356-358` must stop being the silent default path (keep it only as a last-resort validity guard, pointed at a safe value) |

---

## Deferred by design (NOT gaps — do not re-file)

Ruled out against `docs/roadmap.md`; anything here is a conscious product
decision, not a parity bug.

- **Studio Mode / preview-program staging (3b)** — deferred 2026-06-25 as
  low-value for this fork. This also covers Qt's **Quick Transitions**, the
  **T-Bar**, and the preview/program duplicate-scene toggles — all Studio-Mode
  furniture.
- **Recording / Replay buffer** — kept dormant (streaming-only fork,
  2026-06-24). Covers Show Recordings, Remux, split-file/chapter hotkeys,
  pause, and the low-disk-space warning.
- **obs-websocket** — excluded (Qt-coupled, hard-crashes headless); Phase 5
  MCP is the remote-control surface instead.
- **GoLive / Multitrack Video** — dormant (Twitch Enhanced Broadcasting is
  orthogonal to the many-platforms goal).
- **Item 9 — per-binding output overrides** (stream delay/reconnect/bind-IP
  per binding) — deferred.
- **Auto-Configuration wizard** — deferred (per the OBS-parity backlog notes).
- **Settings live-apply with no Cancel** — the Phase 7 page model
  intentionally dropped the modal's transactional revert (roadmap Phase 7
  header). The *Properties* dialog Cancel (§4.1) is NOT covered by this
  decision — that dialog was never part of the Settings-page call.
- **Facebook provider (8f)** — deferred pending App Review paperwork.
- **Menu bar as such** — replaced by the nav-rail IA (Phase 7, locked design);
  individual orphaned *actions* are only gaps where listed above (§10.1).
- **Idian Playground, What's New, update checker, macOS permissions dialog,
  YouTube-specific docks/actions** — dev tooling / upstream-service furniture
  superseded by the fork's own OAuth (Phase 8) or irrelevant to a private
  fork.
- **Projector + multiview UI entry points — intentionally hidden pending a
  value case (user decision).** The capability is fully built (native
  `ProjectorManager`, `projector.*` / `display.listMonitors` bridge methods,
  multiview = backlog Item 10) and the frontend wiring exists but is
  deliberately not exposed: `lib/menus/projectorMenu.ts` has no callers, the
  menu entries are commented out in `SourcesDock.svelte:396`,
  `ScenesDock.svelte:172`, `PreviewDock.svelte:96`, `CanvasDock.svelte:604,653`,
  and `StudioPage.svelte:479-504` `openProgram*/openMultiview*` are uncalled.
  This is not a bug — no clear value case for exposing projectors/multiview has
  emerged yet; re-expose by re-wiring those existing seams when wanted. (Qt
  extras like windowed-projector always-on-top and projector persistence are
  likewise moot until the surface is exposed.)
- **Undo/redo and Multiview are NOT deferred as capabilities** — both shipped
  in the OBS-parity backlog (Items 1 and 10); multiview's *UI* is intentionally
  hidden per the entry above, undo's open point is coverage verification
  (§13.3).
