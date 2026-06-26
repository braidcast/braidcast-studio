import type { AccentName, ThemeMode, ThemeTokens } from "./tokens";

// Density-derived spacing/height: both ship presets use "comfortable" per §5.2.
const COMFORTABLE = { spaceUnit: "8px", controlHeight: "30px" } as const;

// Geist (self-hosted woff2, see app.css @font-face) is the redesign UI/mono stack.
const GEIST_UI = "'Geist', 'Segoe UI', system-ui, sans-serif";
const GEIST_MONO = "'Geist Mono', ui-monospace, monospace";

// Axis 5a — accent. Each entry is [accent, accent-ink]; selecting an accent rewrites
// colorAccent + colorAccentContrast (themeStore.setToken). amber is the mock default.
export const ACCENT_VALUES: Record<AccentName, { accent: string; accentInk: string }> = {
  amber: { accent: "#e7a338", accentInk: "#0a0a0b" },
  blue: { accent: "#3b82f6", accentInk: "#ffffff" },
  violet: { accent: "#8b5cf6", accentInk: "#ffffff" },
  emerald: { accent: "#10b981", accentInk: "#04140e" },
  rose: { accent: "#f43f5e", accentInk: "#ffffff" },
};

// Axis 5b — light/dark. Each entry is the full neutral palette (everything except
// the accent, which composes independently via ACCENT_VALUES). Selecting a mode
// rewrites these nine fields while preserving the chosen accent. Mock values.
type ModePalette = Pick<
  ThemeTokens,
  | "colorBase"
  | "colorRail"
  | "colorSurface"
  | "colorSurface2"
  | "colorBorder"
  | "colorBorder2"
  | "colorText"
  | "colorDim"
  | "colorMuted"
>;

export const MODE_VALUES: Record<ThemeMode, ModePalette> = {
  dark: {
    colorBase: "#0a0a0b",
    colorRail: "#0e0e11",
    colorSurface: "#141418",
    colorSurface2: "#1b1b20",
    colorBorder: "#28282e",
    colorBorder2: "#1e1e23",
    colorText: "#e9e9ec",
    colorDim: "#9a9aa2",
    colorMuted: "#5c5c64",
  },
  light: {
    colorBase: "#e9e9ec",
    colorRail: "#dededf",
    colorSurface: "#f7f7f8",
    colorSurface2: "#ffffff",
    colorBorder: "#d3d3d8",
    colorBorder2: "#e6e6ea",
    colorText: "#18181b",
    colorDim: "#55555c",
    colorMuted: "#8b8b92",
  },
};

// Status palette is mode-independent in the mock. live === bad.
const STATUS = {
  colorLive: "#f04444",
  meterGreen: "#46b85e",
  meterYellow: "#e6b03e",
  meterRed: "#f04444",
} as const;

// The redesign default: mock dark palette + amber accent + Geist + zero radius.
// Built by composing the two axis maps so the neutrals/accent never drift from the
// single source of truth the Appearance UI (7.3) edits.
export const STUDIO_DARK: ThemeTokens = {
  ...MODE_VALUES.dark,
  colorAccent: ACCENT_VALUES.amber.accent,
  colorAccentContrast: ACCENT_VALUES.amber.accentInk,
  ...STATUS,
  fontUi: GEIST_UI,
  fontMono: GEIST_MONO,
  labelCase: "uppercase",
  letterSpacing: "0.06em",
  density: "comfortable",
  ...COMFORTABLE,
  meterStyle: "gradient",
  selectionStyle: "left-bar",
  borderWeight: "1px",
  radius: "0",
  accent: "amber",
  mode: "dark",
};

export const INDUSTRIAL: ThemeTokens = {
  colorBase: "#0a0a0a",
  colorRail: "#0e0e0e",
  colorSurface: "#141414",
  colorSurface2: "#1c1c1c",
  colorBorder: "#333333",
  colorBorder2: "#222222",
  colorText: "#bbbbbb",
  colorDim: "#888888",
  colorMuted: "#666666",
  colorAccent: ACCENT_VALUES.amber.accent,
  colorAccentContrast: ACCENT_VALUES.amber.accentInk,
  colorLive: "#ef4444",
  meterGreen: "#3fae4a",
  meterYellow: "#eab308",
  meterRed: "#ef4444",
  fontUi: "'Consolas', 'SF Mono', ui-monospace, monospace",
  fontMono: "'Consolas', 'SF Mono', ui-monospace, monospace",
  labelCase: "uppercase",
  letterSpacing: "0.1em",
  density: "comfortable",
  ...COMFORTABLE,
  meterStyle: "segmented",
  selectionStyle: "left-bar",
  borderWeight: "1px",
  radius: "0",
  accent: "amber",
  mode: "dark",
};

export const GRAPHITE: ThemeTokens = {
  colorBase: "#0d0f12",
  colorRail: "#090b0e",
  colorSurface: "#171a1f",
  colorSurface2: "#1d2129",
  colorBorder: "#20242b",
  colorBorder2: "#181b21",
  colorText: "#c4cad3",
  colorDim: "#9aa3ae",
  colorMuted: "#7e8794",
  colorAccent: "#3b82f6",
  colorAccentContrast: "#ffffff",
  colorLive: "#ef4444",
  meterGreen: "#22c55e",
  meterYellow: "#eab308",
  meterRed: "#ef4444",
  fontUi: "'Segoe UI', system-ui, sans-serif",
  fontMono: "'Consolas', ui-monospace, monospace",
  labelCase: "uppercase",
  letterSpacing: "0.08em",
  density: "comfortable",
  ...COMFORTABLE,
  meterStyle: "gradient",
  selectionStyle: "left-bar",
  borderWeight: "1px",
  radius: "0",
  accent: "blue",
  mode: "dark",
};

export const SLATE: ThemeTokens = {
  colorBase: "#13161b",
  colorRail: "#0e1116",
  colorSurface: "#1d222a",
  colorSurface2: "#242a33",
  colorBorder: "#2b313b",
  colorBorder2: "#20252d",
  colorText: "#cdd4de",
  colorDim: "#98a0ac",
  colorMuted: "#6b7480",
  colorAccent: "#10b981",
  colorAccentContrast: "#04140e",
  colorLive: "#ef4444",
  meterGreen: "#10b981",
  meterYellow: "#f59e0b",
  meterRed: "#ef4444",
  fontUi: "'Inter', system-ui, sans-serif",
  fontMono: "'Consolas', ui-monospace, monospace",
  labelCase: "none",
  letterSpacing: "0.04em",
  density: "comfortable",
  ...COMFORTABLE,
  meterStyle: "gradient",
  selectionStyle: "fill",
  borderWeight: "1px",
  radius: "0",
  accent: "emerald",
  mode: "dark",
};

export interface PresetEntry {
  id: string;
  name: string;
  tokens: ThemeTokens;
}

// One entry per ship preset. Studio Dark (the mock) is the default (first). Adding a
// preset is a single push here.
export const PRESETS: PresetEntry[] = [
  { id: "studio-dark", name: "Studio Dark", tokens: STUDIO_DARK },
  { id: "industrial", name: "Industrial", tokens: INDUSTRIAL },
  { id: "graphite", name: "Graphite", tokens: GRAPHITE },
  { id: "slate", name: "Slate", tokens: SLATE },
];

export const DEFAULT_PRESET_ID = "studio-dark";

export function presetById(id: string): PresetEntry {
  return PRESETS.find((p) => p.id === id) ?? PRESETS[0];
}

export function defaultTokens(): ThemeTokens {
  return { ...presetById(DEFAULT_PRESET_ID).tokens };
}
