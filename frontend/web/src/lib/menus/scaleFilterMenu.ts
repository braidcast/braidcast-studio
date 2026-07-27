import type { ContextMenuItem } from "$lib/menus/ContextMenu.svelte";
import type { ScaleFilter } from "$lib/api/bridge";

// The 6 OBS scale-filter modes, in OBS's menu order. Tokens match the bridge
// (sceneItems.list `scaleFilter` field + sceneItems.setScaleFilter `filter` param).
const SCALE_FILTERS: { token: ScaleFilter; label: string }[] = [
  { token: "disable", label: "Disable" },
  { token: "point", label: "Point" },
  { token: "bilinear", label: "Bilinear" },
  { token: "bicubic", label: "Bicubic" },
  { token: "lanczos", label: "Lanczos" },
  { token: "area", label: "Area" },
];

// A "Scale Filtering ▸" submenu entry: one checkable child per mode, checked =
// the item's current filter, picking one calls `onPick(token)`.
export function scaleFilterMenu(current: ScaleFilter, onPick: (token: ScaleFilter) => void): ContextMenuItem {
  return {
    label: "Scale Filtering",
    children: SCALE_FILTERS.map((f) => ({
      label: f.label,
      checked: current === f.token,
      action: () => onPick(f.token),
    })),
  };
}
