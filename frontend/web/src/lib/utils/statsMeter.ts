// Shared stats-meter helpers reused by the Stats dock and the Monitor page: the
// value formatter, the good/warn/crit color grades, the rolling-ring push, and the
// sparkline geometry. The two surfaces differ only in sparkline viewBox size and
// history length, so the geometry takes its dimensions as arguments.

import type { OutputStat } from "$lib/api/bridge";

export const METER_TEXT = "var(--color-text)";
export const METER_GREEN = "var(--meter-green)";
export const METER_YELLOW = "var(--meter-yellow)";
export const METER_RED = "var(--meter-red)";

// FPS leaks a raw float from the engine (60.0000024…); show up to `dp` decimals with
// trailing zeros stripped so a locked 60 reads "60", a variable rate "59.94".
export function fmtNum(n: number, dp: number): string {
  return Number(n.toFixed(dp)).toString();
}

// Resident memory: GB at or above 1024 MB, else MB. `gb` lets each surface pick its
// own unit label ("GB" vs "GB resident").
export function fmtMem(mb: number): { v: string; gb: boolean } {
  return mb >= 1024 ? { v: (mb / 1024).toFixed(1), gb: true } : { v: String(Math.round(mb)), gb: false };
}

// "skipped/total" for the encode-lag reads. The host reports the WORST-coping mix
// rather than a sum, so on a multi-canvas setup the pair has to say which mix it is --
// otherwise it reads as a whole-machine total sitting next to the global render-lag
// count, and a healthy two-canvas stream looks like its encoder lags twice as badly as
// its renderer. Named here so the Stats dock and the Monitor page cannot word it
// differently.
export function fmtEncodeFrames(skipped: number, total: number, mixes: number): string {
  return `${skipped}/${total}` + (mixes > 1 ? ` · worst of ${mixes}` : "");
}

// Baseline neutral, warns as the value climbs (CPU load, frame time).
export function elevated(v: number, warn: number, crit: number): string {
  return v >= crit ? METER_RED : v >= warn ? METER_YELLOW : METER_TEXT;
}

// "Healthy is green" grade for the drop metrics (0 % is a positive signal).
export function grade(v: number, warn: number, crit: number): string {
  return v >= crit ? METER_RED : v >= warn ? METER_YELLOW : METER_GREEN;
}

// Severity thresholds (warn, crit) fed to grade()/elevated() for the network-health
// reads, shared by the Stats dock and the Studio go-live bar. Drop mirrors the
// engine's %-health metrics (render lag / encode skip); congestion is a
// slower-moving network-pressure gauge, so its band sits higher before it reads red.
export const DROP_GRADE: [number, number] = [1, 5];
export const CONG_GRADE: [number, number] = [30, 60];

// Host-load bands for the general-metric reads, shared by the Stats dock and the
// Monitor page so a retune moves both surfaces together.
export const CPU_GRADE: [number, number] = [60, 85];
export const FRAME_GRADE: [number, number] = [20, 40];

// Cumulative read across every output: counts by state, summed dropped frames and
// outgoing bitrate, worst drop % and peak congestion. Consumers take the subset they
// render (the Stats dock summary strip uses all of it, the Studio perf row a slice).
export interface OutputSummary {
  total: number;
  live: number;
  errors: number;
  droppedFrames: number;
  worstDropPct: number;
  bitrateKbps: number;
  maxCongestionPct: number;
}

export function summarizeOutputs(outputs: readonly OutputStat[]): OutputSummary {
  const s: OutputSummary = {
    total: outputs.length,
    live: 0,
    errors: 0,
    droppedFrames: 0,
    worstDropPct: 0,
    bitrateKbps: 0,
    maxCongestionPct: 0,
  };
  for (const o of outputs) {
    if (o.state === "live") {
      s.live++;
    } else if (o.state === "error") {
      s.errors++;
    }
    s.droppedFrames += o.droppedFrames;
    s.bitrateKbps += o.bitrateKbps;
    if (o.dropPct > s.worstDropPct) {
      s.worstDropPct = o.dropPct;
    }
    if (o.congestionPct > s.maxCongestionPct) {
      s.maxCongestionPct = o.congestionPct;
    }
  }
  return s;
}

// Append to a fixed-length ring buffer, dropping the oldest past `max`.
export function pushRing(arr: number[], v: number, max: number): void {
  arr.push(v);
  if (arr.length > max) {
    arr.shift();
  }
}

// Polyline points for a sparkline over `series`, mapped into a `w`×`h` viewBox
// (stretched with preserveAspectRatio="none"). A fixed `domain` pins the vertical
// scale; otherwise it auto-fits with 15% headroom. "" when there is nothing to draw.
export function sparkPoints(
  series: number[],
  domain: [number, number] | undefined,
  w: number,
  h: number,
): string {
  const n = series.length;
  if (n < 2) {
    return "";
  }
  let lo: number;
  let hi: number;
  if (domain) {
    [lo, hi] = domain;
  } else {
    lo = Math.min(...series);
    hi = Math.max(...series);
    if (hi - lo < 1e-6) {
      hi = lo + 1;
      lo = Math.max(0, lo - 1);
    }
    const pad = (hi - lo) * 0.15;
    lo -= pad;
    hi += pad;
  }
  const span = hi - lo || 1;
  return series
    .map((val, i) => {
      const x = (i / (n - 1)) * w;
      const t = Math.max(0, Math.min(1, (val - lo) / span));
      return `${x.toFixed(1)},${(h - t * h).toFixed(1)}`;
    })
    .join(" ");
}

// Close the line into a filled area against the baseline of the `w`×`h` viewBox.
export function sparkArea(pts: string, w: number, h: number): string {
  return pts === "" ? "" : `${pts} ${w},${h} 0,${h}`;
}
