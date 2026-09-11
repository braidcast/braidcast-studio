import type { ContextMenuItem, ContextMenuItems } from "$lib/menus/ContextMenu.svelte";
import type { PreviewView } from "$lib/api/bridge";

/** The Scale-submenu commands. Tokens match the bridge (preview.viewAction `action`). */
export type PreviewViewAction = "scaleToWindow" | "scaleToCanvas" | "zoomIn" | "zoomOut";

// One row per command, in the legacy preview's menu order. `checked` is the
// predicate for the rows that describe a mode rather than a step -- omitted, not
// false, for the stepping rows: ContextMenu decides a row's ARIA role and the
// menu's tick gutter on the key being PRESENT, so a step must not carry it.
const VIEW_ACTIONS: { token: PreviewViewAction; label: string; checked?: (v: PreviewView) => boolean }[] = [
  { token: "scaleToWindow", label: "Scale to Window", checked: (v) => !v.fixed },
  { token: "scaleToCanvas", label: "Scale to Canvas", checked: (v) => v.fixed && v.zoomPercent === 100 },
  { token: "zoomIn", label: "Zoom In" },
  { token: "zoomOut", label: "Zoom Out" },
];

// A "Scale ▸" submenu for one preview surface. `view` is null when the surface
// could not be read, which disables the whole submenu rather than showing commands
// that would silently fail.
export function previewScaleMenu(
  view: PreviewView | null,
  onAction: (action: PreviewViewAction) => void,
): ContextMenuItem {
  return {
    label: view ? `Scale (${view.zoomPercent}%)` : "Scale",
    disabled: !view,
    children: VIEW_ACTIONS.map((a) => ({
      label: a.label,
      ...(a.checked && view ? { checked: a.checked(view) } : {}),
      action: () => onAction(a.token),
    })),
  };
}

// The preview's edit lock, as a label flip rather than a checkable row. The
// checkable form would be equally correct, but ContextMenu reserves the tick
// gutter for a whole menu as soon as any one row declares `checked`, so a single
// toggle would indent every other label in this menu. The flip also matches the
// scene-item "Lock"/"Unlock" entry sitting a few rows above it.
export function previewLockItem(view: PreviewView | null, onSetLocked: (locked: boolean) => void): ContextMenuItem {
  const locked = view?.locked ?? false;
  return {
    label: locked ? "Unlock Preview" : "Lock Preview",
    disabled: !view,
    action: () => onSetLocked(!locked),
  };
}

// Both of the above plus the divider that separates them from the scene-item
// entries above. Shared by the Default preview dock and every canvas dock, so the
// two menus cannot drift apart the way their scene-item entries already have.
export function previewViewItems(
  view: PreviewView | null,
  onAction: (action: PreviewViewAction) => void,
  onSetLocked: (locked: boolean) => void,
): ContextMenuItems {
  return [null, previewScaleMenu(view, onAction), previewLockItem(view, onSetLocked)];
}
