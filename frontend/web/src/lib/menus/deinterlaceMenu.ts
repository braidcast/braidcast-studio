import type { ContextMenuItem } from "$lib/menus/ContextMenu.svelte";
import type { DeinterlaceMode, DeinterlaceFieldOrder } from "$lib/api/bridge";

// The 9 OBS deinterlacing modes, in OBS's menu order. Tokens match the bridge
// (sources.get/setDeinterlace `mode`).
const MODES: { token: DeinterlaceMode; label: string }[] = [
  { token: "disable", label: "Disable" },
  { token: "discard", label: "Discard" },
  { token: "retro", label: "Retro" },
  { token: "blend", label: "Blend" },
  { token: "blend2x", label: "Blend (2x)" },
  { token: "linear", label: "Linear" },
  { token: "linear2x", label: "Linear (2x)" },
  { token: "yadif", label: "Yadif" },
  { token: "yadif2x", label: "Yadif (2x)" },
];

const FIELD_ORDERS: { token: DeinterlaceFieldOrder; label: string }[] = [
  { token: "top", label: "Top Field First" },
  { token: "bottom", label: "Bottom Field First" },
];

// A "Deinterlacing ▸" submenu: a checkable mode picker, a divider, then a checkable
// field-order picker. `mode`/`fieldOrder` mark the current selection; picking calls
// the matching callback.
export function deinterlaceMenu(
  mode: DeinterlaceMode,
  fieldOrder: DeinterlaceFieldOrder,
  onMode: (token: DeinterlaceMode) => void,
  onFieldOrder: (token: DeinterlaceFieldOrder) => void,
): ContextMenuItem {
  return {
    label: "Deinterlacing",
    children: [
      ...MODES.map((m) => ({ label: m.label, checked: mode === m.token, action: () => onMode(m.token) })),
      null,
      ...FIELD_ORDERS.map((f) => ({
        label: f.label,
        checked: fieldOrder === f.token,
        action: () => onFieldOrder(f.token),
      })),
    ],
  };
}
