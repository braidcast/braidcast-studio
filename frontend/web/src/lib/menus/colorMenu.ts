import type { ContextMenuItem } from "$lib/menus/ContextMenu.svelte";

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

// Qt offered a live-preview custom color picker; the native <input type=color>
// reproduces it without a bespoke popup. Its value is "#rrggbb" lowercase — the
// exact shape the presets store — so picks pass straight to onPick, no conversion.
// Spawned offscreen and seeded from the current tag: `input` streams live picks
// (swatch updates as the user drags), `change` commits. Removed on commit, or on
// dismissal (deferred so a trailing commit fires first).
function openColorPicker(initial: string, onPick: (color: string) => void): void {
  const input = document.createElement("input");
  input.type = "color";
  input.value = /^#[0-9a-f]{6}$/i.test(initial) ? initial : "#ffffff";
  input.style.cssText = "position:fixed;left:-9999px;width:0;height:0;opacity:0";
  document.body.appendChild(input);
  input.addEventListener("input", () => onPick(input.value));
  input.addEventListener("change", () => {
    onPick(input.value);
    input.remove();
  });
  input.addEventListener("blur", () => setTimeout(() => input.remove(), 0));
  input.click();
}

// A "Color ▸" submenu: a "None" entry (clears the tag), one swatch per preset, then
// a "Custom Color…" live picker and an explicit "Clear". `current` is the item's
// stored hex ("" when unset); picking calls `onPick(hex)`.
export function colorMenu(current: string, onPick: (color: string) => void): ContextMenuItem {
  const norm = (current || "").toLowerCase();
  const presetTokens = new Set(COLORS.map((c) => c.token.toLowerCase()));
  const isCustom = norm !== "" && !presetTokens.has(norm);
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
      null,
      {
        label: "Custom Color…",
        swatch: isCustom ? current : "",
        checked: isCustom,
        action: () => openColorPicker(current, onPick),
      },
      { label: "Clear", swatch: "", disabled: norm === "", action: () => onPick("") },
    ],
  };
}
