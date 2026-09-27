// The freeze still's geometry and the wait for it to reach the screen, kept out of the rune
// store so they can be tested. The host lays out where the surface would draw its canvas
// (the draw callback's own arithmetic, see bridge.cpp's PreviewCanvasRectJson); this only
// carries that rect into the element.

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

/** Hands each element-timing entry's identifier to `onEntry`; the returned function stops it. */
export type ElementTimingSource = (onEntry: (identifier: string) => void) => () => void;

// Element Timing queues an image's entry from the presentation feedback of the frame that
// first painted it, so the entry arriving is the one sign a page gets that a picture has
// reached the screen rather than been scheduled for it. Null where it is not supported.
export function elementTimingSource(): ElementTimingSource | null {
  if (typeof PerformanceObserver === "undefined" || !PerformanceObserver.supportedEntryTypes?.includes("element")) {
    return null;
  }
  return (onEntry) => {
    const observer = new PerformanceObserver((list) => {
      for (const entry of list.getEntries()) {
        onEntry((entry as PerformanceEntry & { identifier?: string }).identifier ?? "");
      }
    });
    observer.observe({ type: "element" });
    return () => observer.disconnect();
  };
}

// Every dock's observer sees every element entry in the document, and the docks capture in
// lockstep on the one preview gate, so an id drawn from a per-dock counter would let one
// dock's still answer for another's. One counter for the document.
let paintSerial = 0;

/** An `elementtiming` identifier no other still in this document carries. */
export function nextPaintId(): string {
  return `freeze-${++paintSerial}`;
}

/** Watch for `identifier` to be presented. Start it before the element is inserted. */
export function watchPresented(identifier: string, source: ElementTimingSource): { presented: Promise<void>; stop: () => void } {
  let stop = () => {};
  const presented = new Promise<void>((resolve) => {
    stop = source((id) => {
      if (id === identifier) {
        stop();
        resolve();
      }
    });
  });
  return { presented, stop: () => stop() };
}

/** The steps that put a freshly inserted still on screen, each already bounded by the caller. */
export interface StillPaintSteps {
  /** Resolves once a frame showing the still has been presented; null when that cannot be observed. */
  presented: Promise<void> | null;
  decode(): Promise<void>;
  nextFrame(): Promise<void>;
  /** False once a later operation has overtaken this one. */
  current(): boolean;
}

// Where the page cannot observe the still being presented: frames to let pass after it has
// decoded before the surface may hide. Measured in desktop Chrome 153 at 144 Hz, two frames
// left the hide ahead of the still's presentation in 35 of 40 trials, and four in none of 20.
export const PAINT_FRAMES = 4;

// Resolves once the still may be relied on to be on screen. A presentation signal, when
// there is one, is the only thing waited on: a frame boundary proves the frame was
// produced, and Chromium presents it a frame or more later.
export async function stillOnScreen(steps: StillPaintSteps, fallbackFrames: number): Promise<void> {
  if (steps.presented) {
    await steps.presented;
    return;
  }
  await steps.decode();
  for (let i = 0; i < fallbackFrames && steps.current(); i++) {
    await steps.nextFrame();
  }
}

/** How putting a still on screen ended; "overtaken" when a later operation superseded it. */
export type StillPaintOutcome = "presented" | "timeout" | "frames" | "overtaken";

export interface StillPaint {
  /** The picture the still's element shows now, if any. */
  heldUri: string | undefined;
  nextUri: string;
  source: ElementTimingSource | null;
  /** Put the still in the DOM carrying `paintId`; resolves to its element once rendered. */
  insert(paintId: string): Promise<{ decode(): Promise<unknown> } | undefined>;
  /** Bounds one step, resolving (never rejecting) when it settles or runs out. */
  bound(step: Promise<unknown>): Promise<void>;
  nextFrame(): Promise<void>;
  /** False once a later operation has overtaken this one. */
  current(): boolean;
}

// The whole hand-over for one capture: the watch starts before the still is inserted, so
// its entry cannot be missed, and the surface may hide once this resolves.
export async function paintStill(p: StillPaint): Promise<StillPaintOutcome> {
  const paintId = nextPaintId();
  // A still already showing this exact picture paints nothing new, so no entry would come.
  const watch = p.source && p.heldUri !== p.nextUri ? watchPresented(paintId, p.source) : null;
  let how: StillPaintOutcome = watch ? "timeout" : "frames";
  const presented = watch?.presented.then(() => {
    how = "presented";
  });
  try {
    const img = await p.insert(paintId);
    if (!p.current()) {
      return "overtaken";
    }
    await stillOnScreen(
      {
        presented: presented ? p.bound(presented) : null,
        decode: () => (img ? p.bound(img.decode()) : Promise.resolve()),
        nextFrame: p.nextFrame,
        current: p.current,
      },
      PAINT_FRAMES,
    );
    return p.current() ? how : "overtaken";
  } finally {
    watch?.stop();
  }
}
