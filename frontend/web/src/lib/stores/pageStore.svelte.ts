// Active top-level view. The 70px nav rail switches between the full-page views;
// App.svelte renders one at a time off this rune. Values mirror the mock's
// state.page, with `stats` mapped to `monitor` to match the rail's labels.

export type Page = "studio" | "canvases" | "streams" | "overlays" | "schedule" | "monitor" | "ai" | "settings";

export const pageStore = $state<{ page: Page }>({ page: "studio" });

export function setPage(p: Page): void {
  pageStore.page = p;
}
