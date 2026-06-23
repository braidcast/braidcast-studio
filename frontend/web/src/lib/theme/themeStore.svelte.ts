import { obs } from "../bridge";
import { applyTheme, type ThemeTokens } from "./tokens";
import { INDUSTRIAL, PRESETS, DEFAULT_PRESET_ID, type PresetEntry } from "./presets";

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
  tokens = $state<ThemeTokens>({ ...INDUSTRIAL });
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
    try {
      const res = await obs.call("theme.load").catch(() => null);
      if (res && typeof res.state === "string" && res.state !== "") {
        const parsed = JSON.parse(res.state) as Partial<ThemeState>;
        if (Array.isArray(parsed.customThemes)) {
          this.customThemes = parsed.customThemes.filter(
            (t): t is PresetEntry =>
              !!t && typeof t.id === "string" && typeof t.name === "string" && !!t.tokens && typeof t.tokens === "object",
          );
        }
        if (typeof parsed.activeId === "string") {
          this.activeId = parsed.activeId;
        }
        // Spread over INDUSTRIAL so a token added after this state was saved is
        // never missing (old/partial blobs stay valid).
        this.tokens = { ...INDUSTRIAL, ...(parsed.activeTokens ?? {}) };
        // Keep the seq counter ahead of any restored custom ids so new saves don't collide.
        this.seq = this.customThemes.reduce((m, t) => {
          const n = Number(t.id.slice(t.id.lastIndexOf("-") + 1));
          return Number.isFinite(n) ? Math.max(m, n + 1) : m;
        }, 0);
      }
    } catch {
      // malformed/old/empty state -> defaults
      this.tokens = { ...INDUSTRIAL };
      this.activeId = DEFAULT_PRESET_ID;
      this.customThemes = [];
    }
    this.applyNow();
  }
}

export const themeStore = new ThemeStore();
