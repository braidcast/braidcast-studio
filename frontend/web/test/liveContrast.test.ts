import { describe, expect, test } from "bun:test";
import { readFileSync } from "node:fs";
import { ACCENT_VALUES, MODE_VALUES, PRESETS } from "$lib/theme/presets";
import { STATE_COLOR } from "$lib/theme/stateColors";
import type { ThemeTokens } from "$lib/theme/tokens";

// Contrast guard for the live tone's two inks (app.css): --color-live-text on every
// ground it is drawn over, and --color-live-ink on the solid --color-live fill. The
// mix percentage, the ink and the StatsDock tint percentages are read out of the CSS,
// so this cannot drift from what ships.

type RGB = [number, number, number]; // gamma-encoded sRGB, 0..1

const hex = (h: string): RGB => {
  const m = /^#([0-9a-f]{6})$/i.exec(h);
  if (!m) throw new Error(`not a #rrggbb color: ${h}`);
  return [0, 2, 4].map((i) => parseInt(m[1].slice(i, i + 2), 16) / 255) as RGB;
};
const toLinear = (c: number) => (c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4);
const toGamma = (c: number) => (c <= 0.0031308 ? 12.92 * c : 1.055 * c ** (1 / 2.4) - 0.055);

function toOklab(c: RGB): RGB {
  const [r, g, b] = c.map(toLinear);
  const l = Math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
  const m = Math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
  const s = Math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
  return [
    0.2104542553 * l + 0.793617785 * m - 0.0040720468 * s,
    1.9779984951 * l - 2.428592205 * m + 0.4505937099 * s,
    0.0259040371 * l + 0.7827717662 * m - 0.808675766 * s,
  ];
}

function fromOklab([L, a, b]: RGB): RGB {
  const l = (L + 0.3963377774 * a + 0.2158037573 * b) ** 3;
  const m = (L - 0.1055613458 * a - 0.0638541728 * b) ** 3;
  const s = (L - 0.0894841775 * a - 1.291485548 * b) ** 3;
  return [
    4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
    -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
    -0.0041960863 * l - 0.7034186147 * m + 1.707614701 * s,
  ].map((v) => toGamma(Math.min(1, Math.max(0, v)))) as RGB;
}

// color-mix(in srgb, a p, b), which is also `a` at alpha p composited over `b`.
const mixSrgb = (a: RGB, p: number, b: RGB): RGB => a.map((v, i) => v * p + b[i] * (1 - p)) as RGB;
// color-mix(in oklab, a p, b)
const mixOklab = (a: RGB, p: number, b: RGB): RGB => {
  const A = toOklab(a);
  const B = toOklab(b);
  return fromOklab(A.map((v, i) => v * p + B[i] * (1 - p)) as RGB);
};

const luminance = (c: RGB) => {
  const [r, g, b] = c.map(toLinear);
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
};
const contrast = (a: RGB, b: RGB) => {
  const [hi, lo] = [luminance(a), luminance(b)].sort((x, y) => y - x);
  return (hi + 0.05) / (lo + 0.05);
};

const read = (rel: string) => readFileSync(new URL(rel, import.meta.url), "utf8");
const APP_CSS = read("../src/app.css");
const STATS_DOCK = read("../src/lib/docks/StatsDock.svelte");

function capture(src: string, re: RegExp, what: string): string {
  const m = re.exec(src);
  if (!m) throw new Error(`could not find ${what}`);
  return m[1];
}

// The body of the rule whose selector is exactly `selector`; rules here hold no
// nested braces.
function ruleBody(src: string, selector: string): string {
  const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  const m = new RegExp(`(?:^|\\n)\\s*${escaped} \\{([^}]*)\\}`).exec(src);
  return m ? m[1] : "";
}

// The live percentage of a `color-mix(in srgb, var(--color-live|--dot) N%, ...)`
// background in `selector`'s rule, or null when the rule sets none.
function liveTint(selector: string): number | null {
  const m = /background:\s*color-mix\(in srgb, var\(--(?:color-live|dot)\) (\d+(?:\.\d+)?)%/.exec(
    ruleBody(STATS_DOCK, selector),
  );
  return m ? Number(m[1]) / 100 : null;
}

const LIVE_TEXT_SHARE =
  Number(
    capture(
      APP_CSS,
      /--color-live-text:\s*color-mix\(in oklab, var\(--color-live\) (\d+(?:\.\d+)?)%, var\(--color-text\)\);/,
      "--color-live-text as color-mix(in oklab, var(--color-live) N%, var(--color-text))",
    ),
  ) / 100;
const LIVE_INK = hex(capture(APP_CSS, /--color-live-ink:\s*(#[0-9a-fA-F]{6});/, "--color-live-ink as a #rrggbb"));
const DEFAULT_LIVE = capture(APP_CSS, /--color-live:\s*(#[0-9a-fA-F]{6});/, "--color-live in app.css");

// Every palette a user can be looking at: each preset as shipped, and each preset
// with either mode applied over it (setToken("mode") rewrites the neutrals and keeps
// the preset's live color). The accent axis rewrites only the accent fields.
type Palette = { name: string; t: ThemeTokens };
const PALETTES: Palette[] = PRESETS.flatMap((p) => [
  { name: `${p.name} (as shipped)`, t: p.tokens },
  ...(["dark", "light"] as const).map((mode) => ({
    name: `${p.name} (${mode})`,
    t: { ...p.tokens, ...MODE_VALUES[mode], mode },
  })),
]);

const GROUNDS = ["colorBase", "colorRail", "colorSurface", "colorSurface2"] as const;
const LIVE_TINTS = [0, 0.14, 0.16, 0.22];
const AA_TEXT = 4.5;
const AA_NON_TEXT = 3;

const liveText = (t: ThemeTokens) => mixOklab(hex(t.colorLive), LIVE_TEXT_SHARE, hex(t.colorText));

describe("--color-live-text", () => {
  test("clears 4.5:1 on every ground, bare and under each live tint", () => {
    const misses: string[] = [];
    for (const { name, t } of PALETTES) {
      const ink = liveText(t);
      for (const g of GROUNDS) {
        for (const tint of LIVE_TINTS) {
          const ground = mixSrgb(hex(t.colorLive), tint, hex(t[g]));
          const r = contrast(ink, ground);
          if (r < AA_TEXT) misses.push(`${name} ${g} +${tint * 100}% live: ${r.toFixed(2)}`);
        }
      }
    }
    expect(misses).toEqual([]);
  });

  test("clears 4.5:1 on the StatsDock error cover, at rest and with the row hovered", () => {
    // The row's tint is var(--dot), and an errored row's dot is the live color.
    expect(STATE_COLOR.error).toBe("var(--color-live)");
    const rowRest = liveTint(".row");
    const rowHover = liveTint(".row.err:hover");
    const coverRest = liveTint(".err-cover");
    expect([rowRest, rowHover, coverRest]).not.toContain(null);
    const coverHover = liveTint(".row.err:hover .err-cover") ?? coverRest;

    const misses: string[] = [];
    for (const { name, t } of PALETTES) {
      const live = hex(t.colorLive);
      const ink = liveText(t);
      for (const [state, row, cover] of [
        ["rest", rowRest!, coverRest!],
        ["hover", rowHover!, coverHover!],
      ] as const) {
        const ground = mixSrgb(live, cover, mixSrgb(live, row, hex(t.colorBase)));
        const r = contrast(ink, ground);
        if (r < AA_TEXT) misses.push(`${name} ${state}: ${r.toFixed(2)}`);
      }
    }
    expect(misses).toEqual([]);
  });

  test("clears 3:1 as a glyph over a selected row's accent wash", () => {
    // app.css: 12% accent wash under selectionStyle left-bar, 22% under fill. Text
    // does not clear 4.5:1 there; a live-toned icon glyph owes SC 1.4.11's 3:1.
    const misses: string[] = [];
    for (const { name, t } of PALETTES) {
      const ink = liveText(t);
      for (const [accentName, { accent }] of Object.entries(ACCENT_VALUES)) {
        for (const wash of [0.12, 0.22]) {
          for (const g of GROUNDS) {
            const r = contrast(ink, mixSrgb(hex(accent), wash, hex(t[g])));
            if (r < AA_NON_TEXT) misses.push(`${name} ${accentName} ${wash * 100}% over ${g}: ${r.toFixed(2)}`);
          }
        }
      }
    }
    expect(misses).toEqual([]);
  });
});

describe("--color-live-ink", () => {
  test("clears 4.5:1 on every shipped --color-live fill", () => {
    const fills = new Set([DEFAULT_LIVE, ...PRESETS.map((p) => p.tokens.colorLive)]);
    for (const fill of fills) {
      expect(contrast(LIVE_INK, hex(fill))).toBeGreaterThanOrEqual(AA_TEXT);
    }
  });
});
