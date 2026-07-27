// Shared formatters that were re-declared across the stats/live surfaces.

export const pad2 = (n: number): string => String(n).padStart(2, "0");

// Elapsed time from milliseconds.
//   default  -> compact "h:mm:ss" (hours group dropped when zero): Stats / Monitor
//   fixed    -> always zero-padded "hh:mm:ss": StudioPage live badge
export function fmtDuration(ms: number, opts?: { fixed?: boolean }): string {
  const total = Math.floor(ms / 1000);
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  if (opts?.fixed) {
    return pad2(h) + ":" + pad2(m) + ":" + pad2(s);
  }
  return h > 0 ? `${h}:${pad2(m)}:${pad2(s)}` : `${m}:${pad2(s)}`;
}

// Bitrate: Mb/s at or above 1000 kbps, else kb/s.
export function fmtBitrate(kbps: number): string {
  return kbps >= 1000 ? (kbps / 1000).toFixed(1) + " Mb/s" : Math.round(kbps) + " kb/s";
}

// Compact count for chrome too tight for the full number ("999", "1.9k", "12k",
// "3.4M"): one decimal below ten of a unit, whole above it, so the field never widens
// past four glyphs as the figure ticks.
export function fmtCompact(n: number): string {
  if (n < 1000) {
    return String(n);
  }
  const unit = n < 1_000_000 ? 1000 : 1_000_000;
  const v = n / unit;
  return (v < 10 ? v.toFixed(1) : String(Math.round(v))) + (unit === 1000 ? "k" : "M");
}

// Title-cased label for a lowercase live-state name ("live" -> "Live").
export function titleState(s: string): string {
  return s.charAt(0).toUpperCase() + s.slice(1);
}

// Frame rate from a numerator/denominator pair: a fractional rate (den > 1) reads to
// two decimals (59.94), an integer rate (or a missing/zero den) reads whole (60).
export function fmtFps(num: number, den: number): string {
  if (!(den > 0)) {
    return String(num);
  }
  return den > 1 ? (num / den).toFixed(2) : String(num);
}
