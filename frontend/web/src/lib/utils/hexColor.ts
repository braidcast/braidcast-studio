// A platform-supplied hex color ("#RRGGBB", e.g. a YouTube Super Chat tier color) needs
// two things before it reaches the UI: validating it's safe to hand to CSS as a custom
// property, and picking legible chip text against it, since tiers range from dark blue
// to yellow. The overlay (frontend/web/public/overlay/default-chatbox/template.js) is
// plain JS with no bundler and keeps its own copy of both functions -- reconcile that
// copy if the contrast math changes here.

/** Strict "#RRGGBB", case-insensitive. The only shape a color is trusted through. */
export const HEX_COLOR_RE = /^#[0-9a-fA-F]{6}$/;

function srgbToLinear(c: number): number {
  const v = c / 255;
  return v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4;
}

/** WCAG relative luminance (0 = black .. 1 = white) of a validated "#RRGGBB" string. */
function relativeLuminance(hex: string): number {
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  return 0.2126 * srgbToLinear(r) + 0.7152 * srgbToLinear(g) + 0.0722 * srgbToLinear(b);
}

/** Black or white, whichever contrasts more against a validated "#RRGGBB" background. */
export function readableTextColor(hex: string): "#000000" | "#ffffff" {
  const l = relativeLuminance(hex);
  const contrastWithWhite = 1.05 / (l + 0.05);
  const contrastWithBlack = (l + 0.05) / 0.05;
  return contrastWithBlack >= contrastWithWhite ? "#000000" : "#ffffff";
}
