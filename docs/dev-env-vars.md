# Dev & Diagnostic Environment Variables

Reference for the environment variables that tune Braidcast's debug/diagnostics,
self-test/bench, and dev-config behavior. All are **off / unset by default** — a
shipped build behaves as if none are present.

Precedence for the debug scheme (`BRAIDCAST_DEBUG` and `BRAIDCAST_DEBUG_COMPONENTS`):
**process env var** wins, then the gitignored repo-root `.env` file (dev builds only;
its path is baked at configure time as `BRAIDCAST_ENV_FILE`), then persisted
`DiagnosticsSettings`, then off. Edit `.env` and relaunch to flip logging without a
rebuild or a shell env var.

## Debug / diagnostics

| Variable | Values / example | What it does | Default |
| --- | --- | --- | --- |
| `BRAIDCAST_DEBUG` | `1` `true` `on` `yes` = on; `0` `false` `off` `no` / empty / unset = off (case-insensitive) | Pure-boolean master switch for the gated `DBG()` log channel. Off ⇒ nothing logs and `DBG()` calls are free. On with no components ⇒ the default category set. | off |
| `BRAIDCAST_DEBUG_COMPONENTS` | comma/space-separated names, e.g. `preview,bridge` · `all` · `all,render` · `render,gpudiag` | Selects which components are active. **Only consulted when `BRAIDCAST_DEBUG` is on.** Accumulative; unknown tokens are ignored. Empty/unset ⇒ the default category set. | default set |
| `BRAIDCAST_GPUDIAG_INTERVAL_MS` | integer ms, min `250`, e.g. `1000` | Sample interval for the `gpudiag` sampler. Only meaningful when the `gpudiag` component is active. | `5000` |
| `BRAIDCAST_DISABLE_BROWSER_SOURCES` | any non-empty value except `0` = on | A/B kill switch: neutralizes every obs-browser OSR source (blank `about:blank` 16×16 page) so a run carries no browser-source GPU load. Non-persistent (the teardown scene-collection save is skipped). Standalone var — **not** a debug component. | off |

### `BRAIDCAST_DEBUG_COMPONENTS` vocabulary

- **Log categories** (their wire names): `lifecycle` `bridge` `preview` `canvas`
  `stream` `encode` `audio` `oauth` `chat` `events` `overlay` `import` `scene`
  `mcp` `net` `cef` `render`.
- **`all`** — expands to the default category set (every category **except**
  `render`).
- **`render`** — opt-in only. It is the per-frame render-thread timing firehose
  (`obs_set_render_debug`, tagged `[render-debug]`); it is excluded from the
  default set **and** from `all`. Enable it explicitly, e.g.
  `BRAIDCAST_DEBUG_COMPONENTS=all,render` or `=render`.
- **`gpudiag`** — a non-category subsystem, not a log category. Turns on the
  periodic GPU-load sampler thread (see `frontend/src/diag/gpu_diag.hpp`). Tune
  its cadence with `BRAIDCAST_GPUDIAG_INTERVAL_MS`.

Examples:

| `BRAIDCAST_DEBUG` | `BRAIDCAST_DEBUG_COMPONENTS` | Result |
| --- | --- | --- |
| `1` | *(unset)* | default category set, no gpudiag |
| `1` | `all` | default category set, no gpudiag |
| `1` | `all,render` | default set **plus** the render firehose |
| `1` | `preview,bridge` | only the preview + bridge categories |
| `1` | `render,gpudiag` | only the render category + the gpudiag sampler |
| `0` | *(anything)* | fully off (components ignored) |

## Self-test / bench

| Variable | Values / example | What it does | Default |
| --- | --- | --- | --- |
| `FE_SMOKE_QUIT_SECONDS` | integer seconds, e.g. `20` | Headless smoke path: auto-terminates the app after N seconds so log capture is automatable; runs the `Run*SelfTest` battery. | unset (no auto-quit) |
| `BRAIDCAST_SELFTEST_STREAM` | mode string, e.g. `perf-repro` | Arms a targeted self-test state machine (currently `perf-repro`, the background power-throttling regression gate). | unset |
| `BRAIDCAST_SELFTEST_DURATION` | integer seconds, e.g. `8` | perf-repro background-measurement window length. | `8` |
| `BRAIDCAST_SELFTEST_MAXLAG` | percent (float), e.g. `2.0` | perf-repro max allowed render-lag percent before the run fails. | `2.0` |

## Dev config

| Variable | Values / example | What it does | Default |
| --- | --- | --- | --- |
| `FE_DEV_URL` | URL, e.g. `http://localhost:5173` | Points the CEF window at a live Vite dev server instead of the offline `app://` bundle. | unset (serve `app://app/`) |
| `BRAIDCAST_ENV_FILE` | absolute path (baked at configure time) | Path to the gitignored repo-root `.env` read as a fallback for the debug scheme. Baked in dev builds; absent in CI/shipped builds. | unset in CI/shipped |
| `BRAIDCAST_BROKER_URL` | base URL (configure-time env, baked into the build) | OAuth broker base URL for self-host / dev builds. Read at CMake configure time, not at runtime. | `https://auth.braidcast.com` |
