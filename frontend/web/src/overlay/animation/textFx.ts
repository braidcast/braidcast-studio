// Splitting a message into the units a text effect animates. Graphemes, not code units or
// code points: a letter effect must never split an emoji, a ZWJ family or a letter from its
// combining accent. Intl.Segmenter ships in every CEF this app runs (Chromium 87+); the
// code-point fallback only exists so a test runtime or an odd embedder degrades rather than
// throws.

import type { TemplatePart } from "../fillTemplate";
import type { TextTarget } from "./params";

let segmenter: Intl.Segmenter | null | undefined;

export function splitGraphemes(text: string): string[] {
  if (segmenter === undefined) {
    segmenter = typeof Intl !== "undefined" && "Segmenter" in Intl ? new Intl.Segmenter(undefined, { granularity: "grapheme" }) : null;
  }
  return segmenter ? Array.from(segmenter.segment(text), (s) => s.segment) : Array.from(text);
}

/** One run of the rendered line: animated graphemes stand alone, everything else stays a
 * single run. Joining every unit's text gives back the line exactly. */
export interface TextUnit {
  text: string;
  animate: boolean;
}

const WHITESPACE = /^\s+$/;

/** The units for `parts` under `target`: every part ("all") or only the filled-in variables
 * ("vars") is split into graphemes. Whitespace never animates -- an inline-block space
 * collapses to nothing, which would close the gaps between words. */
export function textUnits(parts: readonly TemplatePart[], target: TextTarget): TextUnit[] {
  const units: TextUnit[] = [];
  const pushStill = (text: string) => {
    const last = units[units.length - 1];
    if (last && !last.animate) {
      last.text += text;
    } else {
      units.push({ text, animate: false });
    }
  };
  for (const part of parts) {
    if (target === "vars" && part.key === null) {
      pushStill(part.text);
      continue;
    }
    for (const g of splitGraphemes(part.text)) {
      if (WHITESPACE.test(g)) {
        pushStill(g);
      } else {
        units.push({ text: g, animate: true });
      }
    }
  }
  return units;
}
