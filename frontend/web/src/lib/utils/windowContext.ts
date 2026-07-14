// A detached window's bundle is loaded at app://app/index.html?window=<id>&dock=<id>.
// The main window has neither param. WINDOW_ID drives the per-window preview overlay
// routing (preview.* `window` param); DETACHED_DOCK names the single dock this window
// owns (null in the main window).
const params = new URLSearchParams(window.location.search);
export const WINDOW_ID: number = Number.parseInt(params.get("window") ?? "0", 10) || 0;
export const DETACHED_DOCK: string | null = params.get("dock");
