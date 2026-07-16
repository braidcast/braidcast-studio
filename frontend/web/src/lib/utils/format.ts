// Shared formatters that were re-declared across the stats/live surfaces.

const pad = (n: number): string => String(n).padStart(2, "0");

// Elapsed time from milliseconds.
//   default  -> compact "h:mm:ss" (hours group dropped when zero): Stats / Monitor
//   fixed    -> always zero-padded "hh:mm:ss": StudioPage live badge
export function fmtDuration(ms: number, opts?: { fixed?: boolean }): string {
  const total = Math.floor(ms / 1000);
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  if (opts?.fixed) {
    return pad(h) + ":" + pad(m) + ":" + pad(s);
  }
  return h > 0 ? `${h}:${pad(m)}:${pad(s)}` : `${m}:${pad(s)}`;
}

// Bitrate: Mb/s at or above 1000 kbps, else kb/s.
export function fmtBitrate(kbps: number): string {
  return kbps >= 1000 ? (kbps / 1000).toFixed(1) + " Mb/s" : Math.round(kbps) + " kb/s";
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
