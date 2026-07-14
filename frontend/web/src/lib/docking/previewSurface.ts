// Shared native-preview overlay plumbing for the two docks that host a preview
// surface (the Default PreviewDock and the per-canvas CanvasDock). The native
// overlay is a sibling HWND painted above CEF, addressed by { window, canvas? }:
// the Default surface OMITS canvas (global channel-0 path); a per-canvas dock
// passes its uuid. Only the obs plumbing, the paintability guard, and the
// device-px cursor mapping live here -- each dock keeps its own lifecycle wiring
// (coalescing, output-gating, surface-active flag, menu shape), which genuinely
// differs.

import { obs } from "$lib/api/bridge";
import { WINDOW_ID } from "$lib/utils/windowContext";

function target(canvasUuid?: string): Record<string, unknown> {
  return canvasUuid ? { canvas: canvasUuid, window: WINDOW_ID } : { window: WINDOW_ID };
}

// Reads the element rect ONCE. When the region isn't paintable (tab-stacked in the
// background, collapsed, or zero-sized) the overlay would otherwise keep painting
// at its stale rect over whatever is now on top -- so hide it and report false.
// Otherwise assert the rect and report true. Callers use the return to drive their
// own "surface active" state.
export function syncPreviewRect(el: HTMLElement, canvasUuid?: string): boolean {
  const r = el.getBoundingClientRect();
  if (!el.offsetParent || r.width < 1 || r.height < 1) {
    hidePreview(canvasUuid);
    return false;
  }
  obs
    .call("preview.setRect", {
      ...target(canvasUuid),
      x: r.left,
      y: r.top,
      w: r.width,
      h: r.height,
      dpr: window.devicePixelRatio || 1,
    })
    .catch((e) => console.log("preview.setRect failed: " + (e as Error).message));
  return true;
}

export function hidePreview(canvasUuid?: string): void {
  obs.call("preview.hide", target(canvasUuid)).catch(() => {});
}

export function destroyPreview(canvasUuid?: string): void {
  obs.call("preview.destroy", target(canvasUuid)).catch(() => {});
}

// Map an overlay-reported device-pixel cursor to viewport coords via the element
// rect (the overlay HWND reports hits in its own device-px space).
export function mapOverlayCursor(el: HTMLElement, p: { x: number; y: number }): { x: number; y: number } {
  const r = el.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  return { x: r.left + p.x / dpr, y: r.top + p.y / dpr };
}
