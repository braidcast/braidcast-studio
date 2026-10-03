// An animation field's stored value and the knobs it carries. The value is
// { preset, speed?, easing?, direction?, intensity?, delay?, target? }: a knob the user never
// touched is absent and takes the preset's own default, so a better default in a later build
// reaches every widget that left it alone -- the same rule settings follow.

import { clamp } from "../../lib/utils/clamp";
import { isPlainObject } from "../../lib/utils/plainObject";
import { isEasingName, type EasingName } from "./easing";

export type Direction = "up" | "down" | "left" | "right";
export const DIRECTIONS: readonly Direction[] = ["up", "down", "left", "right"];

export type Knob = "speed" | "easing" | "direction" | "intensity" | "delay";

/** Text effects only: whether the whole message moves or just the filled-in variables. */
export type TextTarget = "all" | "vars";

export interface AnimationValue {
  preset: string;
  speed?: number;
  easing?: string;
  direction?: string;
  intensity?: number;
  delay?: number;
  target?: string;
}

/** Knob bounds, in the units the editor shows: speed as a multiplier, intensity in percent,
 * delay in seconds. */
export const KNOB_RANGES = {
  speed: { min: 0.25, max: 3, step: 0.05, fallback: 1 },
  intensity: { min: 0, max: 100, step: 1, fallback: 60 },
  delay: { min: 0, max: 2, step: 0.05, fallback: 0 },
} as const;

/** The knobs as a preset's keyframes read them: intensity as 0..1, delay in ms. */
export interface AnimParams {
  speed: number;
  easing: EasingName;
  direction: Direction;
  intensity: number;
  delayMs: number;
  target: TextTarget;
}

export interface ParamDefaults {
  easing: EasingName;
  direction?: Direction;
  intensity?: number;
}

function knob(v: unknown, range: { min: number; max: number; fallback: number }, fallback?: number): number {
  const n = typeof v === "number" && Number.isFinite(v) ? v : (fallback ?? range.fallback);
  return clamp(n, range.min, range.max);
}

/** A stored value's preset id, or "" when the value carries none. */
export function presetIdOf(value: unknown): string {
  return isPlainObject(value) && typeof value.preset === "string" ? value.preset : "";
}

/** Every knob resolved: the stored value where it is valid, else the preset's default, else
 * the knob's own. Out-of-range numbers clamp rather than fall back, so a hand-edited 5x speed
 * plays at 3x instead of at 1x. */
export function normalizeParams(value: unknown, defaults: ParamDefaults): AnimParams {
  const v = isPlainObject(value) ? value : {};
  const direction =
    typeof v.direction === "string" && (DIRECTIONS as readonly string[]).includes(v.direction)
      ? (v.direction as Direction)
      : (defaults.direction ?? "up");
  return {
    speed: knob(v.speed, KNOB_RANGES.speed),
    easing: isEasingName(v.easing) ? v.easing : defaults.easing,
    direction,
    intensity: knob(v.intensity, KNOB_RANGES.intensity, defaults.intensity) / 100,
    delayMs: knob(v.delay, KNOB_RANGES.delay) * 1000,
    target: v.target === "vars" ? "vars" : "all",
  };
}

/** Linear interpolation between a preset's gentlest and strongest setting. */
export function lerp(lo: number, hi: number, t: number): number {
  return lo + (hi - lo) * t;
}

/** A number for a CSS string: rounded so keyframes never carry float noise like 1e-17. */
export function css(n: number, unit = ""): string {
  return `${Math.round(n * 1000) / 1000}${unit}`;
}
