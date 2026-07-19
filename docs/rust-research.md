# Rust rewrite — feasibility research

**Date:** 2026-07-19 · **Status:** research only. **Decision: not pursuing now** — current-app parity/fixes come first. This is a log for us, captured so the analysis isn't lost.

## The question

Could Braidcast (OBS fork: C/C++ `libobs` engine + CEF/Svelte UI) move to a Rust stack — either fully (C++ removed, all Rust) or a Rust core exposing the libobs plugin ABI — and does real-world precedent de-risk it? Trigger: interest in a Tauri-style stack + the `cpp2rust` transpiler ("safe Rust") + the intuition that Rust now has good A/V + language-bridge support.

## Bottom line

- **No Rust OBS exists.** Nobody has assembled capture + GPU compositing + hardware encode + RTMP fan-out into an OBS-class desktop app in Rust. Every candidate covers only a slice; the two best-funded adjacent Rust apps deliberately stop before live streaming.
- **Precedent de-risks the *components*, not the *product*.** wgpu compositing and gstreamer-rs are production-ready; the pieces that are specifically OBS-critical are exactly where Rust is weak or absent.
- **"Fully Rust" is un-de-risked pioneering** with a concrete showstopper (game-capture hook has zero Rust foundation) and, if plugins are to be kept, a Wine-scale ABI-compat bill.
- **The only precedented path is incremental Rust *behind* the existing C ABI** (the `librsvg` model) — keep FFmpeg/vendor-SDK encode in C indefinitely. Braidcast has already done a miniature version of this (`frontend/api-impl/`).
- **Transpilers don't rescue it.** `cpp2rust` (AST-based, PLDI 2026 research prototype) emits *safe* Rust only via a reference-counting runtime (`Ptr<T>` + runtime checks) — a safe transliteration, not idiomatic Rust — supports C only cleanly (C++ templates/STL/exceptions unproven), has no external-C-library linking story, and a transpile permanently severs upstream sync. It automates a slice of the easy typing, not the hard 90% (linking, idiom, validation, the FFI boundary).

## No Rust OBS precedent — the candidates

| Project | Scope covered | Maturity | Missing vs OBS |
| --- | --- | --- | --- |
| [Smelter](https://github.com/software-mansion/smelter) (Software Mansion) | Real-time wgpu compositing + audio mix + RTMP/WHIP/RTP in/out | 705★, 19 releases, active | No screen capture; server-side toolkit, not a desktop app |
| [Cap](https://github.com/CapSoftware/Cap) | Capture + encode + edit/export (Tauri) | 20.2k★, commercial | **Explicitly no live streaming/RTMP**; no compositing/scenes |
| [RustDesk](https://github.com/rustdesk-org/hwcodec) | Capture + HW encode + network streaming, in production | Very mature | Remote-desktop, not broadcast; **encode via C/C++ `hwcodec`**, not pure Rust |
| [Screenpipe](https://github.com/screenpipe/screenpipe) | 24/7 screen+mic recording | 19k★, YC | AI-memory tool; no compositing/streaming |
| [Kooha](https://github.com/seadve/kooha) | Linux screen recording (GTK4) | Healthy niche | Recording only; on GStreamer/PipeWire (C underneath) |
| [Gyroflow](https://docs.gyroflow.xyz) | GPU video processing on wgpu, pure-Rust core | Shipped | Offline processing, not live capture/stream |
| [libobs-rs](https://github.com/libobs-rs/libobs-rs) | Rust bindings **to** libobs | Small, active | The tell: Rust devs wanting OBS-class output **wrap the C engine, don't rewrite it** |

The one attempt to put Rust *inside* OBS (a WHIP/webrtc-rs prototype) was abandoned partly because "integration of Rust and CMake didn't end up being easy"; OBS shipped WHIP in C.

## Component readiness

| Component | Verdict | Notes |
| --- | --- | --- |
| wgpu compositing | **Ready** | Smelter + Gyroflow in production; Firefox ships wgpu. Strongest piece. Capture-texture→wgpu interop is fiddlier than OBS's native D3D11 path. |
| [gstreamer-rs](https://gitlab.freedesktop.org/gstreamer/gstreamer-rs) | **Ready** | Official subproject; upstream streaming elements (WHIP/WHEP) now *written in Rust*. But GStreamer itself is C — you inherit its pipeline model + debugging. |
| FFmpeg bindings | **Usable-with-gaps** | `ffmpeg-next` is maintenance-only; [rsmpeg](https://github.com/larksuite/rsmpeg) (ByteDance) healthier. All `unsafe` FFI over C; safe wrappers thin. |
| HW encode (NVENC/AMF/QSV) | **Usable-with-gaps** | **No production direct Rust vendor bindings.** Battle-tested routes all go through C (FFmpeg/GStreamer). Even RustDesk ships HW-encode via C++ (`hwcodec`). |
| Screen capture | **Usable-with-gaps** | `windows-capture` (WGC/DXGI) active; `scap` still v0.1-beta. **Critical gap: no Rust equivalent of OBS game capture (graphics-hook DLL injection + shared-texture) — none found.** |
| wgpu / graphics | **Ready** | (as above) |
| Audio (`cpal`) | **Usable-with-gaps** | Device I/O only; per-app WASAPI loopback you'd build yourself. Documented ALSA/PipeWire gotchas. |
| RTMP publish out | **Gap unless via C** | No mature standalone Rust RTMP *publisher* in wide production use. WHIP/WebRTC is where Rust is genuinely strong (gst-plugins-rs, webrtc-rs, str0m). |

## C-ABI plugin hosting — the load-bearing question

**Mechanically yes:** a Rust host loads C-ABI `.dll`/`.so` plugins via [`libloading`](https://adventures.michaelfbryan.com/posts/plugins-in-rust/); a Rust `cdylib` exports `extern "C"` symbols. Routine.

**Precedent for reimplementing a C ABI in Rust so *unmodified* consumers work:**
- [rustls-libssl](https://www.memorysafety.org/blog/rustls-nginx-compatibility-layer/) — partial OpenSSL libssl ABI in Rust; **unmodified nginx runs against it** (self-described experimental, subset only).
- [zlib-rs](https://crates.io/crates/zlib-rs) — memory-safe zlib exposing the zlib C ABI.
- **librsvg (GNOME)** — the strongest model: internals ported to Rust **incrementally while keeping the C API/ABI**; shipped in every Linux distro for years.

**The cost for libobs, measured against this repo:** **1,863 `EXPORT` function declarations across 66 public headers** (`obs.h` = 670, `graphics/graphics.h` = 291, `obs-properties.h` = 108, `obs-data.h` = 117). Functions are the easy half — ABI compat also requires bit-exact public struct layouts (`obs_source_info` etc. are registered *by value* from plugin code), stable enum values + callback signatures, the signal/calldata/proc semantics, and header-inlined utils (dstr/darray/threading) compiled *into* plugins. Plus bug-for-bug fidelity (consumers depend on undocumented behavior). **That is a Wine-class compatibility project, not a rewrite-with-bindings.**

**Verdict:** keeping OBS plugins against a clean-room Rust libobs is feasible *in principle*, unprecedented at this scale, enormously expensive. The de-risked variant is the **librsvg model** — keep the C headers/ABI as the contract, port internals file-by-file behind them (Rust + C link into one library). `frontend/api-impl/`'s headless `obs-frontend-api` shim is already a miniature of exactly this.

## Language bridges — what's load-bearing

- **`bindgen`** (C→Rust) — the workhorse; wrapping libobs/CEF/vendor SDKs. Firefox-grade.
- **`cbindgen`** (Rust→C headers) — how a Rust core exposes a C ABI for plugins. Mature.
- **`cxx`** (Rust↔C++) — only for C++ seams (CEF, Qt-coupled plugins). Mature, constrained.
- **`pyo3`** / **`napi-rs`** — optional scripting/extension-layer choices (Python / JS plugins), NOT feasibility factors. For Braidcast only bindgen/cbindgen are load-bearing.

## Community sentiment (last-30-days + recent)

- **No live "Rust for A/V" conversation** — across ~26 Reddit/HN items in-window, zero threads on building streaming/compositing apps in Rust. The A/V-in-Rust community is small enough to be invisible on r/rust for a month. (Silence = data.)
- **The month's dominant systems threads are rewrite-cost skepticism** — [Rust→Zig rewrite](https://rtfeldman.com/rust-to-zig) (527 pts) and [Andrew Kelley on the Bun Rust rewrite](https://andrewkelley.me/post/my-thoughts-bun-rust-rewrite.html) (813 pts / 707 comments).
- **Rust-for-systems sentiment is high** (GKH's "Rust makes coding fun again", 785 pts) — but the top comments flag **the C-FFI boundary as exactly where Rust's friction concentrates**, which is precisely where a Rust-core-with-C-plugins design would permanently live.

## Recommended path (if ever pursued)

1. **Do not** reimplement libobs or go all-Rust — Wine-scale, un-de-risked, and you'd forfeit the plugin ecosystem + upstream sync + a decade of capture/encode hardening.
2. **librsvg model** — keep libobs's C headers/ABI as the contract; port internals file-by-file behind them where it pays (safety-critical or heavily-churned modules), Rust and C linking into one library.
3. **Rust for the layers where the ecosystem is already strong** — the compositor (wgpu), WHIP/WebRTC egress (gst-plugins-rs/webrtc-rs), services/orchestration. Keep FFmpeg/vendor-SDK encode paths in C indefinitely (even Rust's flagship apps do).
4. **Shell** — CEF→Tauri/WebView2 is a *separate* decision from the language; it addresses the blank-white CEF-GPU-subprocess class and binary size, but the native-GPU-preview-over-webview problem (§4.2) persists regardless.

## Could-not-verify flags

- `vk-video` maintenance status (conflicting signals; treat Vulkan-Video-in-Rust as experimental).
- Named commercial gstreamer-rs users (asserted, roster not public).
- `napi-rs` enterprise adopters (SWC/Prisma widely believed, not confirmed this pass).
- Standalone Rust RTMP-publisher maturity (`rml_rtmp` not deep-checked; low confidence a production-grade one exists).
- X/Twitter sentiment not searched (no auth); sentiment rests on Reddit/HN/web.
