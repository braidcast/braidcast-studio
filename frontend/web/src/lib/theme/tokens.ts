// The §5 token taxonomy as a flat object across four axes. A theme IS this
// object; switching presets or editing one token rewrites the matching CSS
// custom properties on :root and the whole UI re-skins live. --radius is locked
// to "0" globally; it exists as a token only so a custom theme cannot reintroduce
// rounding by accident.

export interface ThemeTokens {
  // Axis 1 — palette
  colorBase: string;
  colorSurface: string;
  colorBorder: string;
  colorText: string;
  colorMuted: string;
  colorAccent: string;
  colorAccentContrast: string;
  colorLive: string;
  meterGreen: string;
  meterYellow: string;
  meterRed: string;
  // Axis 2 — typography
  fontUi: string;
  fontMono: string;
  labelCase: "none" | "uppercase";
  letterSpacing: string;
  // Axis 3 — density
  density: "compact" | "comfortable";
  spaceUnit: string;
  controlHeight: string;
  // Axis 4 — element styles
  meterStyle: "gradient" | "segmented";
  selectionStyle: "left-bar" | "fill";
  borderWeight: string;
  radius: "0";
}

// token field -> CSS custom property name. One entry per token; applyTheme loops
// this so adding a token is a single-line change here, not a new assignment.
export const TOKEN_CSS_VARS: Record<keyof ThemeTokens, string> = {
  colorBase: "--color-base",
  colorSurface: "--color-surface",
  colorBorder: "--color-border",
  colorText: "--color-text",
  colorMuted: "--color-muted",
  colorAccent: "--color-accent",
  colorAccentContrast: "--color-accent-contrast",
  colorLive: "--color-live",
  meterGreen: "--meter-green",
  meterYellow: "--meter-yellow",
  meterRed: "--meter-red",
  fontUi: "--font-ui",
  fontMono: "--font-mono",
  labelCase: "--label-case",
  letterSpacing: "--letter-spacing",
  density: "--density",
  spaceUnit: "--space-unit",
  controlHeight: "--control-height",
  meterStyle: "--meter-style",
  selectionStyle: "--selection-style",
  borderWeight: "--border-weight",
  radius: "--radius",
};

// Write every token to :root as a CSS variable. Idempotent; safe to call on every
// preset switch. --radius is forced to "0" regardless of the incoming value. The
// three string-enum tokens are also mirrored onto :root as data-* attributes, so
// component CSS can branch on them via attribute selectors (CSS cannot match a
// custom property's string value).
export function applyTheme(tokens: ThemeTokens): void {
  const root = document.documentElement;
  for (const key of Object.keys(TOKEN_CSS_VARS) as (keyof ThemeTokens)[]) {
    const value = key === "radius" ? "0" : String(tokens[key]);
    root.style.setProperty(TOKEN_CSS_VARS[key], value);
  }
  root.dataset.selectionStyle = tokens.selectionStyle;
  root.dataset.meterStyle = tokens.meterStyle;
  root.dataset.density = tokens.density;
}
