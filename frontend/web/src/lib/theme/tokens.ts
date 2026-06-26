// The §5 token taxonomy as a flat object across the design axes. A theme IS this
// object; switching presets or editing one token rewrites the matching CSS
// custom properties on :root and the whole UI re-skins live. --radius is locked
// to "0" globally; it exists as a token only so a custom theme cannot reintroduce
// rounding by accident.

export type AccentName = "amber" | "blue" | "violet" | "emerald" | "rose";
export type ThemeMode = "dark" | "light";

export interface ThemeTokens {
  // Axis 1 — palette
  colorBase: string;
  colorRail: string;
  colorSurface: string;
  colorSurface2: string;
  colorBorder: string;
  colorBorder2: string;
  colorText: string;
  colorDim: string;
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
  // Axis 5 — variant discriminators. These don't carry a color directly; they
  // select an entry from ACCENT_VALUES / MODE_VALUES which rewrites the palette
  // fields above (see themeStore.setToken). Stored so the active axis survives a
  // reload and the Appearance UI (7.3) can reflect the current choice.
  accent: AccentName;
  mode: ThemeMode;
}

// The token fields that map to a CSS custom property. Excludes the axis
// discriminators (accent/mode): those are string labels, not CSS-usable values, so
// they're applied only as data-* attributes below. Emitting `--accent` as a var would
// also clobber the legacy `--accent` color alias in app.css (var(--accent) consumers
// would compute the string "amber" instead of a color).
type CssTokenKey = Exclude<keyof ThemeTokens, "accent" | "mode">;

// token field -> CSS custom property name. One entry per CSS token; applyTheme loops
// this so adding a token is a single-line change here, not a new assignment.
export const TOKEN_CSS_VARS: Record<CssTokenKey, string> = {
  colorBase: "--color-base",
  colorRail: "--color-rail",
  colorSurface: "--color-surface",
  colorSurface2: "--color-surface-2",
  colorBorder: "--color-border",
  colorBorder2: "--color-border-2",
  colorText: "--color-text",
  colorDim: "--color-dim",
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
// preset switch. --radius is forced to "0" regardless of the incoming value.
// --color-accent-ink is emitted as an alias of colorAccentContrast so mock-derived
// markup (which uses the accent "ink" name) and legacy components share one source.
// The string-enum tokens are also mirrored onto :root as data-* attributes, so
// component CSS can branch on them via attribute selectors (CSS cannot match a
// custom property's string value).
export function applyTheme(tokens: ThemeTokens): void {
  const root = document.documentElement;
  for (const key of Object.keys(TOKEN_CSS_VARS) as CssTokenKey[]) {
    const value = key === "radius" ? "0" : String(tokens[key]);
    root.style.setProperty(TOKEN_CSS_VARS[key], value);
  }
  root.style.setProperty("--color-accent-ink", tokens.colorAccentContrast);
  root.style.colorScheme = tokens.mode === "light" ? "light" : "dark";
  root.dataset.selectionStyle = tokens.selectionStyle;
  root.dataset.meterStyle = tokens.meterStyle;
  root.dataset.density = tokens.density;
  root.dataset.accent = tokens.accent;
  root.dataset.mode = tokens.mode;
}
