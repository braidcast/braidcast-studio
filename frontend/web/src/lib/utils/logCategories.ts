// Curated DEBUG log categories, shared with the C++ host. The two lists are
// hand-mirrored: a new category is one line here and one line in the enum/name
// table on the other side.
//
// keep in sync with frontend/src/log.hpp (enum class LogCat + LogCatName)

export const Cat = {
  lifecycle: "lifecycle",
  bridge: "bridge",
  preview: "preview",
  canvas: "canvas",
  stream: "stream",
  encode: "encode",
  audio: "audio",
  oauth: "oauth",
  chat: "chat",
  events: "events",
  overlay: "overlay",
  import: "import",
  scene: "scene",
  mcp: "mcp",
  net: "net",
  cef: "cef",
  // Gate-only: toggles libobs's per-frame render-thread timing ([render-debug]) on
  // the host; excluded from the blanket debug toggle. No web log.dbg uses it.
  render: "render",
  // Gate-only: toggles the GPU timer queries behind those diagnostics on the host.
  // Separate from render because the per-frame query readback runs on the graphics
  // thread and can itself cause the stalls that timing exists to find. Excluded
  // from the blanket debug toggle. No web log.dbg uses it.
  rendergpu: "rendergpu",
} as const;

export type LogCategory = (typeof Cat)[keyof typeof Cat];
