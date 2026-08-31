// The rect shape every native overlay is positioned with. The host converts CSS px
// -> device px with `dpr`, so both sides share one rounding rule (bridge.cpp's
// OverlayRectFromParams); nothing here does the conversion itself.
export interface OverlayRect {
  x: number;
  y: number;
  w: number;
  h: number;
  dpr: number;
}

// An element's rect in that shape, or null when the region isn't paintable --
// detached from layout (tab-stacked in the background, collapsed) or zero-sized.
// A null means "hide the overlay": re-asserting a stale rect would raise the native
// child window back above CEF, over whatever is now on top.
export function overlayRectOf(el: HTMLElement): OverlayRect | null {
  const r = el.getBoundingClientRect();
  if (!el.offsetParent || r.width < 1 || r.height < 1) {
    return null;
  }
  return { x: r.left, y: r.top, w: r.width, h: r.height, dpr: window.devicePixelRatio || 1 };
}
