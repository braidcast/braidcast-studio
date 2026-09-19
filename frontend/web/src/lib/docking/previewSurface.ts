// Shared native-preview overlay plumbing for the two docks that host a preview
// surface (the Default PreviewDock and the per-canvas CanvasDock). The native
// overlay is a sibling HWND painted above CEF, addressed by { window, canvas? }:
// the Default surface OMITS canvas (global channel-0 path); a per-canvas dock
// passes its uuid. The obs plumbing, the paintability guard, rect reporting, the
// preview-gate hand-off to the freeze frame and the device-px cursor mapping are here;
// each dock keeps what genuinely differs -- coalescing, what blocks its surface, its
// surface-active flag, menu shape.

import { obs, type PreviewView } from "$lib/api/bridge";
import { previewViewItems, type PreviewOverlayPatch, type PreviewViewAction } from "$lib/menus/previewViewMenu";
import type { ContextMenuItems } from "$lib/menus/ContextMenu.svelte";
import type { PreviewFreeze } from "$lib/stores/previewFreeze.svelte";
import { previewSuspended } from "$lib/stores/previewGate.svelte";
import { overlayRectOf } from "$lib/utils/overlayRect";
import { WINDOW_ID } from "$lib/utils/windowContext";

export interface PreviewTarget {
  window: number;
  canvas?: string;
}

// How every surface-addressed bridge call names its surface: this window, plus the canvas
// uuid for a per-canvas dock and nothing for the Default one. The `window` is what a
// detached dock depends on -- omit it and the call lands on window 0's surface instead --
// so callers outside this module use `previewTarget` rather than building the object.
export function previewTarget(canvasUuid?: string): PreviewTarget {
  return canvasUuid ? { canvas: canvasUuid, window: WINDOW_ID } : { window: WINDOW_ID };
}

// Reads the element rect ONCE. When the region isn't paintable (tab-stacked in the
// background, collapsed, or zero-sized) the overlay would otherwise keep painting
// at its stale rect over whatever is now on top -- so hide it and report false.
// Otherwise assert the rect and report true. Callers use the return to drive their
// own "surface active" state.
export function syncPreviewRect(el: HTMLElement, canvasUuid?: string): boolean {
  const rect = overlayRectOf(el);
  if (!rect) {
    hidePreview(canvasUuid);
    return false;
  }
  obs
    .call("preview.setRect", { ...previewTarget(canvasUuid), ...rect })
    .catch((e) => console.log("preview.setRect failed: " + (e as Error).message));
  return true;
}

export function hidePreview(canvasUuid?: string): void {
  obs.call("preview.hide", previewTarget(canvasUuid)).catch(() => {});
}

export function destroyPreview(canvasUuid?: string): void {
  obs.call("preview.destroy", previewTarget(canvasUuid)).catch(() => {});
}

// What a dock hosting a surface tells the shared rect and gate logic about itself.
export interface PreviewSurfaceDock {
  /** The addressed canvas; undefined for the Default surface. */
  readonly canvasUuid: string | undefined;
  /** The dock keeps the surface hidden whatever the layout: output-gated off or disabled. */
  blocked(): boolean;
  /** The dock's hide: the shared hidePreview plus any state the dock mirrors from it. */
  hide(): void;
  /** Told whether a rect report left the surface painting. */
  shown?(shown: boolean): void;
}

// Report the dock's rect to its surface; returns whether the surface was re-asserted
// (and so will paint). Never re-asserts while the preview gate is held -- that would
// raise the native child window back above CEF, over the modal -- and never hides then
// either: hiding for the gate is syncPreviewGate's, which holds the surface up until the
// still is painted. A blocked dock keeps its surface hidden over its placeholder.
export function reportPreviewRect(el: HTMLElement | undefined, dock: PreviewSurfaceDock): boolean {
  if (!el || previewSuspended()) {
    return false;
  }
  if (dock.blocked()) {
    dock.hide();
    return false;
  }
  const shown = syncPreviewRect(el, dock.canvasUuid);
  dock.shown?.(shown);
  return shown;
}

// The body of each dock's preview-gate $effect: hand the region to the freeze still when
// the gate engages and back when it releases. Returns the effect's cleanup.
//
// Engaging, the surface stays up until the still is painted beneath it, and only then is
// it hidden -- so an overlay opening over the preview appears once the still is ready
// rather than before. A failed capture still hides: a usable menu over a blank region
// beats a menu drawn under the preview, so the menu wins. The cleanup cancels that hide,
// because a gate released before the still lands has already had the dock re-assert the
// rect, and a hide issued after it would leave the surface hidden with nothing to show it
// again. A surface not painting when the gate engages (blocked, or no paintable box) has
// nothing to freeze: capturing would only put a still behind the dock's placeholder, and
// for a destroyed Default surface block the host on a composite wait, so it just hides.
//
// Releasing, a re-asserted surface warms up beneath the web view and rises once it has a
// frame, so the still is held until the surface should cover it; a surface that stays
// hidden leaves nothing to cover the region, so the still goes at once and the dock's
// placeholder shows.
export function syncPreviewGate(
  freeze: PreviewFreeze,
  el: HTMLElement | undefined,
  dock: PreviewSurfaceDock,
): (() => void) | undefined {
  if (!previewSuspended()) {
    if (reportPreviewRect(el, dock)) {
      freeze.release();
    } else {
      freeze.clear();
    }
    return undefined;
  }
  if (!el || dock.blocked() || !overlayRectOf(el)) {
    freeze.clear();
    dock.hide();
    return undefined;
  }
  let cancelled = false;
  void freeze.capture(dock.canvasUuid).then(() => {
    if (!cancelled) {
      dock.hide();
    }
  });
  return () => {
    cancelled = true;
  };
}

// The surface's view (zoom mode, the scale the next frame draws at, the edit lock, the
// overlays), read just before its context menu is built. Null when the host has no
// surface for this dock, which the menu renders as disabled entries rather than as
// commands that would fail on click.
export function fetchPreviewView(canvasUuid?: string): Promise<PreviewView | null> {
  return obs.call("preview.getView", previewTarget(canvasUuid)).catch(() => null);
}

// The mutators answer with the surface's state as of the command just applied --
// mode, lock and percentage all already updated, because the host derives the
// percentage from the pending view rather than from the last rendered frame. A
// caller holding a menu open can refresh straight from the reply.
export function applyPreviewViewAction(action: PreviewViewAction, canvasUuid?: string): Promise<PreviewView> {
  return obs.call("preview.viewAction", { ...previewTarget(canvasUuid), action });
}

export function setPreviewLocked(locked: boolean, canvasUuid?: string): Promise<PreviewView> {
  return obs.call("preview.setLocked", { ...previewTarget(canvasUuid), locked });
}

// The overlays are shared by every preview; the target only picks whose view answers.
export function setPreviewOverlays(patch: PreviewOverlayPatch, canvasUuid?: string): Promise<PreviewView> {
  return obs.call("preview.setOverlays", { ...previewTarget(canvasUuid), ...patch });
}

// Forward a wheel the web view received over a preview region to the host's zoom.
// Needed because Windows routes the wheel either to the window under the pointer or
// to the focused one depending on a shell setting, and in the focused case the
// event lands in the web view rather than on the native overlay -- which the user
// experiences as the wheel doing nothing. The overlay keeps its own handler for the
// other routing and for mid-drag, so this is a second entry point, not a
// replacement. Maps viewport CSS px to the overlay's device-px client space, the
// inverse of mapOverlayCursor.
export function forwardPreviewWheel(el: HTMLElement, e: WheelEvent, canvasUuid?: string): void {
  // Suppress the page scroll this would otherwise be; the region is a preview, not
  // a document.
  e.preventDefault();
  if (e.deltaY === 0) {
    return;
  }
  const r = el.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  obs
    .call("preview.zoomAt", {
      ...previewTarget(canvasUuid),
      x: Math.round((e.clientX - r.left) * dpr),
      y: Math.round((e.clientY - r.top) * dpr),
      // Sign only, magnitude discarded -- one notch per event, matching the legacy
      // preview (frontend_old/widgets/OBSBasicPreview.cpp:553-567), which tests the
      // sign of angleDelta().y() and steps one level either way. This does NOT bound
      // a gesture: a precision wheel or trackpad sends many events per gesture and
      // each is worth a full notch here. If an accumulator is ever wanted it has to
      // land here AND in the overlay's native WM_MOUSEWHEEL in the same change, or
      // the two routings give different zoom for the same physical gesture.
      delta: e.deltaY < 0 ? 1 : -1,
    })
    .catch(() => {});
}

// The Scale, Overflow and Guides submenus and the Lock Preview entry, wired to one
// surface. Lives here rather than in each dock because the wiring is nothing but the
// target packing this module already owns -- two docks had begun to carry identical copies of it,
// and a third would have pasted a third.
export function previewViewMenuItems(
  view: PreviewView | null,
  canvasUuid: string | undefined,
  onError: (e: unknown) => void,
): ContextMenuItems {
  return previewViewItems(
    view,
    (action) => void applyPreviewViewAction(action, canvasUuid).catch(onError),
    (locked) => void setPreviewLocked(locked, canvasUuid).catch(onError),
    (patch) => void setPreviewOverlays(patch, canvasUuid).catch(onError),
  );
}

// Map an overlay-reported device-pixel cursor to viewport coords via the element
// rect (the overlay HWND reports hits in its own device-px space).
export function mapOverlayCursor(el: HTMLElement, p: { x: number; y: number }): { x: number; y: number } {
  const r = el.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  return { x: r.left + p.x / dpr, y: r.top + p.y / dpr };
}
