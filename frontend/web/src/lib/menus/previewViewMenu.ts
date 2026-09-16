import type { ContextMenuItem, ContextMenuItems } from "$lib/menus/ContextMenu.svelte";
import type { PreviewOverflowMode, PreviewOverlays, PreviewView } from "$lib/api/bridge";

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

// The overflow modes, one checked row each, in the legacy settings' order of
// increasing reach.
const OVERFLOW_MODES: { mode: PreviewOverflowMode; label: string }[] = [
  { mode: "hidden", label: "Off" },
  { mode: "selection", label: "Selected Sources" },
  { mode: "always", label: "All Sources" },
];

// The on/off guides, one checkable row each.
const GUIDE_TOGGLES: { key: "safeAreas" | "spacingHelpers"; label: string }[] = [
  { key: "safeAreas", label: "Safe Areas" },
  { key: "spacingHelpers", label: "Spacing Guides" },
];

/** A change to some of the preview overlays; absent keys keep their value. */
export type PreviewOverlayPatch = Partial<PreviewOverlays>;

// An "Overflow ▸" submenu: which items show the striped fill over the part of them
// outside the canvas, and whether items with their visibility off count. The last row
// has nothing to act on while overflow is off, so it is disabled then rather than
// silently inert.
export function previewOverflowMenu(
  view: PreviewView | null,
  onSetOverlays: (patch: PreviewOverlayPatch) => void,
): ContextMenuItem {
  return {
    label: "Overflow",
    disabled: !view,
    children: [
      ...OVERFLOW_MODES.map((m) => ({
        label: m.label,
        ...(view ? { checked: view.overflow === m.mode } : {}),
        action: () => onSetOverlays({ overflow: m.mode }),
      })),
      null,
      {
        label: "Include Invisible Sources",
        ...(view ? { checked: view.overflowInvisible } : {}),
        disabled: view?.overflow === "hidden",
        action: () => onSetOverlays({ overflowInvisible: !view?.overflowInvisible }),
      },
    ],
  };
}

// A "Guides ▸" submenu of the canvas guides. A submenu rather than rows beside Lock
// for the reason previewLockItem gives: checkable rows at this level would indent the
// whole menu.
export function previewGuidesMenu(
  view: PreviewView | null,
  onSetOverlays: (patch: PreviewOverlayPatch) => void,
): ContextMenuItem {
  return {
    label: "Guides",
    disabled: !view,
    children: GUIDE_TOGGLES.map((g) => ({
      label: g.label,
      ...(view ? { checked: view[g.key] } : {}),
      action: () => onSetOverlays({ [g.key]: !view?.[g.key] }),
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

// All of the above plus the divider that separates them from the scene-item
// entries above. Shared by the Default preview dock and every canvas dock, so the
// two menus cannot drift apart the way their scene-item entries already have.
export function previewViewItems(
  view: PreviewView | null,
  onAction: (action: PreviewViewAction) => void,
  onSetLocked: (locked: boolean) => void,
  onSetOverlays: (patch: PreviewOverlayPatch) => void,
): ContextMenuItems {
  return [
    null,
    previewScaleMenu(view, onAction),
    previewOverflowMenu(view, onSetOverlays),
    previewGuidesMenu(view, onSetOverlays),
    previewLockItem(view, onSetLocked),
  ];
}
