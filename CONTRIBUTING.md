Contributing to Braidcast
=========================

Braidcast is a fork of [OBS Studio](https://obsproject.com) that adds native
multi-destination streaming (multiple canvases + output multiplexing) and
replaces the Qt desktop UI with a CEF-hosted Svelte frontend. It is an
experimental fork and is **not** currently upstreamed to the OBS Project — for
contributions to OBS Studio itself, use the upstream repository and its
guidelines.

This document covers the conventions for working in this repository. It builds
on OBS Studio and preserves upstream licensing and attribution (see
`COPYING`, `AUTHORS`, and the per-file copyright headers under `libobs/`,
`plugins/`, and `deps/` — do not remove or alter these).

## Building

The build is CMake-preset driven (CMake ≥ 3.28, Qt 6, and the Svelte frontend
toolchain via `bun`). See `README.rst` and `CLAUDE.md` for the full setup;
the short version on Windows x64:

```
git submodule update --init --recursive
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

The frontend web UI lives in `frontend/web/` and is built as part of the
`braidcast-frontend` target (`bun install && bun run build`); `bun` must be on
`PATH` for CMake configure to succeed.

## Commits

- **Module prefix, not Conventional Commits.** Title as `<module>: <subject>`,
  e.g. `libobs:`, `frontend:`, `plugins:`, `cmake:`, `CI:`, `obs-ffmpeg:`.
  Use the prefix of the module the change touches.
- **50/72.** Title ≤ 50 characters; wrap the body at 72. The body explains
  *why*, not just what.
- Each commit should be a self-contained unit of change that leaves the project
  buildable.
- American English throughout (commit messages, comments, identifiers, files).
- Preserve original authorship when finishing someone else's work — cherry-pick
  to retain the author, or add a `Co-authored-by:` trailer.

## Code style (enforced by CI)

The project pins exact formatter versions; CI rejects changes that don't match.
Column limit is **120 characters**.

- **C / C++ / Objective-C** — `clang-format` **19.1.1**. Run
  `./build-aux/run-clang-format`.
- **CMake** — `gersemi`. Run `./build-aux/run-gersemi`.
- **Swift** — `swift-format`. Run `./build-aux/run-swift-format`.
- **Frontend (TypeScript / Svelte)** — from `frontend/web/`, run `bun run check`
  (svelte-check) and `bun run build` before submitting; the Rollup build catches
  type issues `check` alone can miss.

Run the relevant formatter on the files you changed before opening a change —
CI's format check is scoped to your changed files.

## License

Braidcast is licensed **GPLv2+**, inherited from OBS Studio. Any contribution is
made under the same license. Do not add code under an incompatible license, and
keep upstream copyright headers and attribution intact.
