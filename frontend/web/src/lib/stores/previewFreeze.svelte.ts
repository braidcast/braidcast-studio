// A still of the canvas, held in the DOM while the native preview surface is hidden.
//
// The preview is a child HWND the OS composites above the whole CEF window, so a menu
// or a modal that overlaps it can only be shown by hiding the preview -- which is why
// right-clicking the canvas blanked it. This keeps the last frame on screen in the
// surface's place for as long as the overlay is up, so the preview reads as paused
// rather than broken. The hand-off in both directions is sequenced by syncPreviewGate
// in $lib/docking/previewSurface.
//
// Explicitly a stand-in, not the fix. The frame does not advance, and anything that
// wants live video under a modal needs the boundary itself to go away; the options and
// the ruling against accommodation designs are in
// braidcast-notes/preview-architecture.md.

import { tick } from "svelte";
import { obs } from "$lib/api/bridge";
import {
  elementTimingSource,
  frozenFrameFrom,
  paintStill,
  placeCanvasRect,
  sameSurfaceSize,
  type FrozenFrame,
} from "$lib/docking/freezeFrame";
import type { PreviewTarget } from "$lib/docking/previewSurface";
import { log } from "$lib/utils/log";
import { Cat } from "$lib/utils/logCategories";
import type { OverlayRect } from "$lib/utils/overlayRect";

// How long the still outlives the gate's release. A reshown surface warms up beneath the
// web view and the host raises it once its fresh swapchain has presented, or when
// kWarmupTimeoutMs (250 ms, overlay_surface.cpp) runs out. That timer starts only when
// the host handles this surface's setRect -- after the bridge round trip and a
// synchronous display create, and after every other surface reshowing ahead of it on the
// same UI thread -- so it is not measured from here, and this hold is a margin over it
// rather than a guarantee. Dropping the still before the raise exposes the page backdrop;
// dropping it after is invisible. Raise the host bound and this must follow.
const RELEASE_HOLD_MS = 500;

// Bound on each step of putting the still on screen (its presentation, or its decode and
// each frame wait). The surface only hides once these finish, so an unbounded step would
// leave the overlay drawn over the menu it is being hidden for -- rAF does not run while
// the web view is hidden, and nothing guarantees a decode or a presentation entry settles.
// A step that runs out just moves on: a still that lands late beats a blank region.
const PAINT_STEP_MS = 150;

function bounded(step: Promise<unknown>): Promise<void> {
  return new Promise((resolve) => {
    const timer = setTimeout(resolve, PAINT_STEP_MS);
    const done = () => {
      clearTimeout(timer);
      resolve();
    };
    step.then(done, done);
  });
}

function nextFrame(): Promise<void> {
  return bounded(new Promise((resolve) => requestAnimationFrame(resolve)));
}

const ELEMENT_TIMING = elementTimingSource();

export class PreviewFreeze {
  /** The held frame, or null when the surface is live. */
  frame = $state.raw<FrozenFrame | null>(null);

  /** The element showing `frame`, bound by the dock so capture() can wait on its decode. */
  img = $state<HTMLImageElement | undefined>();

  /** The `elementtiming` identifier the still's element carries, so its presentation can be told apart. */
  paintId = $state<string | undefined>();

  // Per-operation token. Capture and the release hold are both async, so each one
  // checks it is still the latest before touching `frame`: a capture overtaken by the
  // overlay closing must not paint afterwards, and a hold overtaken by a new capture
  // must not drop the new still.
  #seq = 0;
  #releaseTimer: ReturnType<typeof setTimeout> | undefined;
  // The surface the held frame stands in for, and a token of its own for re-placements,
  // so a slow answer for an old size cannot land over a newer one.
  #target: PreviewTarget | undefined;
  #placeSeq = 0;

  /**
   * Grab the current frame and put it on screen. Resolves once a frame showing the still
   * has been presented -- or, where that cannot be observed, once it has decoded and
   * PAINT_FRAMES have passed -- each step bounded (see paintStill); or once the capture
   * has failed or a later operation has overtaken this one. Never rejects.
   * Call BEFORE hiding the surface: the still has to be on screen before the surface
   * leaves it.
   *
   * `element` is the surface's element rect, as preview.setRect takes it. The host
   * answers with where the surface draws the canvas at that size, and the still is placed
   * there: the surface insets the canvas by a margin and a zoom or pan moves it, so
   * filling the element would show it at a different size and position than the preview
   * it replaces. A reply that cannot be placed counts as failed (frozenFrameFrom).
   *
   * A failed capture leaves `frame` as it was -- no still, or the one already held from
   * a moment ago -- and is not surfaced: an error toast on right-click would be worse
   * than the thing it reports.
   */
  async capture(target: PreviewTarget, element: OverlayRect): Promise<void> {
    const seq = this.#next();
    let frame: FrozenFrame | null;
    try {
      frame = frozenFrameFrom(await obs.call("preview.freeze", { ...target, ...element }), element);
    } catch {
      return;
    }
    if (!frame || seq !== this.#seq) {
      return;
    }
    const still = frame;
    const shownAt = performance.now();
    const how = await paintStill({
      heldUri: this.frame?.dataUri,
      nextUri: still.dataUri,
      source: ELEMENT_TIMING,
      insert: async (paintId) => {
        this.#target = target;
        this.paintId = paintId;
        this.frame = still;
        await tick();
        return this.img;
      },
      bound: bounded,
      nextFrame,
      current: () => seq === this.#seq,
    });
    if (how !== "overtaken") {
      log.dbg(Cat.preview, `freeze still on screen (${how}) after ${Math.round(performance.now() - shownAt)} ms`);
    }
  }

  /**
   * The held still's element now measures `element`: move the still to where the surface
   * would draw the canvas at that size. The hidden surface draws nothing, so the host lays
   * the rect out from the size rather than reading it off a frame. Until the answer lands
   * the still keeps its size and aspect ratio, and an answer that cannot be placed leaves
   * it where it is. Never rejects.
   */
  async relayout(element: OverlayRect): Promise<void> {
    // Taken before the early return too: a resize back to the held size must still void
    // an answer in flight for the size in between.
    const placeSeq = ++this.#placeSeq;
    const held = this.frame;
    const target = this.#target;
    if (!held || !target || sameSurfaceSize(held.element, element)) {
      return;
    }
    const seq = this.#seq;
    let placement;
    try {
      placement = placeCanvasRect((await obs.call("preview.canvasRect", { ...target, ...element })).canvasRect, element);
    } catch {
      return;
    }
    if (placement && seq === this.#seq && placeSeq === this.#placeSeq && this.frame === held) {
      this.frame = { ...held, placement, element };
    }
  }

  /** The surface has been re-asserted: drop the still once the surface should cover it. */
  release(): void {
    const seq = this.#next();
    this.#releaseTimer = setTimeout(() => {
      if (seq === this.#seq) {
        this.frame = null;
      }
    }, RELEASE_HOLD_MS);
  }

  /** Drop the still now; nothing is coming back to cover the region. */
  clear(): void {
    this.#next();
    this.frame = null;
  }

  #next(): number {
    clearTimeout(this.#releaseTimer);
    return ++this.#seq;
  }
}
