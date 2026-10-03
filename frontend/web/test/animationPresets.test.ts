import { describe, expect, test } from "bun:test";
import { EASING_NAMES, EASINGS } from "../src/overlay/animation/easing";
import { DIRECTIONS, KNOB_RANGES, normalizeParams, type AnimParams } from "../src/overlay/animation/params";
import {
  durationMs,
  findPreset,
  LAYER_FALLBACK,
  OUT_DURATION_RATIO,
  PRESETS,
  presetsFor,
  type Frame,
  type Preset,
} from "../src/overlay/animation/presets";
import { timingOf } from "../src/overlay/animation/engine";

const ALLOWED = new Set(["transform", "opacity", "filter", "offset"]);

// Every combination of knob extremes a preset can be handed.
function extremes(p: Preset): AnimParams[] {
  const out: AnimParams[] = [];
  for (const speed of [KNOB_RANGES.speed.min, KNOB_RANGES.speed.max]) {
    for (const intensity of [KNOB_RANGES.intensity.min, KNOB_RANGES.intensity.max]) {
      for (const direction of p.directions ?? DIRECTIONS) {
        for (const easing of EASING_NAMES) {
          out.push(normalizeParams({ preset: p.id, speed, intensity, direction, easing, delay: KNOB_RANGES.delay.max }, p.defaults));
        }
      }
    }
  }
  return out;
}

function frameProblem(f: Frame): string | null {
  for (const [k, v] of Object.entries(f)) {
    if (!ALLOWED.has(k)) return `animates ${k}`;
    if (typeof v === "number" && !Number.isFinite(v)) return `${k} is ${v}`;
    if (typeof v === "string" && /NaN|Infinity|undefined/.test(v)) return `${k} is "${v}"`;
  }
  if (f.opacity !== undefined && (f.opacity < 0 || f.opacity > 1)) return `opacity ${f.opacity}`;
  if (f.offset !== undefined && (f.offset < 0 || f.offset > 1)) return `offset ${f.offset}`;
  return null;
}

describe("animation presets", () => {
  test("the four layers carry the spec's preset counts", () => {
    expect(presetsFor("in").length).toBe(20);
    expect(presetsFor("out").length).toBe(20);
    expect(presetsFor("idle").map((p) => p.id)).toEqual(["none", "float", "pulse", "glow", "sway", "shake"]);
    expect(presetsFor("text").map((p) => p.id)).toEqual(["none", "wave", "typewriter", "pop-letters", "shimmer", "rainbow", "jitter"]);
  });

  test("ids are unique within a layer, and every layer's fallback exists", () => {
    for (const layer of ["in", "out", "idle", "text"] as const) {
      const ids = presetsFor(layer).map((p) => p.id);
      expect(new Set(ids).size).toBe(ids.length);
      expect(findPreset(layer, LAYER_FALLBACK[layer])).not.toBeNull();
    }
  });

  test("the migration's burst exits all exist", () => {
    for (const id of ["slide-out-left", "flip-out-x", "drop-out", "fade-out", "zoom-out"]) {
      expect([id, findPreset("out", id) !== null]).toEqual([id, true]);
    }
  });

  test("every preset yields valid keyframes at every knob extreme", () => {
    for (const p of PRESETS) {
      for (const params of extremes(p)) {
        const frames = p.keyframes(params);
        if (p.id === "none") {
          expect(frames).toEqual([]);
          continue;
        }
        expect([p.layer, p.id, frames.length >= 2]).toEqual([p.layer, p.id, true]);
        const offsets = frames.map((f) => f.offset).filter((o): o is number => o !== undefined);
        expect([p.id, [...offsets].sort((a, b) => a - b)]).toEqual([p.id, offsets]);
        for (const f of frames) {
          expect([p.layer, p.id, frameProblem(f)]).toEqual([p.layer, p.id, null]);
        }
        const ms = durationMs(p, params.speed);
        expect([p.id, Number.isFinite(ms) && ms > 0]).toEqual([p.id, true]);
      }
    }
  });

  test("an Out preset defaults to 0.8x the duration of the In preset it mirrors", () => {
    for (const out of presetsFor("out")) {
      const pair = findPreset("in", out.pair ?? "");
      expect([out.id, pair !== null]).toEqual([out.id, true]);
      expect(timingOf("out", { preset: out.id }).durationMs).toBeCloseTo(timingOf("in", { preset: pair!.id }).durationMs * OUT_DURATION_RATIO, 6);
    }
  });

  test("speed divides duration, and out-of-range knobs clamp", () => {
    expect(timingOf("in", { preset: "fade", speed: 2 }).durationMs).toBe(250);
    expect(timingOf("in", { preset: "fade", speed: 50 }).durationMs).toBeCloseTo(500 / 3, 6);
    expect(timingOf("in", { preset: "fade", delay: -4 }).params.delayMs).toBe(0);
  });

  test("an unknown preset plays its layer's default", () => {
    expect(timingOf("in", { preset: "no-such" }).preset.id).toBe("fade");
    expect(timingOf("out", { preset: "fade" }).preset.id).toBe("fade-out");
    expect(timingOf("idle", null).preset.id).toBe("none");
  });

  test("every easing has an In and an Out curve", () => {
    for (const name of EASING_NAMES) {
      expect(EASINGS[name].in.length > 0 && EASINGS[name].out.length > 0).toBe(true);
    }
    expect(EASINGS.spring.in.startsWith("linear(0, ")).toBe(true);
    expect(EASINGS.spring.in.endsWith(", 1)")).toBe(true);
    expect(EASINGS.bounce.out.startsWith("linear(0, ")).toBe(true);
  });
});
