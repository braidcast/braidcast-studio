// The named easing curves an animation field offers. One row per curve; the engine hands the
// CSS straight to the Web Animations API. Each curve has an exit twin, its time-reversed
// mirror, so "Smooth" eases into place on the way in and eases away on the way out -- the
// mirror of an In preset is then the mirror of its whole motion, not just its keyframes.
//
// Spring and bounce are sampled into CSS `linear()` (Chromium 113+, so every CEF this app
// ships): no cubic-bezier can pass its end point more than once.

export type EasingName = "smooth" | "snappy" | "linear" | "spring" | "overshoot" | "bounce";

/** Points per sampled curve: enough that a 0.25x spring still reads as a curve. */
const kSamples = 40;

type Curve = (t: number) => number;

function sampled(curve: Curve): string {
  const points: string[] = [];
  for (let i = 0; i <= kSamples; i++) {
    points.push(String(Math.round(curve(i / kSamples) * 10000) / 10000));
  }
  return `linear(${points.join(", ")})`;
}

/** The time-reversed curve: what plays forward as `curve` played backward. */
const reversed =
  (curve: Curve): Curve =>
  (t) =>
    1 - curve(1 - t);

/** A damped spring that settles exactly on 1 at t = 1. */
const spring: Curve = (t) => 1 - Math.exp(-6 * t) * Math.cos(4.5 * Math.PI * t);

/** The classic ease-out bounce: three diminishing rebounds off the end point. */
const bounce: Curve = (t) => {
  const n = 7.5625;
  const d = 2.75;
  if (t < 1 / d) return n * t * t;
  if (t < 2 / d) return n * (t - 1.5 / d) * (t - 1.5 / d) + 0.75;
  if (t < 2.5 / d) return n * (t - 2.25 / d) * (t - 2.25 / d) + 0.9375;
  return n * (t - 2.625 / d) * (t - 2.625 / d) + 0.984375;
};

/** A cubic-bezier and its time-reversed twin. */
function bezier(x1: number, y1: number, x2: number, y2: number): { in: string; out: string } {
  const f = (n: number) => String(Math.round(n * 1000) / 1000);
  return {
    in: `cubic-bezier(${f(x1)}, ${f(y1)}, ${f(x2)}, ${f(y2)})`,
    out: `cubic-bezier(${f(1 - x2)}, ${f(1 - y2)}, ${f(1 - x1)}, ${f(1 - y1)})`,
  };
}

export interface EasingSpec {
  label: string;
  /** For In, idle and text layers. */
  in: string;
  /** For the Out layer. */
  out: string;
}

export const EASINGS: Record<EasingName, EasingSpec> = {
  smooth: { label: "Smooth", ...bezier(0.25, 0.1, 0.25, 1) },
  snappy: { label: "Snappy", ...bezier(0.2, 0.9, 0.1, 1) },
  linear: { label: "Linear", in: "linear", out: "linear" },
  spring: { label: "Spring", in: sampled(spring), out: sampled(reversed(spring)) },
  overshoot: { label: "Overshoot", ...bezier(0.34, 1.56, 0.64, 1) },
  bounce: { label: "Bounce", in: sampled(bounce), out: sampled(reversed(bounce)) },
};

export const EASING_NAMES = Object.keys(EASINGS) as EasingName[];

export function isEasingName(v: unknown): v is EasingName {
  return typeof v === "string" && Object.hasOwn(EASINGS, v);
}
