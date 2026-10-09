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
`whisper-build/CMakeLists.txt`, not by upstream's defaults: static, CPU backend,
`GGML_NATIVE=OFF` with AVX2, OpenMP off, no `UNICODE`. Upgrade by checking out the
new tag, updating the commit here, and rerunning the voice self-test
(`[selftest] voice-whisper version ...`, whose expected string names the version).
The OpenAI Whisper weights it loads are MIT as well; they are downloaded at run time
and never vendored.

### It owns the one `ggml` target in this binary

whisper.cpp vendors ggml and adds it only `if (NOT TARGET ggml)`. llama.cpp guards the
same way, so a second ggml consumer added later does **not** collide -- it reuses this
one. Three consequences follow from there being exactly one ggml, and they are the
reason this paragraph exists rather than being rediscovered:

- **The pins move together.** A second consumer expects some ggml API, and it gets
  whichever one whisper's tag vendors. Bumping one may force bumping the other.
  Renaming targets to keep two copies is not a way out: two static ggml libraries both
  define `ggml_init`, and MSVC rejects that with LNK2005.
- **The backend set is global to that target.** Whoever enables a GPU backend enables it
  for whisper too, against the `GGML_CUDA`/`GGML_VULKAN` `OFF` here and the spec's
  "opt-in only after the P0 probe measures its cost".
- **`Voice::CpuSupportsVoice` is really a ggml-stack gate.** It refuses on a CPU without
  AVX/AVX2/FMA/F16C/BMI2 because ggml is compiled for them. Anything else on this ggml
  is gated by the identical predicate, whatever the function is called.

Copy `whisper-build/CMakeLists.txt`'s isolation verbatim for any second consumer: plain
`set()` (never `set(... CACHE BOOL ...)`) with `CMAKE_POLICY_DEFAULT_CMP0077 NEW`, so
upstream's `option()` honours the normal variable and writes nothing to the global
cache, plus `EXCLUDE_FROM_ALL SYSTEM` and the `UNICODE` strip.
