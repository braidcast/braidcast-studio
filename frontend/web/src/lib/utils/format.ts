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

// Names a set in a sentence: "Twitch", "Twitch and Kick", "Twitch, Kick and 2 more". One shape
// for every surface that lists destinations or fields, so they cannot drift apart on how many
// they spell out before collapsing the rest into a count.
export function joinNames(items: string[], limit = 3): string {
  const parts = items.length > limit ? [...items.slice(0, limit), `${items.length - limit} more`] : items;
  if (parts.length < 2) {
    return parts[0] ?? "";
  }
  return parts.slice(0, -1).join(", ") + " and " + parts[parts.length - 1];
}

const MINUTE_MS = 60_000;
const HOUR_MS = 60 * MINUTE_MS;
const DAY_MS = 24 * HOUR_MS;

// A timestamp fmtSince and fmtChatTime both treat as real: finite and after the epoch.
// Shared so a consumer gating something else on the same "is this a real timestamp"
// question (e.g. a tooltip next to a formatted cell) can't drift from what the
// formatters themselves consider missing.
export function isRealTimestamp(ms: number): boolean {
  return Number.isFinite(ms) && ms > 0;
}

// Past this many calendar days a count of days stops locating anything, so the date
// itself is the shorter read.
const RELATIVE_DAY_LIMIT = 7;

// Local midnight for a timestamp, so "how many days ago" is answered in calendar days
// rather than in 24-hour blocks: 23:50 last night is yesterday at 00:10 this morning,
// which is what someone reading a date means by it.
function startOfDay(ms: number): number {
  const d = new Date(ms);
  d.setHours(0, 0, 0, 0);
  return d.getTime();
}

// Short human age of an epoch-ms timestamp: relative while elapsed units still locate
// it ("just now", "5m ago", "3h ago", "yesterday", "4d ago"), the date itself once they
// stop. "" for a missing or unset timestamp, so a caller can substitute its own dash
// rather than print an age counted from the epoch.
//
// `nowMs` is a parameter so a list renders every row against one clock read, and so the
// output is reproducible without stubbing the clock.
//
// fmtChatTime below is the other relative-time formatter and is deliberately NOT this one
// with different words. This one LOCATES a moment and so needs a day tier and a date tier;
// that one is a fixed-width column in a scrolling feed, bounded at five characters with no
// date tier at all, which is what keeps a row from reflowing as it ticks. Merging them
// would cost one of those two properties. They share MINUTE_MS/HOUR_MS, so the buckets
// themselves exist once.
export function fmtSince(ms: number, nowMs = Date.now()): string {
  if (!isRealTimestamp(ms)) {
    return "";
  }
  // A host clock a moment ahead of this read gives a negative elapsed; it must not
  // render as a time in the future.
  const elapsed = nowMs - ms;
  if (elapsed < MINUTE_MS) {
    return "just now";
  }
  if (elapsed < HOUR_MS) {
    return Math.floor(elapsed / MINUTE_MS) + "m ago";
  }
  if (elapsed < DAY_MS) {
    return Math.floor(elapsed / HOUR_MS) + "h ago";
  }
  // Rounded, because a DST boundary makes one of these days 23 or 25 hours long.
  const days = Math.round((startOfDay(nowMs) - startOfDay(ms)) / DAY_MS);
  if (days <= 1) {
    return "yesterday";
  }
  if (days < RELATIVE_DAY_LIMIT) {
    return days + "d ago";
  }
  const then = new Date(ms);
  const sameYear = then.getFullYear() === new Date(nowMs).getFullYear();
  return then.toLocaleDateString(undefined, {
    day: "numeric",
    month: "short",
    year: sameYear ? undefined : "numeric",
  });
}

// Chat-row age: answers "am I late replying" rather than "what time is it", so a bare
// count reads faster than fmtSince's "5m ago" when it repeats on every line of a
// scrolling feed. Past the point where a minute count stops being the useful number,
// the clock time itself answers better than a growing count would -- and unlike
// fmtSince there is no day-count/date tier, so the output is always "now", "<60m", or
// "HH:MM": three shapes bounded at 5 characters, which is what lets the caller give
// the value a fixed-width column that never reflows the row as it ticks upward.
// Shares MINUTE_MS/HOUR_MS with fmtSince rather than re-deriving the buckets.
export function fmtChatTime(ms: number, nowMs = Date.now()): string {
  if (!isRealTimestamp(ms)) {
    return "";
  }
  const elapsed = nowMs - ms;
  if (elapsed < MINUTE_MS) {
    return "now";
  }
  if (elapsed < HOUR_MS) {
    return Math.floor(elapsed / MINUTE_MS) + "m";
  }
  return new Date(ms).toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit", hourCycle: "h23" });
}

// Frame rate from a numerator/denominator pair: a fractional rate (den > 1) reads to
// two decimals (59.94), an integer rate (or a missing/zero den) reads whole (60).
export function fmtFps(num: number, den: number): string {
  if (!(den > 0)) {
    return String(num);
  }
  return den > 1 ? (num / den).toFixed(2) : String(num);
}
