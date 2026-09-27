// Floating page layers -- menus, dropdowns, popups -- cut out of the native preview.
//
// The preview is a child HWND the OS composites above the whole CEF window, so a menu
// drawn over it would sit underneath. Rather than hiding the surface for a menu, the host
// cuts the menu's rectangle out of it (preview.setCutouts): the menu shows through the
// hole and the video around it stays live. Modals still hide the surface behind a still
// ($lib/stores/previewFreeze).
//
// The hole shows whatever the web view last presented there, so the order of the two
// changes is the whole difficulty (CutoutSet, $lib/docking/previewCutouts):
// - A layer's rect joins the cut only once a frame painted with the layer in it has been
//   presented. Cut earlier and the hole shows the page from before the layer.
// - A layer's rect leaves the cut before the page stops painting the layer. Layers close by
//   unmounting, which takes them off the page in the same task, so a layer released after
//   it has left leaves a likeness of itself painted where it was until the host has closed
//   the hole (holdUntilRestored). A layer the host reported clear of every surface has no
//   hole, and closes at once (mayShowThrough).
// - A layer that changes size gives back the part it lost at once, and cuts the part it
//   gained once that has been presented.
//
// While any layer is open the window's surfaces also hold no pointer input of their own,
// as under a popup's mouse grab: a press on one closes the layers and goes no further
// (preview.layerPress), and hover and the wheel do nothing there.
//
// A layer is assumed to keep its position while open: a move alone is not observed. So is a
// surface: one that moves under an open layer is not reported, and a layer the host found
// clear of it closes at once even if the surface has moved beneath it since.

import { untrack } from "svelte";
import { obs } from "$lib/api/bridge";
import { PAINT_FRAMES, stillOnScreen } from "$lib/docking/freezeFrame";
import { CutoutSet, holdUntilRestored, mayShowThrough, type CutRect } from "$lib/docking/previewCutouts";
import { EV } from "$lib/utils/eventNames";
import { log } from "$lib/utils/log";
import { Cat } from "$lib/utils/logCategories";
import { bounded, elementTimingSource, nextFrame, nextPaintId, watchPresented } from "$lib/utils/presented";
import { WINDOW_ID } from "$lib/utils/windowContext";

/** A layer's hold on its hole. */
export interface PreviewCutout {
  /** Close the layer's hole; resolves once the host has, or once the set stops retrying. Idempotent. */
  release(): Promise<void>;
}

// Readers only care whether any layer is open; the count is what the writers keep.
let openCount = $state(0);
const anyOpen = $derived(openCount > 0);

/** Reactive: true while a floating layer is open over (or away from) a preview. */
export function previewLayerOpen(): boolean {
  return anyOpen;
}

const ELEMENT_TIMING = elementTimingSource();

// A 1x1 fully transparent PNG. Element Timing reports an image, not a box of text, so a
// layer's presentation is observed through one of these painted in the same frame.
const SENTINEL_PNG =
  "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==";

// Before a failed setCutouts is tried again (CutoutSet gives up after SEND_ATTEMPTS).
const RETRY_MS = 100;

// Longest a leaving layer's likeness waits for the host to close its hole. Far over a
// bridge round trip; a host that has not answered by then is not going to.
const LIKENESS_BOUND_MS = 1000;

// Called from a ResizeObserver callback -- after layout, before paint -- so the sentinel
// is painted in the frame that first paints the layer's new shape, or a later one.
async function presented(current: () => boolean): Promise<void> {
  const id = nextPaintId();
  const watch = ELEMENT_TIMING ? watchPresented(id, ELEMENT_TIMING) : null;
  const img = document.createElement("img");
  img.alt = "";
  img.decoding = "sync";
  img.setAttribute("aria-hidden", "true");
  img.setAttribute("elementtiming", id);
  img.style.cssText = "position:fixed;left:0;top:0;width:1px;height:1px;pointer-events:none";
  img.src = SENTINEL_PNG;
  document.body.appendChild(img);
  try {
    await stillOnScreen(
      { presented: watch ? bounded(watch.presented) : null, decode: () => bounded(img.decode()), nextFrame, current },
      PAINT_FRAMES,
    );
  } finally {
    watch?.stop();
    img.remove();
  }
}

const cutouts = new CutoutSet({
  send({ rects, grab }): Promise<boolean[] | null> {
    log.dbg(Cat.preview, `cutouts -> ${rects.length} rect(s), grab ${grab}`);
    return obs.call("preview.setCutouts", { window: WINDOW_ID, dpr: window.devicePixelRatio || 1, rects, grab }).then(
      (reply) => reply.hits,
      (e: unknown) => {
        log.warn(Cat.preview, "preview.setCutouts failed: " + (e as Error).message);
        return null;
      },
    );
  },
  presented,
  retryDelay: () => new Promise((resolve) => setTimeout(resolve, RETRY_MS)),
});

// A page that has just loaded has no layers, whatever a page before it left behind.
void cutouts.resend();

function rectOf(el: HTMLElement): CutRect | null {
  const r = el.getBoundingClientRect();
  return r.width > 0 && r.height > 0 ? { x: r.left, y: r.top, w: r.width, h: r.height } : null;
}

// An inert copy of a layer that has already left the page, painted where it last measured.
// Put first in the layer's old parent while that is still on the page, so it shares the
// layer's stacking context and a layer open now -- the flyout a hover swapped in over the
// same spot -- paints over it; the body otherwise. Pinned to the measured viewport rect
// because the layer may have been placed against an ancestor the copy does not have.
function paintLikeness(el: HTMLElement, rect: CutRect, home: HTMLElement | null): HTMLElement {
  const likeness = el.cloneNode(true) as HTMLElement;
  likeness.removeAttribute("id");
  for (const node of likeness.querySelectorAll("[id]")) {
    node.removeAttribute("id");
  }
  likeness.inert = true;
  likeness.setAttribute("aria-hidden", "true");
  likeness.style.cssText +=
    `;position:fixed;left:${rect.x}px;top:${rect.y}px;right:auto;bottom:auto;` +
    `width:${rect.w}px;height:${rect.h}px;min-width:0;max-width:none;min-height:0;max-height:none;` +
    "margin:0;box-sizing:border-box;transform:none;animation:none;transition:none;" +
    "pointer-events:none;visibility:visible";
  (home?.isConnected ? home : document.body).prepend(likeness);
  // An ancestor with a transform or filter places `fixed` against itself, not the viewport.
  const at = likeness.getBoundingClientRect();
  if (at.left !== rect.x || at.top !== rect.y) {
    likeness.style.left = `${2 * rect.x - at.left}px`;
    likeness.style.top = `${2 * rect.y - at.top}px`;
  }
  return likeness;
}

// A press on a preview surface never reaches the page, so a layer's own outside-click
// handling cannot see it; the host reports it instead, and every layer in this window
// closes as it would for a click anywhere else outside it. A press with no layer open means
// the host still holds a grab the page let go of -- its release never landed -- so the
// page's state is sent again rather than leave the preview dead to the mouse.
const dismissers = new Set<() => void>();

obs.on(EV.previewLayerPress, (p) => {
  if (p.window !== WINDOW_ID) {
    return;
  }
  if (dismissers.size === 0) {
    log.warn(Cat.preview, "a preview press found no open layer; sending the cutouts again");
    void cutouts.resend();
    return;
  }
  for (const dismiss of [...dismissers]) {
    dismiss();
  }
});

/**
 * Cut `el`'s box out of this window's preview surfaces for as long as it is open, starting
 * once it has been presented and following its size, and hold the surfaces' pointer input
 * from now until it is released. `dismiss` closes the layer when the user presses on a
 * preview, as an outside click would. Call once the element is visible; release it from
 * the teardown that removes it, or earlier.
 */
export function cutPreviewUnder(el: HTMLElement, dismiss: () => void): PreviewCutout {
  // Owners call this from an effect and release it from the effect's teardown; nothing in
  // here is a dependency of theirs.
  return untrack(() => {
    const layer = cutouts.add();
    openCount++;
    dismissers.add(dismiss);
    let lastRect = rectOf(el);
    const home = el.parentElement;
    // Device-pixel boxes, so a scale change -- which moves every device-px edge without
    // resizing anything in CSS px -- is observed too, and the set re-sent at the new scale.
    let dpr = window.devicePixelRatio || 1;
    const ro = new ResizeObserver(() => {
      const now = window.devicePixelRatio || 1;
      if (now !== dpr) {
        dpr = now;
        void cutouts.resend();
      }
      lastRect = rectOf(el);
      void cutouts.reshape(layer, lastRect);
    });
    ro.observe(el, { box: "device-pixel-content-box" });

    return {
      release: () =>
        untrack(() => {
          if (layer.released) {
            return layer.released;
          }
          ro.disconnect();
          openCount--;
          dismissers.delete(dismiss);
          const holeOpen = mayShowThrough(layer);
          const restored = cutouts.release(layer);
          if (holeOpen && !el.isConnected && lastRect) {
            const likeness = paintLikeness(el, lastRect, home);
            void holdUntilRestored(restored, () => likeness.remove(), LIKENESS_BOUND_MS);
          }
          return restored;
        }),
    };
  });
}

/** Action form, for a layer that mounts visible and closes by unmounting. */
export function cutoutLayer(el: HTMLElement, dismiss: () => void): { destroy(): void } {
  const cutout = cutPreviewUnder(el, () => dismiss());
  return {
    destroy() {
      void cutout.release();
    },
  };
}
