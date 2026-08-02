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
