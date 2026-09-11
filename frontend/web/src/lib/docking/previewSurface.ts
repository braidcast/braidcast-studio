// Shared native-preview overlay plumbing for the two docks that host a preview
// surface (the Default PreviewDock and the per-canvas CanvasDock). The native
// overlay is a sibling HWND painted above CEF, addressed by { window, canvas? }:
// the Default surface OMITS canvas (global channel-0 path); a per-canvas dock
// passes its uuid. Only the obs plumbing, the paintability guard, and the
// device-px cursor mapping live here -- each dock keeps its own lifecycle wiring
// (coalescing, output-gating, surface-active flag, menu shape), which genuinely
// differs.

import { obs, type PreviewView } from "$lib/api/bridge";
import { previewViewItems, type PreviewViewAction } from "$lib/menus/previewViewMenu";
import type { ContextMenuItems } from "$lib/menus/ContextMenu.svelte";
import { overlayRectOf } from "$lib/utils/overlayRect";
import { WINDOW_ID } from "$lib/utils/windowContext";

interface PreviewTarget {
  window: number;
  canvas?: string;
}

function target(canvasUuid?: string): PreviewTarget {
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
    .call("preview.setRect", { ...target(canvasUuid), ...rect })
    .catch((e) => console.log("preview.setRect failed: " + (e as Error).message));
  return true;
}

export function hidePreview(canvasUuid?: string): void {
  obs.call("preview.hide", target(canvasUuid)).catch(() => {});
}

export function destroyPreview(canvasUuid?: string): void {
  obs.call("preview.destroy", target(canvasUuid)).catch(() => {});
}

// The surface's view (zoom mode, the scale actually on screen, the edit lock), read
// just before its context menu is built. Null when the host has no surface for this
// dock, which the menu renders as disabled entries rather than as commands that
// would fail on click.
export function fetchPreviewView(canvasUuid?: string): Promise<PreviewView | null> {
  return obs.call("preview.getView", target(canvasUuid)).catch(() => null);
}

// The mutators answer with the surface's state as of the command just applied --
// mode, lock and percentage all already updated, because the host derives the
// percentage from the pending view rather than from the last rendered frame. A
// caller holding a menu open can refresh straight from the reply.
export function applyPreviewViewAction(action: PreviewViewAction, canvasUuid?: string): Promise<PreviewView> {
  return obs.call("preview.viewAction", { ...target(canvasUuid), action });
}

export function setPreviewLocked(locked: boolean, canvasUuid?: string): Promise<PreviewView> {
  return obs.call("preview.setLocked", { ...target(canvasUuid), locked });
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
      ...target(canvasUuid),
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

// The Scale submenu and Lock Preview entries, wired to one surface. Lives here
// rather than in each dock because the wiring is nothing but the target packing
// this module already owns -- two docks had begun to carry identical copies of it,
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
  );
}

// Map an overlay-reported device-pixel cursor to viewport coords via the element
// rect (the overlay HWND reports hits in its own device-px space).
export function mapOverlayCursor(el: HTMLElement, p: { x: number; y: number }): { x: number; y: number } {
  const r = el.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  return { x: r.left + p.x / dpr, y: r.top + p.y / dpr };
}
