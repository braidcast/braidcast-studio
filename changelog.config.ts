// Release-notes configuration, consumed by .github/scripts/changelog/generate.ts
// when a `v*` tag cuts a draft release.
//
// This repo does not use Conventional Commits. CONTRIBUTING.md mandates a module
// prefix that names the subsystem (`libobs:`, `obs-ffmpeg:`, `frontend:`, ...).
// changelogen parses the token before the first `:` as its commit "type", so each
// prefix is routed to one of the sections below.

import type { ChangelogConfig } from "changelogen";

// Section key -> heading. Declaration order is render order.
const types = {
  frontend: { title: "🖥️ Frontend" },
  core: { title: "⚙️ Core" },
  plugins: { title: "🔌 Plugins" },
  build: { title: "🔧 Build" },
  ci: { title: "🤖 CI" },
  docs: { title: "📖 Documentation" },
  test: { title: "🧪 Tests" },
  chore: { title: "🧹 Chore" },
  misc: { title: "📦 Other" },
} satisfies ChangelogConfig["types"];

// Commit prefix (lowercased, the token before the first `:`) -> section key above.
// Prefixes naming a directory under plugins/ are routed to `plugins` automatically,
// so they do not need an entry here. Anything else unlisted falls back to `misc`,
// which is why no commit can silently vanish from the notes.
const moduleMap = {
  frontend: "frontend",

  libobs: "core",
  "libobs-d3d11": "core",
  "libobs-metal": "core",
  "libobs-opengl": "core",
  "libobs-winrt": "core",
  deps: "core",

  plugins: "plugins",
  shared: "plugins",

  cmake: "build",
  build: "build",
  "build-aux": "build",

  ci: "ci",

  docs: "docs",

  test: "test",

  chore: "chore",
  treewide: "chore",
  brand: "chore",
  website: "chore",
} satisfies Record<string, keyof typeof types>;

// Sections that aggregate several modules. For these, the originating prefix is kept
// as a scope, so `win-capture: fix hooking` renders as `- **win-capture:** Fix hooking`
// rather than an unattributable `- Fix hooking`. Sections whose prefix is already the
// section name (frontend, ci, ...) gain nothing from that and are left alone.
const scopedSections = ["core", "plugins"] satisfies (keyof typeof types)[];

export default {
  types,
  moduleMap,
  scopedSections,
  // Generate-only: never write CHANGELOG.md. The release version comes from the
  // git tag via `git describe` + CMake (cmake/common/versionconfig.cmake), so
  // changelogen must never bump a version or touch a package.json either.
  output: false,
  hideAuthorEmail: true,
};
