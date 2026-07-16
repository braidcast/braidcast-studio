<!-- Extracted from roadmap.md on 2026-07-16 to keep the roadmap focused on active work. -->

# Issues & Enhancements

Findings from the deep review sweep started 2026-07-15, tracked to closure. Review agents
**append** findings here; implementation agents **update status in place** and record the
fixing commit. Status: 🔴 open · 🔧 in progress · ✅ fixed · ⏸ won't fix (with reason) ·
❌ withdrawn (not a real defect).

Every finding was re-derived against source by the reviewer and cites the decisive line.
Two claims were **withdrawn during review** and are recorded so nobody re-raises them: a
`sources.relinkMissing` deadlock (libobs's `sources_mutex` is recursive) and an obs-browser
module-unload UAF (`obs-module.c:792` never `dlclose`s).

## Pass 1 — C++ host reliability (2026-07-15)

Scope: `frontend/src/**`, `frontend/utility/`. Bar: a crash, freeze, or silent failure
mid-broadcast is unrecoverable — the user is live in front of an audience.

### Critical

- ✅ **R1 — `Hotkeys.cpp:286-302`: Start/Stop Streaming hotkeys drive the engine off
  libobs's hotkey thread with no synchronization.** **Fixed `fc25f7c82`.** Both callbacks now
  hop through `AsyncTask::PostToUi` — the marshal `onOutputStopped`'s wiring already uses
  (`obs_bootstrap.cpp:915-921`) — with a `MultistreamAlive()` guard for the task `CefShutdown`
  drains after `Stop()` resets the engine. `PostToUi` drops tasks once `Bridge::Shutdown()`
  clears the alive flag, which runs before the engine dies. The five `obs_enum_hotkeys` lambdas
  and the `hotkeys.*` bridge methods were audited and left alone: they run on the caller's
  thread and touch only the hotkey API, which holds its own recursive mutex.
  `obs_hotkey_enable_callback_rerouting` was rejected — it needs a frontend-side pump nothing
  in this tree drains, and `PostToUi` is the existing seam. The comment audits `EmitEvent` and stops
  there, but `StartAllEnabled` iterates the unguarded `bindings.bindings` vector
  (`MultistreamEngine.cpp:426`) and `canvasEncoders` (`MultistreamEngine.hpp:184`) has no
  mutex. Stock OBS's Qt frontend called `obs_hotkey_enable_callback_rerouting` to marshal
  hotkeys onto the UI thread; the CEF port dropped it — **zero hits under `frontend/`**.
  *Scenario:* hotkey pressed while the Outputs tab adds a binding, `push_back` reallocs
  mid-iteration, crash or start against freed memory. *Fix:* marshal via
  `AsyncTask::PostToUi`, as `onOutputStopped` already does.
- ✅ **R2 — `MultistreamEngine.cpp:416-419`: `LiveOutput` destroyed while holding
  `liveMutex`, closing a deadlock cycle against the stop signal.** **Fixed `c163c4417`.**
  `RemoveLive` became `TakeLive`, returning the moved-out `unique_ptr` so the caller destroys
  it after the guard releases. **Four sites, not one** — `StopOutput` (the cited one), `StopAll`
  (`dead.swap(live)`; also covers `~MultistreamEngine`), and both reap paths in `StartOutput`.
  The cycle was re-confirmed against in-tree libobs (`callback/signal.c:298-321` holds
  `sig->mutex` across the callback, `:251-260` disconnect locks it, `obs-output.c:291-303`
  blocks) and is **not stop-specific** — `"start"`/`OnOutputStart` closes the same cycle.
  `obs_output_active`/`obs_output_get_*` under the lock are safe (atomic/field reads). The
  deferred `UpdateSleepInhibit`/`NotifyChanged` ordering is inert: both only observe `live`,
  already emptied atomically under the lock. The comment defends the
  *synchronous* re-entry and misses the *asynchronous* one. Edges verified: `OnOutputStop`
  takes `liveMutex` (`:648`); `signal.c:298-321` holds `sig->mutex` across the callback
  while `signal_handler_disconnect` (`:260`) locks it; `obs_output_destroy` blocks
  (`obs-output.c:301-303`). *Scenario:* output drops mid-stream, RTMP thread holds
  `sig->mutex` and waits `liveMutex`, UI thread holds `liveMutex` and `~OBSSignal` waits
  `sig->mutex`, **hard freeze while live, requires a kill**. Window widens on network stalls
  and on `StopAll` at quit. *Fix:* move the `unique_ptr` out under the lock, destroy after
  releasing.
- ✅ **R3 — `bridge.cpp:5206-5212` and `:7265-7269`: `canvas.remove` reaps only windowId 0's
  preview surface and never reaps projectors, giving a render-thread UAF.**
  **Fixed `f597e3409`.** One shared `RemoveCanvasMixAndConsumers()` at both sites: projectors
  (new `ProjectorManager::CloseForCanvas`, reusing the existing ordered `Close(id)` path), then
  every windowId's preview (new `PreviewManager::DestroyForCanvas`), then encoders + mix. The
  `Destroy(uuid)` overload is **gone** — caller census: two meant "this canvas everywhere" and
  were the bug, one (a self-test) was indifferent, **zero** legitimately meant windowId 0.
  **Ordering is load-bearing:** projectors must die first because they hold no `AddPreview`
  ref, so reaping previews first can drop the count to zero and let `ReconcileEntry` clear the
  video under a live projector display. **Multiview needs no separate reap** — it is a
  `ProjectorKind` carrying the canvas uuid, so `CloseForCanvas` covers it; its 500 ms timer
  and draw callback both die with `Close`. Ref-instead-of-close was rejected: `RemoveCanvas`
  frees the canvas regardless of preview refcount, so a ref would not even prevent the crash.
  `preview_window.hpp:196` resolves `Destroy(uuid)` to `Destroy(0, uuid)`, and the loop
  matches `it->windowId == windowId` (`preview_window.cpp:1768`), so a detached surface is
  skipped — `WindowManager::Detach` (`window_manager.cpp:68-126`) mints a new windowId on an
  ordinary detach click. `Projector::Instance()` never appears in the removal path
  (`bridge.cpp` 662/6883/6996/7015 only); `projector_window.cpp:996` borrows the canvas
  unref'd. *Scenario:* open a projector for idle canvas B, delete B, `CanvasIsLive(B)` is
  false so the gate passes, mix freed, Multiview's 500 ms timer derefs it, **process dies,
  taking canvas A's live stream with it**. *Fix:* a `DestroyForCanvas(uuid)` sweeping every
  windowId and closing matching projectors; the convenience overload is the real defect, two
  call sites already drifted onto it.
- ✅ **R4 — `obs_bootstrap.cpp:3365-3373` + `main.cpp:610-615`: browser sources destroyed
  *after* the last CEF pump, then handed to `CefShutdown()`.** **Fixed `285c33153`.** `Stop()`
  now takes `main.cpp`'s existing bounded `DrainCefTasks` pump (injected — the frontend keeps
  CEF ownership) and runs it after the source-removal sweep, before the store clears and
  `obs_shutdown()`. **Draining after `Stop()` returns was rejected:** `Stop()` runs
  `obs_shutdown()` itself, so the close cascade would execute against freed graphics and
  cleared stores. The `g_scene` claim was **confirmed** — its only assignment is
  `CreateDefaultScene()` (`:340`), whose only caller is under `if (!SceneCollection::Load())`
  (`:817`). Verified no double-free: `BrowserSource::Destroy()` frees its textures
  synchronously (covered by the pre-existing `obs_wait_for_destroy_queue` drain) and the
  destructor is CEF-only, touching no libobs object. `TeardownScene()`'s `g_scene` early-return
  is left as a separate wart — the sweep compensates and now drains correctly. `obs_bootstrap.hpp:12-16`
  states the invariant (browser sources "MUST be called while CEF is still pumpable" —
  `Destroy()` only *posts* `delete this` to TID_UI). But `TeardownScene()` early-returns on
  `!g_scene` (`:1000-1002`) and `g_scene` is set only on the `CreateDefaultScene()` fallback,
  so on **every run with a saved collection** the `main.cpp:610` drain drains nothing and the
  real sources die at `Stop()`'s `obs_enum_sources(removeCb)`. `CefDoMessageLoopWork` exists
  at one site (`main.cpp:321`), called only from 532 and 610, both before `Stop()` at 613,
  with `CefShutdown()` at 615. `obs_bootstrap.cpp:3360`'s own comment admits the sweep exists
  because `TeardownScene()` no-ops. **This is the "intermittent CEF-exit segfault"; the
  "teardown/GPU artifact, not a regression" dismissal is wrong** — it is an older latent
  defect, which is exactly why it was not a Phase 8 regression. *Fix:* pump `DrainCefTasks()`
  between the source-removal sweep and `CefShutdown()`.
- ✅ **R5 — `overlay_server.cpp:238` via `chat_hub.cpp:45-47`: chat SSE does blocking 3 s
  socket sends on the CEF UI thread, which obs-browser shares.** **Fixed `25ccd4514`.**
  `BroadcastChat` moved into `ctx.emit` on the chat transport worker, before the `PostToUi`
  hop — mirroring `EventHub::Ingest`. **Order is preserved:** `EventHub` broadcasts inline on
  the transport's single long-lived worker rather than spawning one per event, so per-account
  chat order is inherent; only cross-platform interleaving between the overlay and dock feeds
  changes, which was never guaranteed. **No queue was added** — the broadcast back-pressures
  that one platform's read loop via TCP, bounded by the 3 s send timeout, so there is nothing
  to overflow. The workers ride `AsyncTask::RunAsync` and so participate in `WaitForDrain`.
  `FrontendOwnsCef()` **confirmed** end to end (`obs_bootstrap.cpp:667` sets the env,
  `obs-browser-plugin.cpp:87/284` reads it and skips its own `CefInitialize`;
  `multi_threaded_message_loop = false` at `main.cpp:433`) — this froze the **stream**, not
  just the UI. `overlays.test` had the same defect and moved to the existing async lane. `chat_hub.cpp:42` states its
  own thread ("RouteEmit runs on the CEF UI thread") then calls `BroadcastChat`, which
  `SendAll`s with `kSseSendTimeoutMs = 3000`. `obs-browser-plugin.cpp:284-291`
  (`if (FrontendOwnsCef()) { ... return; }`) means **every browser source on stream renders
  on that same TID_UI**. *Scenario:* one overlay widget stops draining its socket, buffer
  fills, next chat line blocks TID_UI up to 3 s, **every browser source on air freezes on its
  last frame**; repeats during a raid. The events path does this correctly
  (`event_hub.cpp:259` broadcasts on a worker). *Fix:* hop to a worker before
  `BroadcastFrame`, mirroring `EventHub::Ingest`.
- ✅ **R6 — `main.cpp:390` + `libobs/util/base.c:57-66`: the crash handler has no sink.**
  **Fixed `2cb8822b2`.** `SessionLog::InstallCrashHandler()` at `main.cpp:391`, before the
  filter install, so the filter can never fire while the default sink is live. The sink writes
  `<config>/crashes/Crash <ts>.txt` reusing `session_log`'s own rotation/timestamp helpers,
  resolves its path at install time (no path work in a crashed process), guards re-entry, and
  `_exit(-1)`s. `Smoke-Package.ps1` now requires exit 0 *after* the `FE_SMOKE_QUIT_SECONDS`
  window and no crash report from the run. **One claim below was wrong:** `minidump_write_dump`
  is resolved at `libobs/obs-win-crash-handler.c:134` and referenced nowhere else — it is dead
  code **upstream too**, so the old frontend never wrote dumps either. `bcrash()` (`:518`)
  carries the full text report (header, all-thread stacks, module list); that is what the sink
  persists. Real minidumps would need a `libobs/` change — see G3.
  `obs_init_win32_crash_handler()` installs the exception *filter*, ending in `bcrash(...)`,
  but `base_set_crash_handler` has **zero callers tree-wide**, so the default runs:
  `vfprintf(stderr, ...); exit(0);`. `minidump_write_dump` is resolved
  (`obs-win-crash-handler.c:134`) and **never called**. Port regression:
  `frontend_old/obs-main.cpp:958` had `base_set_crash_handler(main_crash_handler, nullptr)`
  and wrote real dumps; `main.cpp` copied that file's privilege/mitigation/RTWQ setup and
  dropped only the sink. *Scenario:* crash 40 min into a stream in a `wWinMain` process with
  no console, output goes nowhere, bypasses `blog()` so SessionLog never sees it, and the
  process reports **success** to Windows, so `Smoke-Package.ps1:345` scores it
  `"process exited 0"` = **pass**. **Force multiplier: this is why R4 and the `80000003` left
  no inspectable artifact, and why the CI boot-smoke gate green-lights a crashed app.**
  *Fix:* port `main_crash_handler`, register before the filter, exit non-zero, and harden the
  smoke gate so a crashed process cannot pass.
- ✅ **R7 — `bridge.cpp:3512-3520`: `sceneItems.group` never dedupes `ids`, null-derefing
  libobs.** **Fixed `304091e14`.** Order-preserving `std::find` skip in the resolve loop, first
  occurrence wins. **Silent dedupe, not an error** — it follows the method's own established
  convention: non-integer entries, unresolvable ids, and group items are all silently skipped,
  and only an all-skipped array errors. The null-write chain was re-confirmed against in-tree
  libobs (`obs-scene.c:3541-3545` guard, `:3565-3569` per-element detach, `:294-305`
  `detach_sceneitem` nulls `parent` but leaves `prev`/`next`, `:299` the null write). **The
  finding understated it:** a duplicate that is *not* first also corrupts — the relink loop at
  `:3570-3582` self-links the item. The dedupe covers both. No seen-guard in the resolve loop; libobs's guard (`obs-scene.c:3541-3545`)
  rejects only foreign-parent/group items, so a duplicate pointer passes and
  `obs-scene.c:3565-3569` runs `detach_sceneitem` once per element. The second call finds
  `parent == NULL`, and when the item is the scene's first (`prev == NULL`) it takes
  `else -> item->parent->first_item` (`obs-scene.c:299`), a **null-pointer write**.
  *Scenario:* `sceneItems.group {scene:"Main", ids:[5,5]}` with item 5 first. Reachable from a
  renderer multi-select bug *and* from the MCP server, which proxies any registered method
  through `Bridge::Dispatch`. *Fix:* dedupe before `obs_scene_insert_group`.
- ✅ **R8 — `ws_client.cpp:93-101`: the WebSocket upgrade has no whole-request timeout.**
  **Fixed `bf46940c5`.** `kUpgradeTimeoutMs = 30s` via `CURLOPT_TIMEOUT_MS` — it must exceed
  the 15 s `CONNECTTIMEOUT` it subsumes, and matches `util/http_client.cpp:70`'s 30 s default.
  **Session-safety proven from curl source** (`lib/ws.c` @ `curl-8_12_1`, matching the vendored
  8.12.1-DEV header): `curl_ws_recv` consults no timeout at all and `curl_ws_send`'s non-raw
  path takes `Curl_senddata`; the file's only `Curl_timeleft` check is in
  `ws_send_raw_blocking`, unreachable here. So the bound covers the 101 handshake only.
  **All four transports share `Chat::WsClient`** — no private copies of the defect.
  `TwitchChat`'s lock-held connect is fixed by handshaking on a stack-local client and
  move-assigning under `wsMutex_` (new `WsClient::operator=(WsClient &&)`), so only the O(1)
  handoff is serialized; dropping the lock outright would have raced `sendText` against
  `connect`'s handle mutation. `twitch_events.cpp:277`/`kick_events.cpp:251` also connect under
  their mutex but are single-worker with no contender, so the timeout alone suffices there.
  `CONNECTTIMEOUT 15L` is set; `CURLOPT_TIMEOUT` is not (it appears only at
  `http_client.cpp:71`). With `CONNECT_ONLY=2L` the HTTP 101 happens in the *perform* phase,
  governed by `CURLOPT_TIMEOUT` (default 0 = unlimited). `disconnect()` only flips an atomic
  and cannot break it. *Scenario:* Twitch IRC drops mid-stream, reconnect calls `ws_.connect`
  **while holding `wsMutex_`**, a captive portal completes TLS but never sends 101, chat is
  dead for the rest of the stream and the detached worker never unwinds so `WaitForDrain(5s)`
  always times out at quit. *Fix:* set `CURLOPT_TIMEOUT_MS`; move `connect` out from under the
  mutex.

### Important

- ✅ **R9 — `CanvasService.cpp:140-147`: `ResetVideo`'s bool is discarded.**
  **Fixed `558c4bc93`.** The non-Default reset now runs against a **scratch def before the
  commit**, mirroring the Default path (`bridge.cpp:462-505` via `applyGlobalVideo`), and fails
  the request rather than persisting a resolution the mix never took. The global refusal was
  re-confirmed (`obs-canvas.c:443-450` gates on `obs_video_active()`, a global counter).
  `CanvasRuntime::ResetVideo` now returns true for an **inactive** canvas (no mix; it builds
  fresh from the def on activation) and false only on a real reset failure. One pre-existing
  edge left alone: if `obs_canvas_reset_video_internal` fails *after* clearing the old mix
  (`obs-canvas.c:338-354`), the canvas is left mix-less — the def now at least stays consistent
  and the error surfaces.
  `obs-canvas.c:443-447` refuses the reset **globally** while `obs_video_active()`, and the
  default encoder is software `obs_x264`, so *any* canvas streaming makes it a silent no-op
  for *every* canvas. `canvases.json` persists the new resolution, the UI reports success, the
  mix keeps the old one. The Default path (`bridge.cpp:497-505`) rolls back correctly; the
  non-Default path does not.
- ✅ **R10 — `MultistreamEngine.cpp:326-327`: only `"start"`/`"stop"` are connected.**
  `obs-output.c:3026-3030` **suppresses `"stop"`** on a reconnectable drop and fires
  `"reconnect"`, which is never connected. UI shows green "live" for up to 25 x 10 s, roughly
  **250 s**, while nothing reaches the platform. **Fixed `558c4bc93`** (host) + `767c87652`
  (web). Both `"reconnect"` and `"reconnect_success"` are connected; the latter matters because
  libobs fires it **instead of `"start"`** on resume (`obs-output.c:2667-2672, 2792-2801`).
  Exhausted retries fire a real `"stop"` with `OBS_OUTPUT_DISCONNECTED` (`:2963-2969`), so the
  existing `OnOutputStop`→`Error` path already covers terminal failure. New `State::Reconnecting`
  (appended, so `StateName` indices stay stable) **counts as active** in `IsActiveState` — libobs
  keeps `obs_output_active()` true and the encoders attached across the window, so treating it
  as Idle would reopen the UAF the live gate prevents. **The "~250 s" was the floor:** those are
  Braidcast's `AdvancedSettings` defaults (25 x 10 s), not libobs's (20 x 2 s), and exponential
  backoff (x1.3, capped 15 min) makes the real window longer. Web side ranks it
  `live > reconnecting > connecting > error > idle`, colors it from mixed red/yellow meter
  tokens (connecting is already amber), and both `live || connecting` gates now call one shared
  `isActiveState` predicate. Verified: `bun run check` 311 files, 0 errors, 0 warnings.
- ✅ **R11 — `CanvasService.cpp:78-79`: the live-edit gate checks only the edited canvas.**
  **Fixed `558c4bc93`.** The gate calls `MultistreamEngine::IsCanvasOrInheritorLive`, which
  follows inheritance only when the edited canvas is the Default (nothing inherits from a
  non-Default). Inheritance is **three-dimensional**, and the composite is now named once as
  `CanvasDefinition::InheritsAnyDefault()` on the entity that owns it: encoder slots
  (`CanvasEncoderDef::InheritsDefault()`), resolution/fps (`useDefaultResolution`), and color
  (`color.useDefault`). `InvalidateCanvasEncoders` (which lives in `MultistreamEngine:171-192`,
  **not** `CanvasService` as CLAUDE.md phrases it) was deliberately **not** widened to the
  composite predicate — its encoder-only scope is correct for an encoder cache. The gate is
  intentionally broad: any inheritance dimension refuses any structural edit, costing a
  conservative refusal of a Default encoder edit while a color-only inheritor is live.
  Inheriting canvases take resolution/encoders from the Default
  (`MultistreamEngine.cpp:146-147`), so editing the idle Default while an inheritor is live
  passes the gate, giving divergence plus a duplicate encoder pair on a live canvas (breaks
  encode-once). No UAF.
- ✅ **R12 — `broker_strategy.cpp:339`: unconditional `Accounts().Put` resurrects an account
  disconnected mid-refresh**, re-persisting deleted credentials. Every sibling writer refuses
  this (`account_store.cpp:245`: "never re-insert a removed account"). **Fixed `14be488dd`.**
  New `AccountStore::UpdateExisting()` reuses the exact lock+`find`+return-if-absent guard the
  sibling writers use; `broker_strategy.cpp` calls it instead of `Put`. A disconnect in flight
  during a refresh stays removed; the refresh still succeeds for the in-flight caller.
- ✅ **R13 — `mcp/HttpServer.cpp:220`: no `SO_RCVTIMEO`, no client-socket registry**, so
  closing the listen socket will not unblock a parked `recv` and `Stop()`'s `join()` hangs
  forever, on the UI thread, mid-stream, on an MCP toggle. `overlay_server.cpp:93,466` already
  implements the correct seam — reuse it. **Fixed `35c1011ec`.** Call path confirmed:
  `mcp.setConfig` (UI thread) → `McpServer::ApplyConfigPatch` → `RestartListener` →
  `HttpServer::Stop()`. `HttpServer` handles one connection at a time, so a single tracked
  `clientSocket_` (not a `std::set`) suffices: `Stop()` now `shutdown(SD_BOTH)`s the parked
  socket, with `kHeaderRecvTimeoutMs = 10000` (same as overlay_server) as the standalone
  backstop.
- ✅ **R14 — `event_names.hpp`: no transport health channel exists.** Only
  `events.new`/`events.backfill`; every transport death terminated at `HostLog`, so a dead
  transport still showed a green badge — the amplifier that made most failures above invisible.
  **Fixed `e62272006` (with G1).** New `TransportHealth` aggregator
  (`events/transport_health.*`) holds a mutex-guarded per-transport state map
  (connecting/connected/reconnecting/failed/disconnected + last error), reports at each transport's
  existing state seams (one shared `EmitChatState` helper covers all three chat platforms; the
  event hubs + overlay lifecycle report at connect/fail/stop), and emits the bridge
  `transports.healthChanged` event + `transports.health` snapshot method — mirroring the
  `multistream.status`/`multistream.changed` pattern at every layer. Notifies outside the lock so
  the TID_UI emit can't re-enter the mutex; a function-local singleton like the hubs (detached
  workers outlive `Stop()` and would UAF a reset instance). Web `transportHealthStore` mirrors
  `multistreamStatusStore`. **Data only — no health widget wired in** (see G1 note).
- ✅ **R15 — `overlay_server.cpp:286` (`BroadcastTo`) holds `sseMutex_` across blocking
  sends** — the exact hazard `BroadcastFrame`'s comment exists to avoid. The fix landed in one
  sibling, not the other. **Fixed `25ccd4514`** alongside R5. `BroadcastFrame` gained an
  optional widget filter and `BroadcastTo` delegates to it, deleting the duplicated loop.
  **Two more sites the finding missed**, both swept: the SSE handshake sent under the lock
  (now reserves an `ssePending_` capacity slot and sends unlocked) and the keepalive ping —
  which sent under the lock to the *quietest* socket, i.e. the one likeliest to be wedged, for
  up to 3 s every 15 s per stuck client. Safe because the sending thread owns the fd and the
  prune path only `shutdown()`s, never `closesocket()`s.
- ✅ **R16 — `scene_collections.cpp:146-165` (real path `frontend/src/scene/`): a doubly-corrupt
  index refuses to reseed (correctly) but nothing rebuilds from the intact `scenes/*.json` on
  disk**, so the user boots to a blank Default scene with their collections stranded, signalled
  only by a log line. **Fixed `14be488dd`.** New `SceneCollections::RebuildFromScenes()`, wired
  in `obs_bootstrap` after `Load()` and gated on the existing `IndexWasCorrupt()`. Scans
  `scenes/*.json` (excluding the `.output_bindings`/`.scene_links` siblings), validates each via
  `obs_data_create_from_json_file_safe(...,"bak")`, skips individually-corrupt files, **renames**
  the corrupt index to `.corrupt` (never deletes), and rebuilds. **Honest degradation:** the
  display `name` and `active` pointer are not stored in a scene file, so names fall back to the
  filename slug and active to the first found — stated in the log. (Scene-file byte-shape was
  inferred from `scene_persistence.cpp`, not directly observed; the `_safe` validate covers a
  wrong guess.)
- ✅ **R17 — fire-and-forget saves.** `SaveJsonAtomic` (`StorePaths.cpp:56-63`) is genuinely
  atomic (temp+bak+rename) and returns `bool`; **every caller discards it**. Disk-full
  mid-session loses a whole session's edits silently. **Fixed `14be488dd`.** One shared
  `ReportSaveResult(bool, path)` in `StorePaths` logs `[storage] failed to save <path>` and
  forwards the bool; **11 stores + `event_store`/`McpServer`/`overlay_store`** route their
  `Save()`/`Persist()` (now `void`→`bool`) through it. **Bounded follow-up (open):** ~25
  single-mutation bridge handlers (`canvas.update`, `streams.*`, …) still answer `{ok:true}` but
  now log on failure — surfacing those to the caller is a separate pass, not silently skipped.
- ✅ **R18 — `main.cpp`: the `CreateBrowserSync` abort path is a hand-copied partial
  duplicate of the real teardown** — leaks a ghost tray icon, skips RTWQ/mutex cleanup. **Fixed
  `cd4a83593`.** Extracted the clean-exit sequence into one `Teardown()` both the clean path and
  the abort call, so teardown order lives in one place; each step keeps its existing init guard
  so it is safe at the abort's stage. Also closed the leaked shared-Client ref, preview/projector/
  interact HWNDs, and the Stop()-before-surface-destruction ordering hazard (Stop() frees mixes
  those surfaces render). *Follow-up (minor, open):* the earlier `!host` / `!Start()` bailouts
  still do bare `CefShutdown(); return 1;` and leak mutex+RTWQ — unifying those needs init-stage
  flags, out of R18's scope.
- ✅ **R19 — unbounded buffers.** `ws_client.cpp:178` (`accum_` uncapped) and
  `http_client.cpp:20` + `:79` (no `CURLOPT_MAXFILESIZE`, with gzip decompression, against
  third-party emote hosts on the go-live path). OOM lands in the encoders' address space.
  **Fixed `35c1011ec`.** WS reassembly capped at `kMaxAccumBytes = 4 MB` → existing
  disconnect/reconnect path. HTTP capped at `kMaxResponseBytes = 20 MB`, enforced **both** via
  `CURLOPT_MAXFILESIZE_LARGE` (honest `Content-Length`) **and** in `WriteToString` against the
  actual decompressed bytes (a chunked/gzip-bombed response can omit or lie about
  `Content-Length`). Gzip auto-decompress confirmed on; the attacker-influenced path is
  `third_party_emotes.cpp:43` (7TV/BTTV/FFZ-style hosts) at go-live.
- ✅ **R20 — `bridge.cpp`: the file's only raw `std::thread(...).detach()`**, invisible to
  `WaitForDrain`. **Fixed `edf8d4a46`.** The self-heal stream-credential worker
  (`SelfHealStreamCredentials` → `SelfHealFetchWorker`) did blocking network then wrote the
  profile store / emitted a bridge event on completion. Routed through `AsyncTask::RunAsync` (the
  seam every other registered worker uses) so `WaitForDrain` counts it; its `PostToUi` writeback
  already drops after `SetAlive(false)`, so a late completion no-ops. No new registry, no deadlock
  (the only worker→UI wait self-times-out).
- ✅ **R21 — `bridge.cpp:7176-7327`: `settings.restore` discards six setters' errors and
  returns `{ok:true}`** — a Cancel that silently does not revert. **Fixed `14be488dd`.** Partial
  application is unavoidable (video/audio/canvas setters commit to libobs+disk before later
  sections run, no rollback), so it now aggregates per-section failures across **all 8**
  restorable sections (video, audio, globalDevices, streamProfiles, outputBindings, canvases,
  hotkeys, mcp) and returns `{ok:false, failed:[{section,error}]}`, the soft-failure shape
  `multistream.startOutput` already uses. **`frontend/web` follow-up — resolved `2b0a2eccf`:**
  `settings.restore` has **zero** live web callers — the Cancel/revert footer it backed was
  intentionally dropped for the live-apply page model (kept server-side only for the headless
  self-test), so nothing on the web assumes success. Added just the typed `{ok, failed?[]}`
  contract entry to `bridge.ts` so a future caller / the self-test path is compile-time safe
  against the real shape; did not resurrect the dropped revert UI (would be a design pivot).

### Pass 1 addendum — surfaced while fixing (2026-07-16)

Found by the R3 implementation agent while reading the projector lifetime model. Same family
as R3 (a borrowed canvas mix freed under a live consumer), different trigger, so they survive
R3's fix. Both are report-only so far.

- ✅ **R22 — a projector's borrowed mix can be freed by `ReconcileEntry` without any canvas
  removal.** Canvas/Multiview projectors held **no** `AddPreview` ref (only the Program kind).
  Disabling a canvas's last enabled binding, or closing its last preview, drove `ReconcileEntry`
  to `obs_canvas_clear_video` while an open projector still drew that mix. **Fixed `7b11d91e6`.**
  Canvas/Multiview projectors now `AddPreview`/`RemovePreview` on their target uuid — a
  first-class mix consumer instead of a silent borrower — balanced 1:1 across every close path
  (normal close, `CloseForCanvas`, app-quit `DestroyAll`, dtor, open-failure), released after
  `obs_display_destroy` so no draw callback can reach a freed mix.
- ✅ **R23 — `CanvasIsLive` does not cover the virtual camera.** `VirtualCamManager` holds an
  `AddPreview` ref on its target canvas, but `IsCanvasLive` knew only outputs. **Confirmed and
  fixed `7b11d91e6`.** Traced: `canvas.remove` (and the revert reap) gate on `IsCanvasLive`,
  which missed the vcam, so removal ran the free chain (`obs_canvas_remove`→`release`→`destroy`→
  `clear_mix`) under the vcam's live `video_t` → render-thread UAF. Fixed by teaching
  `IsCanvasLive` / `IsCanvasOrInheritorLive` about the vcam via a new
  `VirtualCamManager::FeedsCanvas` (reuses `heldCanvas_`, no duplicate lookup). The structural-edit
  path was already safe — `obs_canvas_reset_video` is guarded by `obs_video_active()`, which
  scans canvas mixes too; only *removal* was unguarded.
- 🔴 **R24 — `OutputStat.state` is typed Title-cased but the host sends lowercase, so Stats and
  Monitor have never worked.** Found by the R10 web agent, **confirmed on both sides**:
  `bridge.cpp:5986` sends `MultistreamEngine::StateName(s.state)` — the *same* lowercase
  `StateName` the multistream path uses — while `bridge.ts:819` declares
  `"Idle" | "Connecting" | "Live" | "Error"` and its doc comment at `:813-814` explicitly (and
  wrongly) claims the stats bridge title-cases. Consequences, all dead code today:
  `OUTPUT_STATE_COLOR` never matches so every Stats/Monitor dot is unstyled
  (`StatsDock.svelte:247`, `MonitorPage.svelte:204,207`); `StatsDock.svelte:122`/`:123` mean the
  **live and error counters are permanently 0**; `:248`'s click-to-open **error detail is
  unreachable**; `MonitorPage.svelte:201` likewise. Pre-existing, not caused by R10. *Fix:*
  retype `state: MultistreamState`, collapse `OUTPUT_STATE_COLOR` into `STATE_COLOR` (it becomes
  an exact duplicate), migrate the four comparisons — ~10 lines, and Stats/Monitor get the
  reconnecting color for free.

### Gaps vs the premium bar

- ✅ **G1 — no health channel** for events/chat/overlay. Highest-leverage addition; converts most
  Importants above from silent to visible. Same root as R14. **Backend + data path shipped
  `e62272006`** (see R14); **visual surface wired `704ced03f`.** `MultichatDock`'s per-platform
  chips now key off `transportHealthStore` instead of the binary `chat.state`: reconnecting/failed
  transports stay visible and colored (reusing the `STATE_COLOR` tokens via a
  `TRANSPORT_STATE_COLOR` remap, no new palette) with the last error on hover, via opt-in
  `dotColorOf`/`titleOf`/`disabledOf` props on the shared `PlatformChips` (EventsDock unchanged).
  The now-redundant `chat.state` listener + its teardown-workaround re-snapshot were removed.
  **Optional future consumers (not built):** events-transport (`events:<id>`) and overlay
  (`"overlay"`) health have no UI row yet.
- 🟡 **G2 — no recovery anywhere.** `client.cpp:184-192` `OnRenderProcessTerminated` comments
  out *every* parameter (`status`, `error_code`, `error_string`) and never reloads. A dead
  renderer mid-stream leaves a permanently blank UI while the stream runs blind. **Partially
  addressed `73e881a42`:** the GPU-subprocess crash-loop (the blank-screen case, the resolved
  `80000003`) now auto-recovers — a boot that never paints trips a persistent software-rendering
  fallback next launch. Still open: mid-stream **render-process** death (`OnRenderProcessTerminated`)
  is not yet reloaded/recovered.
- 🟡 **G3 — no crash coverage for CEF subprocesses.** `main.cpp:345-348` returns before the
  filter installs; `no_sandbox = true` with no crashpad config. **Partially addressed
  `73e881a42`:** the GPU subprocess's crash is now *detected* (via the boot sentinel) and
  routed to software-mode fallback, so it no longer silently blanks the app. Still open: no
  minidump/crashpad for CEF subprocesses (a true crash report, vs. the detect-and-degrade the
  sentinel gives), and the render/GPU subprocesses still run with no filter.
- ✅ **G4 — no jitter in `Backoff::next()`** (`ws_client.cpp`); all WS transports walked an
  identical ladder in lockstep. **Fixed `5fc5c72e9`.** Equal jitter applied after the existing
  clamp — delay lands in `[floor(ms/2), ms]`, keeping a sane floor while de-correlating retries;
  per-`Backoff` `std::mt19937` seeded once from `random_device` (member, so each per-thread
  transport has its own with no lock/race).
- ✅ **G5 — no watchdogs, no `State::Connecting` deadline.** **Resolved (2026-07-16) — both halves
  already bounded, no change needed.** The chat/events WS path (`ws_client.cpp`) is deadline-bounded
  — synchronous `connect()` under `CURLOPT_CONNECTTIMEOUT=15` + `CURLOPT_TIMEOUT_MS=30000`, covering
  the "TLS up, no HTTP 101" stall. The `MultistreamEngine` `State::Connecting` is signal-driven and
  bounded by librtmp: happy-eyeballs 25 s TCP connect + `SO_RCVTIMEO` 30 s + `SO_SNDTIMEO` 15 s →
  `connect_thread` always fires `stop`/`reconnect`, so a binding leaves `Connecting` in ≤~30 s
  (`CONNECT_FAILED` on a first attempt routes through `end_data_capture`→`signal_stop`→`Error`). A
  redundant engine watchdog was rejected as a net negative: it would race the connect-thread
  teardown (the UAF class R22/R23 fix) and false-kill slow-but-valid connects. Unaudited (flagged):
  an adversarial dribble-a-byte-per-30 s server, and non-RTMP output types (HLS/SRT/RIST/WHIP) which
  carry their own connect timeouts. Self-tests still run ~20 synchronous tests on the CEF UI thread.
- ✅ **G6 — silent startup degradation.** `LoadCuratedModules` logged `init-failed`/`open-failed`
  through `HostLog` (LOG_INFO), indistinguishable from routine startup chatter, so a failed
  `obs-x264` looked healthy until Go Live. **Fixed `0d43a5f96`.** The two failure paths and their
  per-category summaries now log at `LOG_WARNING`, so a warning scan catches which module did not
  load and why; log-and-continue behavior unchanged (no required-module concept in the list).

### Open question — RESOLVED (2026-07-16)

- ✅ **`Unhandled exception: 80000003` — it is the CEF GPU subprocess crash-looping at
  startup, and it is what produces the blank-white-screen "app won't load."** Confirmed on a
  live machine (RTX 5070 Ti): the CEF `debug.log` shows `GPU process exited unexpectedly:
  exit_code=-2147483645` (= `0x80000003`, EXCEPTION_BREAKPOINT) within ~6 s of launch, in a
  loop (`GpuProcessHost` reinitializes it, it crashes again). The mechanism the open question
  guessed was right — a `libcef.dll` `CHECK()` / `IMMEDIATE_CRASH()` = `__debugbreak()` on
  MSVC — but the **location was wrong**: it is the **GPU subprocess at startup**, not the main
  process at `CefShutdown()` (R4 is a genuine but separate main-process bug). A GPU newer than
  CEF 6533/Chromium 127 makes the GPU process reject the driver. When it dies the renderer
  never composites: the session log shows `[cef] browser created` then **zero** web activity
  (no page-load, no bridge calls) then a clean libobs shutdown — the backend loads every canvas
  fine, but the Svelte UI never paints → blank white window that reads as "no data / fresh
  start." **Verified fix:** launching with `--disable-gpu` eliminates the crash and the UI
  loads fully (SwiftShader compositing). Shipped as an automatic, persistent safe-mode fallback
  in `73e881a42` (see G2/G3). Manual escape hatch for an install whose UI never comes up:
  create `braidcast_disable_gpu.txt` beside `braidcast.exe`, or launch with `--disable-gpu`.

### Verified solid (do not re-raise)

`interact_window.cpp` (hard source ref + `remove`-signal to `WM_CLOSE` deferral +
disconnect-before-release). The **preview gate refcount is balanced** — the historical
imbalance is genuinely fixed; the lifetime bugs migrated to consumers the gate does not cover.
The recorded `streamMeta` UAF is properly fixed with **no siblings on the async lane** (all 5
async methods hit mutex-guarded or marshaled state; the hotkey lane in R1 is a *different* lane
the fix never reached). Bridge callback resolution and exception barriers are airtight, with no
hung-promise path. Atomic writes plus `.bak` recovery are real end-to-end. `Stop()`'s ordering
rationale, obs refcounting across ~40 resolve/release methods, OAuth single-flight refresh,
Multiview's snapshot design, and `event_store`'s bounded 500-cap concurrency all check out. All
three servers bind `INADDR_LOOPBACK`, never `INADDR_ANY`. No client secret ships (`obf.h`
obfuscates a public `client_id`; the broker injects secrets server-side).
