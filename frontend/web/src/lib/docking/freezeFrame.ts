// The freeze still's geometry, kept out of the rune store so it can be tested. The host
// lays out where the surface would draw its canvas (the draw callback's own arithmetic,
// see bridge.cpp's PreviewCanvasRectJson); this only carries that rect into the element.

import type { ObsMethods, PreviewCanvasRect } from "$lib/api/bridge";
import type { OverlayRect } from "$lib/utils/overlayRect";

/** A box inside the surface's element, in CSS px from its top-left corner. */
export interface BoxPlacement {
  left: string;
  top: string;
  width: string;
  height: string;
}

/** A held frame, where the surface would draw it, and the element rect that is for. */
export interface FrozenFrame {
  dataUri: string;
  placement: BoxPlacement;
  element: OverlayRect;
}

// Place a canvas rect the host computed for `element`. CSS px rather than fractions of
// the element: until a resize has been re-placed the still keeps its size and aspect
// ratio instead of stretching with the box. Null for a degenerate rect, and for one laid
// out for a surface of another size than this element's -- a stale answer describes a
// different picture, and placing it would show the canvas at the wrong size.
export function placeCanvasRect(r: PreviewCanvasRect | undefined, element: OverlayRect): BoxPlacement | null {
  if (!r || r.w <= 0 || r.h <= 0) {
    return null;
  }
  // The host rounds CSS px to device px; a tolerance avoids restating its rule here.
  if (Math.abs(r.surfaceW - element.w * element.dpr) > 1 || Math.abs(r.surfaceH - element.h * element.dpr) > 1) {
    return null;
  }
  const px = (n: number) => `${n / element.dpr}px`;
  return { left: px(r.x), top: px(r.y), width: px(r.w), height: px(r.h) };
}

// A freeze reply as the still to show, or null when it cannot be placed -- which counts
// as a failed capture: the still would otherwise have to guess where the canvas was.
export function frozenFrameFrom(reply: ObsMethods["preview.freeze"], element: OverlayRect): FrozenFrame | null {
  const placement = placeCanvasRect(reply.canvasRect, element);
  return placement ? { dataUri: reply.dataUri, placement, element } : null;
}

/** Whether two element rects give the surface the same size (position is irrelevant). */
export function sameSurfaceSize(a: OverlayRect, b: OverlayRect): boolean {
  return a.w === b.w && a.h === b.h && a.dpr === b.dpr;
}
