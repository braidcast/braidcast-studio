import { obs } from "$lib/api/bridge";
import { applyTheme, type ThemeTokens } from "$lib/theme/tokens";
import {
  ACCENT_VALUES,
  MODE_VALUES,
  PRESETS,
  DEFAULT_PRESET_ID,
  defaultTokens,
  type PresetEntry,
} from "$lib/theme/presets";

// Persisted theme schema. The store owns it and (de)serializes it into the opaque
// `state` blob carried by theme.save / theme.load. activeTokens is the LIVE token
// object (an edited preset diverges from its built-in source but keeps activeId
// until the user picks another preset or saves).
interface ThemeState {
  activeId: string;
  activeTokens: ThemeTokens;
  customThemes: PresetEntry[];
}

// Density -> derived spacing/control height. Editing the density token also rewrites
// these so the whole UI re-spaces; one row per density (add a density = one entry).
const DENSITY_VALUES: Record<ThemeTokens["density"], { spaceUnit: string; controlHeight: string }> = {
  compact: { spaceUnit: "6px", controlHeight: "26px" },
  comfortable: { spaceUnit: "8px", controlHeight: "30px" },
};

const PERSIST_DEBOUNCE_MS = 300;

// Runes store for the active theme. Editing a token repaints live (applyTheme
// rewrites the CSS vars) and persists (debounced, since color inputs fire rapidly
// on drag). Switching presets / saving customs persists immediately. hydrate()
// restores the saved state on boot. Persistence failures are non-fatal.
class ThemeStore {
  // The LIVE token object. setToken replaces it wholesale (a new identity) so $derived
  // / $effect consumers re-run.
  tokens = $state<ThemeTokens>(defaultTokens());
  activeId = $state(DEFAULT_PRESET_ID);
  customThemes = $state<PresetEntry[]>([]);

  // Built-ins first, then customs. Drives the preset chips row.
  allThemes = $derived([...PRESETS, ...this.customThemes]);

  // Monotonic counter for custom-theme id uniqueness without Date.now()/Math.random.
  private seq = 0;
  private persistTimer: ReturnType<typeof setTimeout> | undefined;

  private applyNow(): void {
    applyTheme(this.tokens);
  }

  setToken<K extends keyof ThemeTokens>(key: K, value: ThemeTokens[K]): void {
    const next: ThemeTokens = { ...this.tokens, [key]: value };
    if (key === "density") {
      const derived = DENSITY_VALUES[value as ThemeTokens["density"]];
      next.spaceUnit = derived.spaceUnit;
      next.controlHeight = derived.controlHeight;
    } else if (key === "accent") {
      // Accent axis: rewrite the accent color + its ink, leaving the neutrals
      // (and the chosen mode) untouched so the two axes compose.
      const acc = ACCENT_VALUES[value as ThemeTokens["accent"]];
      next.colorAccent = acc.accent;
      next.colorAccentContrast = acc.accentInk;
    } else if (key === "mode") {
      // Light/dark axis: swap the whole neutral palette, preserving the accent.
      Object.assign(next, MODE_VALUES[value as ThemeTokens["mode"]]);
    }
    this.tokens = next;
    this.applyNow();
    this.persistDebounced();
  }

  selectPreset(id: string): void {
    const entry = this.allThemes.find((t) => t.id === id) ?? PRESETS[0];
    this.tokens = { ...entry.tokens };
    this.activeId = entry.id;
    this.applyNow();
    this.persist();
  }

  saveCustom(name: string): string {
    const slug = name.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "") || "theme";
    const id = "custom-" + slug + "-" + this.seq++;
    const entry: PresetEntry = { id, name, tokens: { ...this.tokens } };
    this.customThemes = [...this.customThemes, entry];
    this.activeId = id;
    this.persist();
    return id;
  }

  deleteCustom(id: string): void {
    this.customThemes = this.customThemes.filter((t) => t.id !== id);
    if (this.activeId === id) {
      this.selectPreset(DEFAULT_PRESET_ID);
    } else {
      this.persist();
    }
  }

  persist(): void {
    const state: ThemeState = {
      activeId: this.activeId,
      activeTokens: this.tokens,
      customThemes: this.customThemes,
    };
    obs.call("theme.save", { state: JSON.stringify(state) }).catch(() => {
      // non-fatal: theming already applied in-memory
    });
  }

  persistDebounced(): void {
    clearTimeout(this.persistTimer);
    this.persistTimer = setTimeout(() => this.persist(), PERSIST_DEBOUNCE_MS);
  }

  async hydrate(): Promise<void> {
    const res = await obs.call("theme.load").catch(() => null);
    const blob = res && typeof res.state === "string" ? res.state : "";
    if (blob !== "") {
      const read = readThemeState(blob);
      if (read.lossy) {
        // What could not be read would go with the next theme change, which saves the
        // whole state: keep the file aside first. Best-effort, like every theme write.
        await obs.call("theme.quarantine").catch(() => null);
      }
      this.customThemes = read.customThemes;
      this.activeId = read.activeId;
      this.tokens = read.tokens;
      // Keep the seq counter ahead of any restored custom ids so new saves don't collide.
      this.seq = this.customThemes.reduce((m, t) => {
        const n = Number(t.id.slice(t.id.lastIndexOf("-") + 1));
        return Number.isFinite(n) ? Math.max(m, n + 1) : m;
      }, 0);
    }
    this.applyNow();
  }
}

const isPresetEntry = (t: unknown): t is PresetEntry => {
  const e = t as Partial<PresetEntry> | null;
  return !!e && typeof e.id === "string" && typeof e.name === "string" && !!e.tokens && typeof e.tokens === "object";
};

/** A saved theme state read as far as it can be: whatever is unreadable falls back to the
 * default, and `lossy` says something was dropped (an unparseable blob, or a custom theme
 * entry that is not one), which a save would then make permanent. */
export function readThemeState(blob: string): {
  activeId: string;
  tokens: ThemeTokens;
  customThemes: PresetEntry[];
  lossy: boolean;
} {
  let parsed: unknown;
  try {
    parsed = JSON.parse(blob);
  } catch {
    parsed = undefined;
  }
  if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
    return { activeId: DEFAULT_PRESET_ID, tokens: defaultTokens(), customThemes: [], lossy: true };
  }
  const state = parsed as Partial<Record<keyof ThemeState, unknown>>;
  let lossy = false;
  let customThemes: PresetEntry[] = [];
  if (Array.isArray(state.customThemes)) {
    customThemes = state.customThemes.filter(isPresetEntry);
    lossy = customThemes.length !== state.customThemes.length;
  } else if (state.customThemes !== undefined) {
    lossy = true;
  }
  const tokens =
    state.activeTokens && typeof state.activeTokens === "object" && !Array.isArray(state.activeTokens)
      ? (state.activeTokens as Partial<ThemeTokens>)
      : {};
  return {
    activeId: typeof state.activeId === "string" ? state.activeId : DEFAULT_PRESET_ID,
    // Spread over the default preset so a token added after this state was saved is never
    // missing (old/partial blobs stay valid).
    tokens: { ...defaultTokens(), ...tokens },
    customThemes,
    lossy,
  };
}

export const themeStore = new ThemeStore();
