import { describe, expect, test } from "bun:test";
import { readFileSync } from "node:fs";
import { MODE_VALUES, PRESETS } from "$lib/theme/presets";
import {
  STATE_COLOR,
  STATE_TEXT_COLOR,
  TRANSPORT_STATE_TEXT_COLOR,
  textTone,
} from "$lib/theme/stateColors";
import type { ThemeTokens } from "$lib/theme/tokens";

// Contrast guard for the meter tones' text twins (app.css, issue #37): every state a
// label is drawn in clears 4.5:1 on every ground, bare and under the row tints the
// state surfaces put behind it. The mix percentages are read out of the CSS, so this
// cannot drift from what ships. --color-live-text has its own guard (liveContrast).

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


const APP_CSS = readFileSync(new URL("../src/app.css", import.meta.url), "utf8");

function share(name: string): number {
  const m = new RegExp(
    `--${name}-text:\\s*color-mix\\(\\s*in oklab,[\\s\\S]*?(\\d+(?:\\.\\d+)?)%,\\s*var\\(--color-text\\)\\s*\\);`,
  ).exec(APP_CSS);
  if (!m) throw new Error(`could not find --${name}-text in app.css`);
  return Number(m[1]) / 100;
}

type Palette = { name: string; t: ThemeTokens };
const PALETTES: Palette[] = PRESETS.flatMap((p) => [
  { name: `${p.name} (as shipped)`, t: p.tokens },
  ...(["dark", "light"] as const).map((mode) => ({
    name: `${p.name} (${mode})`,
    t: { ...p.tokens, ...MODE_VALUES[mode], mode },
  })),
]);

const GROUNDS = ["colorBase", "colorRail", "colorSurface", "colorSurface2"] as const;
// Bare, the StatsDock row's 7% wash, and the 14% state-tag tint on the Canvases tab.
const TINTS = [0, 0.07, 0.14];
const AA_TEXT = 4.5;

// Each twin's fill, as the theme paints it.
const TONES: Record<string, (t: ThemeTokens) => RGB> = {
  "meter-green": (t) => hex(t.meterGreen),
  "meter-yellow": (t) => hex(t.meterYellow),
  "meter-red": (t) => hex(t.meterRed),
  "meter-orange": (t) => mixSrgb(hex(t.meterRed), 0.5, hex(t.meterYellow)),
};

describe("state text twins", () => {
  for (const [name, fill] of Object.entries(TONES)) {
    test(`--${name}-text clears 4.5:1 on every ground, bare and under its own tint`, () => {
      const p = share(name);
      const misses: string[] = [];
      for (const { name: palette, t } of PALETTES) {
        const ink = mixOklab(fill(t), p, hex(t.colorText));
        for (const g of GROUNDS) {
          for (const tint of TINTS) {
            const r = contrast(ink, mixSrgb(fill(t), tint, hex(t[g])));
            if (r < AA_TEXT) misses.push(`${palette} ${g} +${tint * 100}%: ${r.toFixed(2)}`);
          }
        }
      }
      expect(misses).toEqual([]);
    });
  }

  test("every state a label is drawn in is a twin or already a text color", () => {
    const textColors = new Set([
      "var(--meter-green-text)",
      "var(--meter-yellow-text)",
      "var(--meter-red-text)",
      "var(--meter-orange-text)",
      "var(--color-live-text)",
      "var(--color-muted)",
      "var(--color-text)",
    ]);
    for (const v of [...Object.values(STATE_TEXT_COLOR), ...Object.values(TRANSPORT_STATE_TEXT_COLOR)]) {
      expect(textColors.has(v)).toBe(true);
    }
    expect(textTone(STATE_COLOR.reconnecting)).toBe("var(--meter-orange-text)");
    expect(textTone("var(--meter-yellow)")).toBe("var(--meter-yellow-text)");
  });
});
