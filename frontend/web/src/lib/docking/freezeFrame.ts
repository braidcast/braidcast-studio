// The freeze still's geometry and the wait for it to reach the screen, kept out of the rune
// store so they can be tested. The host draws the still with the surface's own draw callback
// at the surface's device-pixel size (bridge.cpp's MethodPreviewFreeze); this only lays that
// picture over the element pixel for pixel.

import type { ObsMethods } from "$lib/api/bridge";
import type { OverlayRect } from "$lib/utils/overlayRect";
import { nextPaintId, watchPresented, type ElementTimingSource } from "$lib/utils/presented";

/** A box inside the surface's element, in CSS px from its top-left corner. */
export interface BoxPlacement {
  left: string;
  top: string;
  width: string;
  height: string;
}

/** A held frame, where it sits in the element, and the element rect it was drawn for. */
export interface FrozenFrame {
  dataUri: string;
  placement: BoxPlacement;
  element: OverlayRect;
}

// Lay a width x height device-px still over `element` at one image pixel per device pixel,
// starting on the device pixel the surface's HWND starts on: the host rounds the element's
// CSS position to device px, and a still left at the fractional position would be resampled
// across two pixels. Sized in CSS px, so until a resize has been re-captured the still keeps
// its size rather than stretching with the box. Null for an empty still, and for one drawn
// for a surface of another size than this element's -- a stale answer is a different
// picture.
export function placeStill(width: number, height: number, element: OverlayRect): BoxPlacement | null {
  if (width <= 0 || height <= 0) {
    return null;
  }
  const { x, y, w, h, dpr } = element;
  // The host rounds CSS px to device px; a tolerance avoids restating its rule here.
  if (Math.abs(width - w * dpr) > 1 || Math.abs(height - h * dpr) > 1) {
    return null;
  }
  const snap = (v: number) => `${Math.round(v * dpr) / dpr - v}px`;
  const px = (n: number) => `${n / dpr}px`;
  return { left: snap(x), top: snap(y), width: px(width), height: px(height) };
}

// A freeze reply as the still to show, or null when it cannot be placed -- which counts as a
// failed capture.
export function frozenFrameFrom(reply: ObsMethods["preview.freeze"], element: OverlayRect): FrozenFrame | null {
  const placement = placeStill(reply.width, reply.height, element);
  return placement ? { dataUri: reply.dataUri, placement, element } : null;
}

/** Whether two element rects give the surface the same size (position is irrelevant). */
export function sameSurfaceSize(a: OverlayRect, b: OverlayRect): boolean {
  return a.w === b.w && a.h === b.h && a.dpr === b.dpr;
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
