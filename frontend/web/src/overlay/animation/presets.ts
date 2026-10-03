// The animation preset table. A preset is a row; adding one is adding a row, never a branch
// in the engine. Keyframes animate transform, opacity and filter only, which the compositor
// runs without layout inside a browser source.
//
// Directions name the way the motion travels: "slide-up" rises into place, "slide-out-up"
// rises out of view. Distances in % are of the animated element's own box.

import type { EasingName } from "./easing";
import { css, lerp, type AnimParams, type Direction, type Knob, type ParamDefaults } from "./params";

export type Layer = "in" | "out" | "idle" | "text";

/** One keyframe. Only compositor-friendly properties. */
export interface Frame {
  transform?: string;
  opacity?: number;
  filter?: string;
  offset?: number;
}

export interface Preset {
  id: string;
  label: string;
  category: string;
  layer: Layer;
  /** In, text: duration at 1x. Out: 0.8x its pair's. Idle: one loop. */
  baseMs: number;
  knobs: readonly Knob[];
  defaults: ParamDefaults;
  /** The directions the direction knob offers, when the preset has one. */
  directions?: readonly Direction[];
  /** Out presets: the In preset this one mirrors, whose duration it takes 0.8x of. */
  pair?: string;
  /** Text presets: delay between graphemes at 1x. */
  staggerMs?: number;
  /** Idle presets and looping text effects repeat while the alert is shown. */
  loop?: boolean;
  /** An empty list means the layer does nothing ("none"). */
  keyframes(p: AnimParams): Frame[];
}

/** The fraction of In duration an Out preset takes by default. */
export const OUT_DURATION_RATIO = 0.8;

const ALL_DIRECTIONS: readonly Direction[] = ["up", "down", "left", "right"];
const SIDEWAYS: readonly Direction[] = ["left", "right"];

/** Unit vector of travel for a direction (screen y grows downward). */
const VEC: Record<Direction, readonly [number, number]> = {
  up: [0, -1],
  down: [0, 1],
  left: [-1, 0],
  right: [1, 0],
};

const translate = (dir: Direction, d: number, unit: string): string =>
  `translate(${css(VEC[dir][0] * d, unit)}, ${css(VEC[dir][1] * d, unit)})`;
/** Where an In motion starts: `d` back along its direction of travel. */
const from = (dir: Direction, d: number, unit: string): string => translate(dir, -d, unit);
/** +1 for a clockwise-reading direction (right), -1 otherwise. */
const turn = (dir: Direction): number => (dir === "left" ? -1 : 1);

const KNOBS_ALL: readonly Knob[] = ["speed", "easing", "intensity", "delay"];
const KNOBS_DIR: readonly Knob[] = ["speed", "easing", "direction", "intensity", "delay"];
const KNOBS_TIME: readonly Knob[] = ["speed", "easing", "delay"];

const d = (easing: EasingName, direction?: Direction, intensity?: number): ParamDefaults => ({
  easing,
  direction,
  intensity,
});

// --- In -----------------------------------------------------------------------------

const slideIn = (dir: Direction): Preset => ({
  id: `slide-${dir}`,
  label: `Slide ${dir}`,
  category: "Slide",
  layer: "in",
  baseMs: 550,
  knobs: KNOBS_ALL,
  defaults: d("snappy"),
  keyframes: (p) => [
    { transform: from(dir, lerp(10, 120, p.intensity), "%"), opacity: 0 },
    { transform: "translate(0, 0)", opacity: 1 },
  ],
});

const bounceIn = (dir: Direction): Preset => ({
  id: `bounce-${dir}`,
  label: `Bounce ${dir}`,
  category: "Bounce",
  layer: "in",
  baseMs: 900,
  knobs: KNOBS_ALL,
  defaults: d("bounce"),
  keyframes: (p) => [
    { transform: from(dir, lerp(20, 150, p.intensity), "%"), opacity: 0, offset: 0 },
    { opacity: 1, offset: 0.2 },
    { transform: "translate(0, 0)", opacity: 1, offset: 1 },
  ],
});

const IN_PRESETS: Preset[] = [
  {
    id: "fade",
    label: "Fade",
    category: "Fade",
    layer: "in",
    baseMs: 500,
    knobs: KNOBS_TIME,
    defaults: d("smooth"),
    keyframes: () => [{ opacity: 0 }, { opacity: 1 }],
  },
  {
    id: "fade-scale",
    label: "Fade and grow",
    category: "Fade",
    layer: "in",
    baseMs: 500,
    knobs: KNOBS_ALL,
    defaults: d("smooth"),
    keyframes: (p) => [
      { transform: `scale(${css(lerp(0.95, 0.6, p.intensity))})`, opacity: 0 },
      { transform: "scale(1)", opacity: 1 },
    ],
  },
  ...ALL_DIRECTIONS.map(slideIn),
  {
    id: "zoom-in",
    label: "Zoom in",
    category: "Zoom",
    layer: "in",
    baseMs: 500,
    knobs: KNOBS_ALL,
    defaults: d("snappy"),
    keyframes: (p) => [
      { transform: `scale(${css(lerp(0.8, 0.1, p.intensity))})`, opacity: 0 },
      { transform: "scale(1)", opacity: 1 },
    ],
  },
  {
    id: "zoom-pop",
    label: "Pop",
    category: "Zoom",
    layer: "in",
    baseMs: 600,
    knobs: KNOBS_ALL,
    defaults: d("smooth"),
    keyframes: (p) => [
      { transform: "scale(0.5)", opacity: 0, offset: 0 },
      { transform: `scale(${css(1 + lerp(0.05, 0.3, p.intensity))})`, opacity: 1, offset: 0.6 },
      { transform: "scale(1)", opacity: 1, offset: 1 },
    ],
  },
  ...ALL_DIRECTIONS.map(bounceIn),
  {
    id: "flip-x",
    label: "Flip down",
    category: "Flip",
    layer: "in",
    baseMs: 650,
    knobs: KNOBS_ALL,
    defaults: d("smooth"),
    keyframes: (p) => [
      { transform: `perspective(40rem) rotateX(${css(lerp(30, 90, p.intensity), "deg")})`, opacity: 0 },
      { transform: "perspective(40rem) rotateX(0deg)", opacity: 1 },
    ],
  },
  {
    id: "flip-y",
    label: "Flip across",
    category: "Flip",
    layer: "in",
    baseMs: 650,
    knobs: KNOBS_ALL,
    defaults: d("smooth"),
    keyframes: (p) => [
      { transform: `perspective(40rem) rotateY(${css(lerp(30, 90, p.intensity), "deg")})`, opacity: 0 },
      { transform: "perspective(40rem) rotateY(0deg)", opacity: 1 },
    ],
  },
  {
    id: "rotate-in",
    label: "Spin in",
    category: "Rotate",
    layer: "in",
    baseMs: 650,
    knobs: KNOBS_DIR,
    directions: SIDEWAYS,
    defaults: d("snappy", "right"),
    keyframes: (p) => [
      {
        transform: `rotate(${css(-turn(p.direction) * lerp(30, 200, p.intensity), "deg")}) scale(${css(lerp(0.9, 0.5, p.intensity))})`,
        opacity: 0,
      },
      { transform: "rotate(0deg) scale(1)", opacity: 1 },
    ],
  },
  {
    id: "drop",
    label: "Drop",
    category: "Bounce",
    layer: "in",
    baseMs: 800,
    knobs: KNOBS_ALL,
    defaults: d("bounce"),
    keyframes: (p) => [
      { transform: `translateY(${css(-lerp(40, 200, p.intensity), "%")})`, opacity: 0, offset: 0 },
      { opacity: 1, offset: 0.15 },
      { transform: "translateY(0%)", opacity: 1, offset: 1 },
    ],
  },
  {
    id: "unfold",
    label: "Unfold",
    category: "Flip",
    layer: "in",
    baseMs: 550,
    knobs: KNOBS_DIR,
    directions: ["up", "left"],
    defaults: d("snappy", "up"),
    keyframes: (p) => {
      const s = css(lerp(0.6, 0, p.intensity));
      return [
        { transform: p.direction === "left" ? `scale(${s}, 1)` : `scale(1, ${s})`, opacity: 0 },
        { transform: "scale(1, 1)", opacity: 1 },
      ];
    },
  },
  {
    id: "blur-in",
    label: "Focus in",
    category: "Fade",
    layer: "in",
    baseMs: 600,
    knobs: KNOBS_ALL,
    defaults: d("smooth"),
    keyframes: (p) => [
      { filter: `blur(${css(lerp(2, 16, p.intensity), "px")})`, opacity: 0 },
      { filter: "blur(0px)", opacity: 1 },
    ],
  },
  {
    id: "glitch-in",
    label: "Glitch",
    category: "Special",
    layer: "in",
    baseMs: 550,
    knobs: KNOBS_DIR,
    directions: SIDEWAYS,
    defaults: d("linear", "right"),
    keyframes: (p) => {
      const a = lerp(0.2, 1.5, p.intensity) * -turn(p.direction);
      const at = (k: number) => `translateX(${css(a * k, "rem")})`;
      return [
        { transform: `${at(1)} skewX(10deg)`, opacity: 0, filter: "hue-rotate(90deg)", offset: 0 },
        { transform: `${at(-1)} skewX(0deg)`, opacity: 1, filter: "hue-rotate(45deg)", offset: 0.2 },
        { transform: `${at(0.5)} skewX(-6deg)`, opacity: 1, filter: "hue-rotate(0deg)", offset: 0.4 },
        { transform: `${at(-0.33)} skewX(0deg)`, opacity: 1, filter: "hue-rotate(0deg)", offset: 0.6 },
        { transform: `${at(0.15)} skewX(0deg)`, opacity: 1, filter: "hue-rotate(0deg)", offset: 0.8 },
        { transform: "translateX(0rem) skewX(0deg)", opacity: 1, filter: "hue-rotate(0deg)", offset: 1 },
      ];
    },
  },
  {
    id: "swing-in",
    label: "Swing",
    category: "Rotate",
    layer: "in",
    baseMs: 800,
    knobs: KNOBS_DIR,
    directions: SIDEWAYS,
    defaults: d("smooth", "right"),
    keyframes: (p) => {
      const a = lerp(10, 45, p.intensity) * turn(p.direction);
      return [
        { transform: `rotate(${css(-a, "deg")})`, opacity: 0, offset: 0 },
        { transform: `rotate(${css(a * 0.4, "deg")})`, opacity: 1, offset: 0.45 },
        { transform: `rotate(${css(-a * 0.15, "deg")})`, opacity: 1, offset: 0.75 },
        { transform: "rotate(0deg)", opacity: 1, offset: 1 },
      ];
    },
  },
];

// --- Out ----------------------------------------------------------------------------

/** An Out row. Its duration is 0.8x its pair's, taken from the In row so the two cannot drift. */
function outPreset(row: Omit<Preset, "layer" | "baseMs">): Preset {
  const pair = IN_PRESETS.find((p) => p.id === row.pair);
  if (!pair) {
    throw new Error(`out preset ${row.id} pairs with unknown in preset ${row.pair}`);
  }
  return { ...row, layer: "out", baseMs: pair.baseMs * OUT_DURATION_RATIO };
}

const slideOut = (dir: Direction): Preset =>
  outPreset({
    id: `slide-out-${dir}`,
    label: `Slide ${dir}`,
    category: "Slide",
    pair: `slide-${dir}`,
    knobs: KNOBS_ALL,
    defaults: d("snappy"),
    keyframes: (p) => [
      { transform: "translate(0, 0)", opacity: 1 },
      { transform: translate(dir, lerp(10, 120, p.intensity), "%"), opacity: 0 },
    ],
  });

const bounceOut = (dir: Direction): Preset =>
  outPreset({
    id: `bounce-out-${dir}`,
    label: `Bounce ${dir}`,
    category: "Bounce",
    pair: `bounce-${dir}`,
    knobs: KNOBS_ALL,
    defaults: d("smooth"),
    keyframes: (p) => [
      { transform: "translate(0, 0)", opacity: 1, offset: 0 },
      { transform: translate(dir, -lerp(4, 20, p.intensity), "%"), opacity: 1, offset: 0.3 },
      { transform: translate(dir, lerp(20, 150, p.intensity), "%"), opacity: 0, offset: 1 },
    ],
  });

const OUT_PRESETS: Preset[] = [
  outPreset({
    id: "fade-out",
    label: "Fade",
    category: "Fade",
    pair: "fade",
    knobs: KNOBS_TIME,
    defaults: d("smooth"),
    keyframes: () => [{ opacity: 1 }, { opacity: 0 }],
  }),
  outPreset({
    id: "fade-scale-out",
    label: "Fade and shrink",
    category: "Fade",
    pair: "fade-scale",
    knobs: KNOBS_ALL,
    defaults: d("smooth"),
    keyframes: (p) => [
      { transform: "scale(1)", opacity: 1 },
      { transform: `scale(${css(lerp(0.95, 0.6, p.intensity))})`, opacity: 0 },
    ],
  }),
  ...ALL_DIRECTIONS.map(slideOut),
  outPreset({
    id: "zoom-out",
    label: "Zoom out",
    category: "Zoom",
    pair: "zoom-in",
    knobs: KNOBS_ALL,
    defaults: d("smooth"),
    keyframes: (p) => [
      { transform: "scale(1)", opacity: 1 },
      { transform: `scale(${css(lerp(1.1, 1.6, p.intensity))})`, opacity: 0 },
    ],
  }),
  outPreset({
    id: "zoom-pop-out",
    label: "Pop",
    category: "Zoom",
    pair: "zoom-pop",
    knobs: KNOBS_ALL,
    defaults: d("smooth"),
    keyframes: (p) => [
      { transform: "scale(1)", opacity: 1, offset: 0 },
      { transform: `scale(${css(1 + lerp(0.05, 0.3, p.intensity))})`, opacity: 1, offset: 0.4 },
      { transform: "scale(0.5)", opacity: 0, offset: 1 },
    ],
  }),
  ...ALL_DIRECTIONS.map(bounceOut),
  outPreset({
    id: "flip-out-x",
    label: "Flip up",
    category: "Flip",
    pair: "flip-x",
    knobs: KNOBS_ALL,
    defaults: d("smooth"),
    keyframes: (p) => [
      { transform: "perspective(40rem) rotateX(0deg)", opacity: 1 },
      { transform: `perspective(40rem) rotateX(${css(lerp(30, 90, p.intensity), "deg")})`, opacity: 0 },
    ],
  }),
  outPreset({
    id: "flip-out-y",
    label: "Flip across",
    category: "Flip",
    pair: "flip-y",
    knobs: KNOBS_ALL,
    defaults: d("smooth"),
    keyframes: (p) => [
      { transform: "perspective(40rem) rotateY(0deg)", opacity: 1 },
      { transform: `perspective(40rem) rotateY(${css(lerp(30, 90, p.intensity), "deg")})`, opacity: 0 },
    ],
  }),
  outPreset({
    id: "rotate-out",
    label: "Spin out",
    category: "Rotate",
    pair: "rotate-in",
    knobs: KNOBS_DIR,
    directions: SIDEWAYS,
    defaults: d("snappy", "right"),
    keyframes: (p) => [
      { transform: "rotate(0deg) scale(1)", opacity: 1 },
      {
        transform: `rotate(${css(turn(p.direction) * lerp(30, 200, p.intensity), "deg")}) scale(${css(lerp(0.9, 0.5, p.intensity))})`,
        opacity: 0,
      },
    ],
  }),
  outPreset({
    id: "drop-out",
    label: "Fall",
    category: "Bounce",
    pair: "drop",
    knobs: KNOBS_ALL,
    defaults: d("smooth"),
    keyframes: (p) => [
      { transform: "translateY(0%) rotate(0deg)", opacity: 1 },
      {
        transform: `translateY(${css(lerp(40, 200, p.intensity), "%")}) rotate(${css(lerp(0, 12, p.intensity), "deg")})`,
        opacity: 0,
      },
    ],
  }),
  outPreset({
    id: "fold",
    label: "Fold",
    category: "Flip",
    pair: "unfold",
    knobs: KNOBS_DIR,
    directions: ["up", "left"],
    defaults: d("snappy", "up"),
    keyframes: (p) => {
      const s = css(lerp(0.6, 0, p.intensity));
      return [
        { transform: "scale(1, 1)", opacity: 1 },
        { transform: p.direction === "left" ? `scale(${s}, 1)` : `scale(1, ${s})`, opacity: 0 },
      ];
    },
  }),
  outPreset({
    id: "blur-out",
    label: "Blur away",
    category: "Fade",
    pair: "blur-in",
    knobs: KNOBS_ALL,
    defaults: d("smooth"),
    keyframes: (p) => [
      { filter: "blur(0px)", opacity: 1 },
      { filter: `blur(${css(lerp(2, 16, p.intensity), "px")})`, opacity: 0 },
    ],
  }),
  outPreset({
    id: "glitch-out",
    label: "Glitch",
    category: "Special",
    pair: "glitch-in",
    knobs: KNOBS_DIR,
    directions: SIDEWAYS,
    defaults: d("linear", "right"),
    keyframes: (p) => {
      const a = lerp(0.2, 1.5, p.intensity) * turn(p.direction);
      const at = (k: number) => `translateX(${css(a * k, "rem")})`;
      return [
        { transform: "translateX(0rem) skewX(0deg)", opacity: 1, filter: "hue-rotate(0deg)", offset: 0 },
        { transform: `${at(-0.15)} skewX(0deg)`, opacity: 1, filter: "hue-rotate(0deg)", offset: 0.2 },
        { transform: `${at(0.33)} skewX(6deg)`, opacity: 1, filter: "hue-rotate(0deg)", offset: 0.4 },
        { transform: `${at(-0.5)} skewX(0deg)`, opacity: 1, filter: "hue-rotate(45deg)", offset: 0.6 },
        { transform: `${at(1)} skewX(-10deg)`, opacity: 1, filter: "hue-rotate(90deg)", offset: 0.8 },
        { transform: `${at(1.5)} skewX(-10deg)`, opacity: 0, filter: "hue-rotate(90deg)", offset: 1 },
      ];
    },
  }),
  outPreset({
    id: "swing-out",
    label: "Swing",
    category: "Rotate",
    pair: "swing-in",
    knobs: KNOBS_DIR,
    directions: SIDEWAYS,
    defaults: d("smooth", "right"),
    keyframes: (p) => {
      const a = lerp(10, 45, p.intensity) * turn(p.direction);
      return [
        { transform: "rotate(0deg)", opacity: 1, offset: 0 },
        { transform: `rotate(${css(-a * 0.3, "deg")})`, opacity: 1, offset: 0.3 },
        { transform: `rotate(${css(a, "deg")})`, opacity: 0, offset: 1 },
      ];
    },
  }),
];

// --- Idle ---------------------------------------------------------------------------

const NONE = (layer: Layer): Preset => ({
  id: "none",
  label: "None",
  category: "None",
  layer,
  baseMs: 0,
  knobs: [],
  defaults: d("linear"),
  keyframes: () => [],
});

const IDLE_PRESETS: Preset[] = [
  NONE("idle"),
  {
    id: "float",
    label: "Float",
    category: "Idle",
    layer: "idle",
    baseMs: 3000,
    loop: true,
    knobs: KNOBS_DIR,
    directions: ALL_DIRECTIONS,
    defaults: d("smooth", "up", 40),
    keyframes: (p) => [
      { transform: "translate(0rem, 0rem)" },
      { transform: translate(p.direction, lerp(0.15, 0.8, p.intensity), "rem") },
      { transform: "translate(0rem, 0rem)" },
    ],
  },
  {
    id: "pulse",
    label: "Pulse",
    category: "Idle",
    layer: "idle",
    baseMs: 1600,
    loop: true,
    knobs: KNOBS_ALL,
    defaults: d("smooth", undefined, 40),
    keyframes: (p) => [
      { transform: "scale(1)" },
      { transform: `scale(${css(1 + lerp(0.01, 0.08, p.intensity))})` },
      { transform: "scale(1)" },
    ],
  },
  {
    id: "glow",
    label: "Glow",
    category: "Idle",
    layer: "idle",
    baseMs: 2000,
    loop: true,
    knobs: KNOBS_ALL,
    defaults: d("smooth", undefined, 50),
    keyframes: (p) => [
      { filter: "drop-shadow(0 0 0px rgba(255, 255, 255, 0))" },
      {
        filter: `drop-shadow(0 0 ${css(lerp(4, 18, p.intensity), "px")} rgba(255, 255, 255, ${css(lerp(0.2, 0.7, p.intensity))}))`,
      },
      { filter: "drop-shadow(0 0 0px rgba(255, 255, 255, 0))" },
    ],
  },
  {
    id: "sway",
    label: "Sway",
    category: "Idle",
    layer: "idle",
    baseMs: 2400,
    loop: true,
    knobs: KNOBS_DIR,
    directions: SIDEWAYS,
    defaults: d("smooth", "right", 40),
    keyframes: (p) => {
      const a = lerp(1, 6, p.intensity) * turn(p.direction);
      return [
        { transform: "rotate(0deg)" },
        { transform: `rotate(${css(a, "deg")})` },
        { transform: "rotate(0deg)" },
        { transform: `rotate(${css(-a, "deg")})` },
        { transform: "rotate(0deg)" },
      ];
    },
  },
  {
    id: "shake",
    label: "Shake",
    category: "Idle",
    layer: "idle",
    baseMs: 700,
    loop: true,
    knobs: KNOBS_ALL,
    defaults: d("linear", undefined, 25),
    keyframes: (p) => {
      const a = lerp(0.05, 0.4, p.intensity);
      return [
        { transform: "translate(0rem, 0rem)" },
        { transform: `translate(${css(-a, "rem")}, 0rem)` },
        { transform: `translate(${css(a, "rem")}, 0rem)` },
        { transform: `translate(${css(-a * 0.5, "rem")}, 0rem)` },
        { transform: "translate(0rem, 0rem)" },
      ];
    },
  },
];

// --- Text ---------------------------------------------------------------------------
// Keyframes for ONE grapheme; the engine staggers them along the text.

const TEXT_PRESETS: Preset[] = [
  NONE("text"),
  {
    id: "wave",
    label: "Wave",
    category: "Text",
    layer: "text",
    baseMs: 900,
    staggerMs: 70,
    loop: true,
    knobs: KNOBS_DIR,
    directions: ["up", "down"],
    defaults: d("smooth", "up", 50),
    keyframes: (p) => [
      { transform: "translateY(0em)" },
      { transform: `translateY(${css((p.direction === "down" ? 1 : -1) * lerp(0.05, 0.4, p.intensity), "em")})` },
      { transform: "translateY(0em)" },
    ],
  },
  {
    id: "typewriter",
    label: "Typewriter",
    category: "Text",
    layer: "text",
    baseMs: 40,
    staggerMs: 55,
    knobs: ["speed", "delay"],
    defaults: d("linear"),
    keyframes: () => [{ opacity: 0 }, { opacity: 1 }],
  },
  {
    id: "pop-letters",
    label: "Pop letters",
    category: "Text",
    layer: "text",
    baseMs: 350,
    staggerMs: 45,
    knobs: KNOBS_ALL,
    defaults: d("overshoot", undefined, 50),
    keyframes: (p) => [
      { transform: "scale(0)", opacity: 0, offset: 0 },
      { transform: `scale(${css(1 + lerp(0.1, 0.5, p.intensity))})`, opacity: 1, offset: 0.7 },
      { transform: "scale(1)", opacity: 1, offset: 1 },
    ],
  },
  {
    id: "shimmer",
    label: "Shimmer",
    category: "Text",
    layer: "text",
    baseMs: 1400,
    staggerMs: 60,
    loop: true,
    knobs: KNOBS_ALL,
    defaults: d("smooth", undefined, 50),
    keyframes: (p) => [
      { filter: "brightness(1)" },
      { filter: `brightness(${css(1 + lerp(0.3, 1.2, p.intensity))})` },
      { filter: "brightness(1)" },
    ],
  },
  {
    id: "rainbow",
    label: "Rainbow",
    category: "Text",
    layer: "text",
    baseMs: 2400,
    staggerMs: 80,
    loop: true,
    knobs: KNOBS_TIME,
    defaults: d("linear"),
    // sepia + saturate first: hue-rotate alone leaves white and grey text unchanged.
    keyframes: () => [
      { filter: "sepia(1) saturate(8) hue-rotate(0deg)" },
      { filter: "sepia(1) saturate(8) hue-rotate(360deg)" },
    ],
  },
  {
    id: "jitter",
    label: "Jitter",
    category: "Text",
    layer: "text",
    baseMs: 300,
    staggerMs: 37,
    loop: true,
    knobs: KNOBS_ALL,
    defaults: d("linear", undefined, 40),
    keyframes: (p) => {
      const a = lerp(0.02, 0.12, p.intensity);
      return [
        { transform: "translate(0em, 0em)" },
        { transform: `translate(${css(a, "em")}, ${css(-a, "em")})` },
        { transform: `translate(${css(-a, "em")}, ${css(a, "em")})` },
        { transform: `translate(${css(a, "em")}, ${css(a, "em")})` },
        { transform: "translate(0em, 0em)" },
      ];
    },
  },
];

export const PRESETS: readonly Preset[] = [...IN_PRESETS, ...OUT_PRESETS, ...IDLE_PRESETS, ...TEXT_PRESETS];

/** What an unknown or missing preset id plays instead, per layer. */
export const LAYER_FALLBACK: Record<Layer, string> = { in: "fade", out: "fade-out", idle: "none", text: "none" };

const BY_LAYER = new Map<Layer, Map<string, Preset>>();
for (const p of PRESETS) {
  let layer = BY_LAYER.get(p.layer);
  if (!layer) {
    layer = new Map();
    BY_LAYER.set(p.layer, layer);
  }
  layer.set(p.id, p);
}

export function presetsFor(layer: Layer): Preset[] {
  return [...(BY_LAYER.get(layer)?.values() ?? [])];
}

/** The preset `id` names on `layer`, or null when it names none there. */
export function findPreset(layer: Layer, id: string): Preset | null {
  return BY_LAYER.get(layer)?.get(id) ?? null;
}

/** Duration of one play (one loop for idle) at `speed`. */
export function durationMs(preset: Preset, speed: number): number {
  return preset.baseMs / speed;
}
