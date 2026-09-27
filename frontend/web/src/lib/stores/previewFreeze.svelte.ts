// A still of the preview surface, held in the DOM while a modal hides the surface.
//
// The preview is a child HWND the OS composites above the whole CEF window, so a modal
// that overlaps it can only be shown by hiding the surface. This keeps the surface's
// last frame on screen in its place for as long as the modal is up -- the surface's own
// draw at its own device-pixel size, lossless. Against DXGI captures of the live surface
// (SDR, dpr 1) everything but moving video came out byte-identical, so the hand-off shows
// the video stopping. The hand-off in both directions is sequenced by syncPreviewGate in
// $lib/docking/previewSurface. Menus and dropdowns do not hide the surface; they cut
// themselves out of it ($lib/stores/previewCutouts).
//
// The frame does not advance; live video under a modal needs the boundary itself to go
// away (braidcast-notes/preview-architecture.md).

import { tick } from "svelte";
import { obs } from "$lib/api/bridge";
import { frozenFrameFrom, paintStill, type FrozenFrame } from "$lib/docking/freezeFrame";
import type { PreviewTarget } from "$lib/docking/previewSurface";
import { StillHold } from "$lib/docking/stillHold";
import { log } from "$lib/utils/log";
import { Cat } from "$lib/utils/logCategories";
import type { OverlayRect } from "$lib/utils/overlayRect";
import { bounded, elementTimingSource, nextFrame } from "$lib/utils/presented";

// How long the still outlives the gate's release. A reshown surface warms up beneath the
// web view and the host raises it once its fresh swapchain has presented, or when
// kWarmupTimeoutMs (250 ms, overlay_surface.cpp) runs out. That timer starts only when
// the host handles this surface's setRect -- after the bridge round trip and a
// synchronous display create, and after every other surface reshowing ahead of it on the
// same UI thread -- so it is not measured from here, and this hold is a margin over it
// rather than a guarantee. Dropping the still before the raise exposes the page backdrop;
// dropping it after is invisible. Raise the host bound and this must follow.
const RELEASE_HOLD_MS = 500;

const ELEMENT_TIMING = elementTimingSource();

/**
 * The still a dock holds in its hidden surface's place. Which capture or redraw may land,
 * and when the still goes, is StillHold's; this draws one and paints it.
 *
 * capture(): grab the surface's current frame and put it on screen. Resolves once a frame
 * showing the still has been presented -- or, where that cannot be observed, once it has
 * decoded and PAINT_FRAMES have passed -- each step bounded (see paintStill); or once the
 * capture has failed or a later operation has overtaken this one. Never rejects. Call
 * BEFORE hiding the surface: the still has to be on screen before the surface leaves it.
 * `element` is the surface's element rect, as preview.setRect takes it; the host draws the
 * still at the size the surface has there.
 *
 * A failed capture leaves `frame` as it was -- no still, or the one already held from a
 * moment ago -- and is not surfaced: an error toast on opening a modal would be worse than
 * the thing it reports.
 */
export class PreviewFreeze extends StillHold<PreviewTarget> {
  frame = $state.raw<FrozenFrame | null>(null);

  /** The element showing `frame`, bound by the dock so capture() can wait on its decode. */
  img = $state<HTMLImageElement | undefined>();

  /** The `elementtiming` identifier the still's element carries, so its presentation can be told apart. */
  paintId = $state<string | undefined>();

  constructor() {
    super(RELEASE_HOLD_MS);
  }

  protected async draw(target: PreviewTarget, element: OverlayRect, current: () => boolean): Promise<void> {
    let frame: FrozenFrame | null;
    try {
      frame = frozenFrameFrom(await obs.call("preview.freeze", { ...target, ...element }), element);
    } catch {
      return;
    }
    if (!frame || !current()) {
      return;
    }
    const still = frame;
    const shownAt = performance.now();
    const how = await paintStill({
      heldUri: this.frame?.dataUri,
      nextUri: still.dataUri,
      source: ELEMENT_TIMING,
      insert: async (paintId) => {
        this.paintId = paintId;
        this.frame = still;
        await tick();
        return this.img;
      },
      bound: bounded,
      nextFrame,
      current,
    });
    if (how !== "overtaken") {
      log.dbg(Cat.preview, `freeze still on screen (${how}) after ${Math.round(performance.now() - shownAt)} ms`);
    }
  }
}
