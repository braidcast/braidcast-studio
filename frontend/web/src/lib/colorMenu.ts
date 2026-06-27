import type { ContextMenuItem } from "./ContextMenu.svelte";

// A small preset palette for per-scene-item color tags. Tokens are the hex values
// stored on the item (sceneItems.list `color` / sceneItems.setColor `color`).
const COLORS: { token: string; label: string }[] = [
  { token: "#e23b3b", label: "Red" },
  { token: "#e8821a", label: "Orange" },
  { token: "#e6c12e", label: "Yellow" },
  { token: "#3fa845", label: "Green" },
  { token: "#2f8fd6", label: "Blue" },
  { token: "#7b56d6", label: "Purple" },
  { token: "#d156b0", label: "Magenta" },
  { token: "#8a939b", label: "Gray" },
];

// A "Color ▸" submenu: a "None" entry (clears the tag) then one swatch per preset.
// `current` is the item's stored hex ("" when unset); picking calls `onPick(hex)`.
export function colorMenu(current: string, onPick: (color: string) => void): ContextMenuItem {
  const norm = (current || "").toLowerCase();
  return {
    label: "Color",
    children: [
      { label: "None", swatch: "", checked: norm === "", action: () => onPick("") },
      null,
      ...COLORS.map((c) => ({
        label: c.label,
        swatch: c.token,
        checked: norm === c.token.toLowerCase(),
        action: () => onPick(c.token),
      })),
    ],
  };
}
