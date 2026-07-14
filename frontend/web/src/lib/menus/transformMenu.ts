import { obs, type TransformAction } from "$lib/api/bridge";
import type { ContextMenuItem } from "$lib/menus/ContextMenu.svelte";
import { openTransform } from "$lib/dialogs/transformOpener.svelte";

// Address for a scene item across the global (no canvas) and per-canvas paths.
// Mirrors TransformTarget but tolerates a null scene from the preview hit payload.
export interface TransformMenuTarget {
  canvas?: string;
  scene?: string | null;
  id: number;
}

// The quick transform verbs the bridge exposes via sceneItems.transformAction,
// ordered to mirror the classic OBS Transform submenu.
const ACTIONS: { label: string; action: TransformAction }[] = [
  { label: "Reset Transform", action: "reset" },
  { label: "Fit to Screen", action: "fitToScreen" },
  { label: "Stretch to Screen", action: "stretchToScreen" },
  { label: "Center to Screen", action: "center" },
  { label: "Rotate 90° CW", action: "rotate90cw" },
  { label: "Rotate 90° CCW", action: "rotate90ccw" },
  { label: "Rotate 180°", action: "rotate180" },
  { label: "Center Vertically", action: "centerVertical" },
  { label: "Center Horizontally", action: "centerHorizontal" },
  { label: "Flip Horizontal", action: "flipH" },
  { label: "Flip Vertical", action: "flipV" },
];

function params(t: TransformMenuTarget): Record<string, unknown> {
  const p: Record<string, unknown> = { id: t.id };
  if (t.canvas != null) {
    p.canvas = t.canvas;
  }
  if (t.scene != null) {
    p.scene = t.scene;
  }
  return p;
}

// A "Transform ▸" submenu: Edit Transform (opens the numeric dialog) plus every
// bridge-backed quick action. `label` names the item in the dialog header. Each
// caller passes its own canvas context so the ops address the right surface.
export function transformMenu(target: TransformMenuTarget, label: string): ContextMenuItem {
  const report = (e: unknown) => console.log("transformAction failed: " + (e as Error).message);
  return {
    label: "Transform",
    children: [
      {
        label: "Edit Transform",
        action: () => openTransform({ canvas: target.canvas, scene: target.scene ?? undefined, id: target.id }, label),
      },
      null,
      ...ACTIONS.map((a) => ({
        label: a.label,
        action: () => void obs.call("sceneItems.transformAction", { ...params(target), action: a.action }).catch(report),
      })),
    ],
  };
}
