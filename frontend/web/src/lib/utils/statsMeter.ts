// Shared stats-meter helpers reused by the Stats dock and the Monitor page: the
// value formatter, the good/warn/crit color grades, the rolling-ring push, and the
// sparkline geometry. The two surfaces differ only in sparkline viewBox size and
// history length, so the geometry takes its dimensions as arguments.

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

// Baseline neutral, warns as the value climbs (CPU load, frame time).
export function elevated(v: number, warn: number, crit: number): string {
  return v >= crit ? METER_RED : v >= warn ? METER_YELLOW : METER_TEXT;
}

// "Healthy is green" grade for the drop metrics (0 % is a positive signal).
export function grade(v: number, warn: number, crit: number): string {
  return v >= crit ? METER_RED : v >= warn ? METER_YELLOW : METER_GREEN;
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
