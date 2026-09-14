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

// How long the still outlives the gate's release. A reshown surface warms up beneath the
// web view and the host raises it once its fresh swapchain has presented, or when
// kWarmupTimeoutMs (250 ms, overlay_surface.cpp) runs out. That timer starts only when
// the host handles this surface's setRect -- after the bridge round trip and a
// synchronous display create, and after every other surface reshowing ahead of it on the
// same UI thread -- so it is not measured from here, and this hold is a margin over it
// rather than a guarantee. Dropping the still before the raise exposes the page backdrop;
// dropping it after is invisible. Raise the host bound and this must follow.
const RELEASE_HOLD_MS = 500;

// Frames to let pass after the still has decoded before the surface may hide, so the
// frame that paints it has been produced rather than merely scheduled.
const PAINT_FRAMES = 2;

// Bound on each step of putting the still on screen (its decode, each frame wait). The
// surface only hides once these finish, so an unbounded step would leave the overlay
// drawn over the menu it is being hidden for -- rAF does not run while the web view is
// hidden, and nothing guarantees a decode settles. A step that runs out just moves on:
// a still that finishes decoding late beats a blank region.
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

export class PreviewFreeze {
  /** PNG data URI of the held frame, or null when the surface is live. */
  frame = $state<string | null>(null);

  /** The element showing `frame`, bound by the dock so capture() can wait on its decode. */
  img = $state<HTMLImageElement | undefined>();

  // Per-operation token. Capture and the release hold are both async, so each one
  // checks it is still the latest before touching `frame`: a capture overtaken by the
  // overlay closing must not paint afterwards, and a hold overtaken by a new capture
  // must not drop the new still.
  #seq = 0;
  #releaseTimer: ReturnType<typeof setTimeout> | undefined;

  /**
   * Grab the current frame and put it on screen. Resolves once the still is decoded in
   * its element and a frame carrying it has been produced (each step bounded), or once
   * the capture has failed or a later operation has overtaken this one. Never rejects.
   * Call BEFORE hiding the surface: the still has to be on screen before the surface
   * leaves it.
   *
   * `canvasUuid` omitted addresses the Default canvas, the same convention every other
   * preview method uses. A failed capture leaves `frame` as it was -- no still, or the
   * one already held from a moment ago -- and is not surfaced: an error toast on
   * right-click would be worse than the thing it reports.
   */
  async capture(canvasUuid?: string): Promise<void> {
    const seq = this.#next();
    let dataUri: string;
    try {
      dataUri = (await obs.call("preview.freeze", canvasUuid ? { canvas: canvasUuid } : {})).dataUri;
    } catch {
      return;
    }
    if (seq !== this.#seq) {
      return;
    }
    this.frame = dataUri;
    await tick();
    if (seq !== this.#seq) {
      return;
    }
    if (this.img) {
      await bounded(this.img.decode());
    }
    for (let i = 0; i < PAINT_FRAMES && seq === this.#seq; i++) {
      await nextFrame();
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
