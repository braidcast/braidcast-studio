// Plays an animation field's value on an element through the Web Animations API. Every
// decision about what a preset looks like lives in presets.ts; this file only resolves a
// stored value to a row, applies the knobs and the reduced-motion rule, and starts it.

import type { TemplatePart } from "../fillTemplate";
import { logOnce } from "../logOnce";
import { EASINGS } from "./easing";
import { normalizeParams, presetIdOf, type AnimParams } from "./params";
import { durationMs, findPreset, LAYER_FALLBACK, type Frame, type Layer, type Preset } from "./presets";
import { textUnits } from "./textFx";

/** Every layer's motion under prefers-reduced-motion: a crossfade this long, or nothing. */
export const REDUCED_MOTION_MS = 200;

/** The row a stored value plays, falling back to the layer's default -- logged once per id,
 * since a fallback is a schema or hand-edit fault worth seeing but not worth a line per alert. */
export function resolvePreset(layer: Layer, value: unknown): Preset {
  const id = presetIdOf(value);
  const found = id ? findPreset(layer, id) : null;
  if (found) {
    return found;
  }
  const fallback = LAYER_FALLBACK[layer];
  if (id) {
    logOnce(`preset|${layer}|${id}`, `unknown ${layer} animation "${id}"; playing "${fallback}" instead`);
  }
  return findPreset(layer, fallback) as Preset;
}

/** The timing a preset and its knobs come to, for the engine and for anything that has to
 * wait on a play (the deck holds a card until its exit has run). */
export function timingOf(layer: Layer, value: unknown): { preset: Preset; params: AnimParams; durationMs: number } {
  const preset = resolvePreset(layer, value);
  const params = normalizeParams(value, preset.defaults);
  return { preset, params, durationMs: durationMs(preset, params.speed) };
}

/** How long a play of `value` on `layer` runs before it is done, delay included: what a
 * caller waits before removing what it animated. 0 for a layer that does nothing. A looping
 * preset answers one loop, which no caller waits on. */
export function playMs(layer: Layer, value: unknown): number {
  if (reducedMotion()) {
    return CROSSFADE[layer] ? REDUCED_MOTION_MS : 0;
  }
  const { preset, params, durationMs: ms } = timingOf(layer, value);
  return preset.keyframes(params).length === 0 ? 0 : params.delayMs + ms;
}

function reducedMotion(): boolean {
  return typeof matchMedia === "function" && matchMedia("(prefers-reduced-motion: reduce)").matches;
}

const CROSSFADE: Partial<Record<Layer, Frame[]>> = {
  in: [{ opacity: 0 }, { opacity: 1 }],
  out: [{ opacity: 1 }, { opacity: 0 }],
};

/** Play `value` on `el` as `layer`. Returns the running Animation, or null when the layer
 * does nothing (a "none" preset, or idle and text under reduced motion). An In or Out play
 * holds its end state (`fill: both`), so a card that has entered stays entered and one that
 * has left stays gone until it is removed. */
export function animate(el: Element, layer: Layer, value: unknown): Animation | null {
  if (reducedMotion()) {
    const frames = CROSSFADE[layer];
    return frames ? el.animate(frames as Keyframe[], { duration: REDUCED_MOTION_MS, fill: "both" }) : null;
  }
  const { preset, params, durationMs: ms } = timingOf(layer, value);
  const frames = preset.keyframes(params);
  if (frames.length === 0 || ms <= 0) {
    return null;
  }
  return el.animate(frames as Keyframe[], {
    duration: ms,
    delay: params.delayMs,
    easing: layer === "out" ? EASINGS[params.easing].out : EASINGS[params.easing].in,
    iterations: preset.loop ? Infinity : 1,
    fill: preset.loop ? "none" : "both",
  });
}

/** Render `parts` into `el` and start the text effect `value` on it. Each animated grapheme
 * gets its own inline-block span; the line's text is unchanged (joining the spans gives it
 * back), so it still selects and copies as one string. Returns the animations started. */
export function applyTextFx(el: HTMLElement, parts: readonly TemplatePart[], value: unknown): Animation[] {
  const text = parts.map((p) => p.text).join("");
  const { preset, params, durationMs: ms } = timingOf("text", value);
  const frames = preset.keyframes(params);
  if (frames.length === 0 || ms <= 0 || reducedMotion()) {
    el.textContent = text;
    return [];
  }
  el.textContent = "";
  const doc = el.ownerDocument;
  const stagger = (preset.staggerMs ?? 0) / params.speed;
  const started: Animation[] = [];
  let i = 0;
  for (const unit of textUnits(parts, params.target)) {
    if (!unit.animate) {
      el.appendChild(doc.createTextNode(unit.text));
      continue;
    }
    const span = doc.createElement("span");
    span.className = "fx-g";
    span.textContent = unit.text;
    el.appendChild(span);
    started.push(
      span.animate(frames as Keyframe[], {
        duration: ms,
        delay: params.delayMs + i * stagger,
        easing: EASINGS[params.easing].in,
        iterations: preset.loop ? Infinity : 1,
        fill: preset.loop ? "none" : "both",
      }),
    );
    i++;
  }
  return started;
}
