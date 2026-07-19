import type { ContextMenuItem } from "$lib/menus/ContextMenu.svelte";

// The 7 OBS blending modes, in OBS's menu order. Tokens match the bridge
// (sceneItems.list `blendMode` field + sceneItems.setBlendingMode `mode` param).
const BLEND_MODES: { token: string; label: string }[] = [
  { token: "normal", label: "Normal" },
  { token: "additive", label: "Additive" },
  { token: "subtract", label: "Subtract" },
  { token: "screen", label: "Screen" },
  { token: "multiply", label: "Multiply" },
  { token: "lighten", label: "Lighten" },
  { token: "darken", label: "Darken" },
];

// The 2 OBS blending methods. Tokens match the bridge (sceneItems.list
// `blendMethod` field + sceneItems.setBlendingMethod `method` param).
const BLEND_METHODS: { token: string; label: string }[] = [
  { token: "default", label: "Default" },
  { token: "srgbOff", label: "SRGB Off" },
];

// A "Blending Mode ▸" submenu entry: one checkable child per mode, checked =
// the item's current mode, picking one calls `onPick(token)`.
export function blendModeMenu(current: string, onPick: (token: string) => void): ContextMenuItem {
  return {
    label: "Blending Mode",
    children: BLEND_MODES.map((m) => ({
      label: m.label,
      checked: current === m.token,
      action: () => onPick(m.token),
    })),
  };
}

// A "Blending Method ▸" submenu entry: one checkable child per method, checked =
// the item's current method, picking one calls `onPick(token)`.
export function blendMethodMenu(current: string, onPick: (token: string) => void): ContextMenuItem {
  return {
    label: "Blending Method",
    children: BLEND_METHODS.map((m) => ({
      label: m.label,
      checked: current === m.token,
      action: () => onPick(m.token),
    })),
  };
}
