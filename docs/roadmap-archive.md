# Braidcast — Roadmap Archive

Completed phases moved out of `roadmap.md` to keep it focused on active work.
Content is verbatim as of the 2026-07-16 move; `roadmap.md` carries live status.

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
