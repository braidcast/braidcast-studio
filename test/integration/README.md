# Braidcast headless backend integration tests

Test Braidcast's backend logic **without opening the app** — no Qt, no CEF, no
window. This is the first vertical slice: a cmocka test binary that boots libobs
headless and drives the **real** persistence code path (`SceneCollection::Save/
Load` + the multistream stores) to regression-guard the persistence fixes.

## What this slice covers (fast tier)

`test_persistence` — four round-trip tests, each of which fails if its guarded fix
is reverted:

| Test | Guards | Fails-on-revert because |
| --- | --- | --- |
| `test_scene_order_roundtrip` | `scene_persistence.cpp` `scene_order` array | Without it, reload falls back to creation order `[A,B,C]`; the test asserts the reordered `[C,A,B]`. |
| `test_scene_order_legacy_missing_key` | Load's absent-`scene_order` reconcile | Strips `scene_order` from the on-disk file; asserts no scene vanishes and order self-heals to creation order. |
| `test_audio_state_roundtrip` | scene-source mute/volume save **+** `SaveFilter` channel 1–6 exclusion | Asserts a scene source's mute/volume survive Save/Load, and that a source bound to global audio channel 1 is **not** written into the collection (the exclusion `PersistSourceState` pairs with). |
| `test_additional_canvas_item_roundtrip` | `SaveFilter` keeping additional-canvas sources + the dropped `if (!isAdditional)` save-skip | Creates a non-default canvas + a scene with a hidden item; reverting the filter change means the canvas scene isn't saved, so Load seeds a fresh empty scene and the `CScene`/item assertions fail. |

The tests call the production `SceneCollection::Save/Load`, the real `CanvasStore`
/ `CanvasRuntime`, and libobs `obs_save_sources_filtered`/`obs_load_sources` — not
a test-only reimplementation — so they would have caught the bugs the fixes closed.

## Why the store layer, not the bridge (CEF entanglement)

The Braidcast frontend is a single monolithic executable: `bridge.cpp` (the
JSON-RPC `Bridge::Dispatch`) compiles CEF headers and links CEF, and there is no
intermediate library. Linking `Bridge::Dispatch` into a headless test binary would
drag in the whole CEF app. So slice 1 tests **one layer below the bridge** — the
persistence stores + libobs directly, which is exactly where the bugs were.

The persistence TUs (`scene_persistence.cpp`, `CanvasRuntime.cpp`, `CanvasStore.cpp`,
`CanvasDefinition.cpp`, `StorePaths.cpp`) reference **no** Bridge/CEF symbols. They
reach their collaborators through four `ObsBootstrap::`/`Transitions::` accessors
that live in `obs_bootstrap.cpp` (which pulls the CEF app). `bootstrap_shim.cpp`
provides test-owned definitions of exactly those four symbols, each forwarding to
harness-owned instances (`testseam.hpp`). This is a dependency-injection **seam**,
not a save/load reimplementation — the production Save/Load logic runs unchanged.

## Graphics requirement (not fully GPU-free)

The pure `obs_data`/`obs_save_sources` serialization is context-free, **but** the
real `SceneCollection::Save/Load` pull in `obs_get_main_canvas` and the
`CanvasRuntime` `obs_canvas` mixes, which require `obs_reset_video` to have run. The
harness therefore stands up a **D3D11** pipeline. D3D11 **WARP** (the software
rasterizer) satisfies this on a headless CI box with **no physical GPU**, so the
tier is CI-friendly — but it is not "no graphics context". No network, no CEF.

## Config isolation

libobs `os_get_config_path` resolves via `SHGetFolderPath(CSIDL_APPDATA)`, which
ignores env overrides — so stores that only expose `FilePath()`-based saves
(`CanvasStore::Save`, `GlobalAudioChannels::Persist`) cannot be redirected by env.
The harness therefore isolates via the **explicit-path overloads**
`SceneCollection::Save(path)/Load(path)`, pointing each test at a fresh per-test
temp dir (`%TEMP%/braidcast_it_<pid>_<n>/collection.json`), and never invokes the
`FilePath()`-based savers. As defense-in-depth, the shim's
`SceneCollections::ActiveScenePath()` also returns the temp path, so even a no-arg
`Save()`/`Load()` stays isolated. Nothing touches the user's real profile.

## Build & run

Not built by default. Enable the option (default OFF):

```
cmake --preset windows-x64 -DENABLE_INTEGRATION_TESTS=ON
cmake --build --preset windows-x64 --config RelWithDebInfo --target test_persistence
```

The binary is placed in the OBS rundir bin dir (`…/bin/64bit`) so it finds
`libobs-d3d11.dll` and `data/libobs` at runtime. Run it directly, or via CTest:

```
ctest --test-dir build_x64 -C RelWithDebInfo -R test_persistence --output-on-failure
```

## Tiered plan

This slice is tier 1. The remaining tiers, in build order:

### 1. Fast tier — THIS SLICE
No GPU-hardware / no network / no CEF (D3D11 WARP). Store + libobs persistence
round-trips. Runs in the rundir; milliseconds per test.

### 2. Bridge-level tier — JSON dispatch (next)
Drive the **real** `Bridge::Dispatch(method, jsonParams, result, error)`
synchronously (it runs single-threaded on TID_UI; no CEF message loop is needed
to call it) so tests exercise handlers like `audio.setMuted`, `sceneItems.
setVisible`, `scenes.reorder` end-to-end — i.e. the save **triggers**
(`PersistSourceState`/`CommitSceneItemChange`) this slice can't reach.
**Needs CEF decoupling of `bridge.cpp`:** today it constructs CEF `CefRefPtr`
responses and includes CEF headers directly. The concrete decoupling: extract a
**bridge response sink interface** (e.g. `IBridgeResponder` with `resolve(json)` /
`reject(error)`) that the CEF `CefV8`/`CefMessageRouter` path implements in the
app, and that the test implements as a plain capturing stub. `Bridge::Dispatch`
would take the sink instead of a `CefRefPtr` callback, and the CEF-only helpers
(`client.hpp`, V8 marshalling) would move out of the dispatch TU. Once dispatch no
longer includes CEF headers, the bridge TU links into this harness the same way
the store TUs do, and the shim graduates from 4 seams to a fuller `ObsBootstrap`
(the engine/audio-monitor accessors the handlers touch).

### 3. Go-live preflight tier
Bind a destination with a **missing** stream key and assert the named-reason
refusal fires (e.g. "Kick has no stream key") **before** any output starts — no
real streaming. Needs: the bridge tier (to drive `multistream.startOutput`), an
`OutputBindingStore` + `StreamProfileStore` seeded in memory with a keyless
profile, and the engine's preflight gate reachable without a live encoder (assert
the refusal, never actually `StartOutput`).

### 4. Mock-broker login tier
Point the OAuth broker base URL at a **fake** `auth.braidcast.com` (the
`BRAIDCAST_BROKER_URL` build var / env already exists) that returns canned
token/error responses, and test the token store refresh/expiry + the
provider-capability preflight (e.g. "Kick has no stream key") **without real
provider consent**. Needs: a tiny in-process HTTP stub bound to loopback, the
env-pointed base URL, and the DPAPI token store exercised against canned payloads.
No real network egress, no real accounts.

### 5. Opt-in live tier
Real GPU, a **local RTMP sink** (e.g. a loopback `nginx-rtmp`/`mediamtx`), and a
throwaway test-bot account. Actually encode + push one canvas and assert the
output goes live and stops cleanly. Opt-in only (real hardware + a real, if local,
network sink); never in the default CI lane.

## Files

- `harness.{hpp,cpp}` — boots libobs once (startup + D3D11 + audio), per-test temp
  dir + clean world, scene/canvas/item helpers, Save/Load/teardown+reload.
- `testseam.hpp` — the DI seam contract between the harness and the shim.
- `bootstrap_shim.cpp` — test-owned `ObsBootstrap::`/`Transitions::`/
  `SceneCollections::ActiveScenePath` definitions forwarding to the harness.
- `test_persistence.cpp` — the four cmocka round-trip tests.
- `CMakeLists.txt` — compiles the curated persistence TUs + the harness; links
  `OBS::libobs` + cmocka.
