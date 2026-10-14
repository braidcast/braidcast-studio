# Vendored frontend dependencies

Vendored rather than fetched: Norton MITMs TLS on the development machine and
breaks CMake's `file(DOWNLOAD)`, which is already why `mage deps` prefetches
everything else. These have no build-time network step at all.

The vendored sources are excluded from the format gate — see `magefile.go`
`formatSkipDirs`, `build-aux/.run-format.zsh`, and the `DisableFormat`
`.clang-format` beside this file. Do not hand-edit them; replace wholesale on
upgrade and update the versions below. `CMakeLists.txt` and this README are ours
rather than upstream's, and are formatted and reviewed normally.

| Library | Version | License | SHA256 of source archive |
| --- | --- | --- | --- |
| SQLite amalgamation | 3.53.4 | Public domain | `1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d` |
| sqlite_orm | 1.9.1 | AGPL-3.0 | `de2db80e4f716a27c4e1f4cb8a356394e428676c98c90b0577b0431107d3cccf` |
| whisper.cpp | 1.9.4 (git submodule `whisper.cpp`, commit recorded below) | MIT | n/a: pinned by commit, not by archive |

SQLite archive: `sqlite-amalgamation-3530400.zip` (2946650 bytes). sqlite.org
publishes a SHA3-256 rather than a SHA-256, so that is the digest to check against
upstream: `628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e`. Both
it and the byte count were verified against the download page for this copy.

sqlite_orm archive: `v1.9.1.tar.gz` from the GitHub release tag (663545 bytes).

## sqlite_orm is AGPL-3.0

sqlite_orm is dual-licensed: AGPL-3.0 as distributed, MIT only after a paid
purchase. The AGPL copy is the one vendored here, deliberately. It is acceptable
because Braidcast is GPLv2-**or-later** and this source tree is public, which is
what AGPL's copyleft and its section 13 network clause ask for. The shipped binary
is already GPLv3-forced regardless: the obs-deps FFmpeg in `bin/64bit/` is built
`--enable-gpl --enable-version3`, so AGPLv3 adds no constraint the product was not
already under.

Do not relicense the frontend or strip this note without re-checking that
reasoning.

## Unrelated `sqlite3.h` in obs-deps

There is a `sqlite3.h` in obs-deps at `include/libajantv2/ajabase/persistence/`.
That is the AJA SDK's own vendored copy, header-only and part of a vendor tree.
It is unrelated to this one; do not couple to it.

## whisper.cpp is a submodule

Unlike the two copies above, whisper.cpp is a git submodule at tag `v1.9.4`,
commit `927cfce34f31707e17f2bff35c349632fb9e2c3a`. It is built by
`whisper-build/CMakeLists.txt`, not by upstream's defaults: four DLLs
(`whisper.dll`, `ggml.dll`, `ggml-base.dll`, `ggml-cpu.dll`), CPU backend only,
`GGML_NATIVE=OFF` with AVX2, OpenMP off, no `UNICODE`, no `/WX` at link.
`braidcast.exe` delay-loads all four; `frontend/cmake/voice-engine.cmake` holds
the list, the `/DELAYLOAD` flags, the rundir copy and the install rules. Upgrade
by checking out the new tag, updating the commit here, and rerunning the voice
self-test (`[selftest] voice-whisper version ...`, whose expected string names the
version). The OpenAI Whisper weights it loads are MIT as well; they are downloaded
at run time and never vendored.

Its MIT license (one notice for whisper.cpp and the ggml code in the same tree,
"The ggml authors") travels with the binaries it ships as:
`frontend/data/license/whisper.cpp-ggml-MIT.txt`, a copy of the submodule's
`LICENSE`, installed with the rest of `data/`. Recopy it when the tag changes.

### Why DLLs, delay-loaded

`ggml-cpu` is compiled for one ISA, `/arch:AVX2`, and the app must still start on a
CPU without AVX2 (voice control is simply unavailable there). A static link could
not promise that, and no cheap audit could prove it:

- `ggml-cpu`'s C++ files have static initializers, which run before `main` and so
  before any CPU check.
- Inline functions (`std::` templates and the like) instantiated in those files are
  compiled for AVX2 too, and the linker keeps one copy of each for the whole exe;
  that copy can be `ggml-cpu`'s, and then app code far from voice runs it.

As DLLs, ggml's code lives only in ggml's own images, with its own copies of every
inline function, and none of those images is mapped until
`Voice::EnsureWhisperLoaded` (`src/voice/WhisperLibrary.cpp`) loads them: after
`Voice::CpuSupportsVoice` passes, the first time voice is turned on. It loads them
by full path from the exe's directory and binds the delay-load imports at once, so
a missing or mismatched DLL turns voice off with a reason instead of raising a
delay-load exception at the first whisper call. Every `whisper_*` call is behind
it; `voice.state`'s `ready.cpu` and the Voice tab report its reason the way they
report the CPU's.

Two checks keep it that way: the smoke self-test case "no DLL of it is mapped at
start with voice off", and `.github/scripts/Check-VoiceDlls.ps1`, which reads the
built exe with `dumpbin` and fails if any of the four is a load-time import or is
missing beside the exe. `Smoke-Package.ps1` asserts the four DLLs in the zip and the
installer, since a package without them still launches.

### Why not `GGML_BACKEND_DL` + `GGML_CPU_ALL_VARIANTS`

That route builds one `ggml-cpu-<variant>.dll` per ISA level (nine on MSVC: x64,
sse42, sandybridge, haswell, ...) and picks the best one at run time, so a pre-AVX2
CPU could run voice on SSE kernels. It is not used, for now:

- Whether voice is usable at SSE speed is unmeasured. The latency targets are being
  measured (P0) on AVX2 hardware, and the spec gates voice on the CPU; letting older
  CPUs in is a product decision that needs those numbers first.
- whisper.cpp does not load backends itself: the app would call
  `ggml_backend_load_all`, whose search includes the current working directory
  (`ggml/src/ggml-backend-reg.cpp`, `ggml_backend_load_best`). Loading code from the
  working directory needs `GGML_BACKEND_DIR` or an explicit path to be safe.
- Nine builds of `ggml-cpu`, nine more DLLs to stage, install and check.

The delay-loaded DLLs here are the step it would build on: the same staging and the
same loader, with the gate moved from "has AVX2" to "has some variant".

### Going back to a static link

If whisper is ever linked statically again, `Check-VoiceDlls.ps1` no longer applies
and the audit the index's risk R2 called for is owed, for both hazards above:

1. Static initializers: `dumpbin /symbols` every `ggml-cpu` object
   (`build_x64/frontend/deps/whisper-build/whisper.cpp/ggml/src/ggml-cpu.dir/<config>/*.obj`)
   and list the dynamic initializers (`??__E...` symbols, and what `.CRT$XCU` points
   at). `dumpbin /disasm` each one and look for VEX code: `ymm` registers, `v`-prefixed
   mnemonics (`vmovups`, `vfmadd...`, `vpbroadcast...`), and `shlx`/`pdep` (BMI2).
2. Shared inline functions: link `braidcast.exe` with `/MAP` (or `/VERBOSE`), and list
   every function whose kept definition came from a `ggml-cpu` object. Each one that
   anything outside ggml also calls (any `std::` template is a candidate) runs ggml's
   AVX2 copy everywhere; disassemble the exe's copy as in step 1.
3. Run the smoke test under Intel SDE with `-snb` (AVX without AVX2) and `-nhm` (no
   AVX), which traps any instruction those CPUs lack, on the paths the run reaches.

Any VEX code found in step 1 or 2 means the static link cannot ship.

### It owns the one `ggml` target in this binary

whisper.cpp vendors ggml and adds it only `if (NOT TARGET ggml)`. llama.cpp guards the
same way, so a second ggml consumer added later does **not** collide -- it reuses this
one. Three consequences follow from there being exactly one ggml, and they are the
reason this paragraph exists rather than being rediscovered:

- **The pins move together.** A second consumer expects some ggml API, and it gets
  whichever one whisper's tag vendors. Bumping one may force bumping the other.
  Renaming targets to keep two copies is not a way out: only one `ggml.dll` fits
  beside the exe, and Windows hands any later load of a module by that name the copy
  already loaded.
- **The backend set is global to that target.** Whoever enables a GPU backend enables it
  for whisper too, against the `GGML_CUDA`/`GGML_VULKAN` `OFF` here and the spec's
  "opt-in only after the P0 probe measures its cost".
- **`Voice::CpuSupportsVoice` is really a ggml-stack gate.** It refuses on a CPU without
  AVX/AVX2/FMA/F16C/BMI2 because ggml is compiled for them. Anything else on this ggml
  is gated by the identical predicate, whatever the function is called, and must load
  ggml through `Voice::EnsureWhisperLoaded` (or the same gate-then-load order) rather
  than call into it from code that runs at startup.

Copy `whisper-build/CMakeLists.txt`'s isolation verbatim for any second consumer: plain
`set()` (never `set(... CACHE BOOL ...)`) with `CMAKE_POLICY_DEFAULT_CMP0077 NEW`, so
upstream's `option()` honours the normal variable and writes nothing to the global
cache (`BUILD_SHARED_LIBS` included: it is `ON` for that directory only), plus
`EXCLUDE_FROM_ALL SYSTEM`, the `UNICODE` strip and the `/WX` link strip. A DLL it adds
needs a `/DELAYLOAD`, a rundir copy and an `install()` rule like those in
`frontend/cmake/voice-engine.cmake`: `EXCLUDE_FROM_ALL` drops upstream's own install
rules.
