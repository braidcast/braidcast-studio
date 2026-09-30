// How a capture-rate row reads, shared by the Stats dock and the Monitor page so the
// two cannot word one reading differently. The rules are the host's (see
// frontend/src/diag/capture_rate.hpp); this only chooses words for them:
//   - WGC and DXGI count a frame only when the screen changes, so their rate is a
//     fact about the content and never a warning. A steady lock is the host's neutral
//     note, shown as written.
//   - Async sources are the one kind that can warn: rendered fell below what the
//     producer delivered. That warning always carries words, never colour alone.
//   - "unmeasurable" (showing, but nothing counts its frames) and "idle" (not
//     capturing right now) are different facts and read differently.

import type { CaptureRateRow } from "$lib/api/bridge";
import { fmtDuration } from "$lib/utils/format";
import { fmtNum } from "$lib/utils/statsMeter";

/** One decimal and a per-second unit, like the session log line ("30.0/s"). */
export function fmtRate(fps: number): string {
  return fps.toFixed(1) + "/s";
}

type Kind = CaptureRateRow["kind"];

export const CAPTURE_KIND_LABEL: Record<Kind, string> = {
  wgc: "WGC",
  dxgi: "DXGI",
  async: "Async",
  gameHook: "Game",
  none: "",
};

export const CAPTURE_KIND_TITLE: Record<Kind, string> = {
  wgc: "Windows Graphics Capture: counts a frame only when the screen changes",
  dxgi: "Desktop Duplication: counts a frame only when the screen changes",
  async: "Asynchronous video (camera, media): frames received and frames rendered",
  gameHook: "Game capture: its frames are not counted yet",
  none: "",
};

export type CaptureTone = "ok" | "warn" | "muted";

export interface CaptureView {
  /** Short method badge ("WGC"); "" when the method is unknown. */
  kind: string;
  kindTitle: string;
  /** The reading: a rate, "in … · out …", "Measuring…", "Unmeasurable" or "—". */
  value: string;
  /** Share of the canvas rate ("50% of canvas rate"), or null where it does not apply. */
  share: string | null;
  /** The live canvas rate the reading is judged against, or null off air. */
  ref: string | null;
  /** Neutral context: the host's lock note, or why a reading is missing. Never a warning. */
  note: string | null;
  /** The one warning a capture row can carry, in words. */
  warn: string | null;
  tone: CaptureTone;
  /** The whole row as one sentence for a screen reader. */
  label: string;
}

const MEASURING = "Measuring…";

/** The section-level states both surfaces show around the rows. */
export const CAPTURE_SECTION_TEXT = {
  unavailable: "Capture rates unavailable",
  starting: "Starting capture measurement…",
  emptyTitle: "No capture sources on screen",
  emptySub: "Display, window, game and video capture sources in an active scene show their frame rate here.",
} as const;

// Whole percent; a share too small to round to 1% still reads as present.
function pct(part: number, whole: number): string {
  const p = (part / whole) * 100;
  return p > 0 && p < 1 ? "<1%" : Math.round(p) + "%";
}

export function describeCapture(r: CaptureRateRow): CaptureView {
  const base = {
    kind: CAPTURE_KIND_LABEL[r.kind],
    kindTitle: CAPTURE_KIND_TITLE[r.kind],
    ref: r.refFps !== null ? fmtNum(r.refFps, 2) + " fps" : null,
    share: null,
    note: null,
    warn: null,
  };
  const method = base.kind ? `, ${base.kind}` : "";

  if (r.status === "idle") {
    return { ...base, value: "—", tone: "muted", label: `${r.name}${method}: not capturing` };
  }

  if (r.status === "ok" && (r.kind === "wgc" || r.kind === "dxgi")) {
    if (r.rate === null) {
      return { ...base, value: MEASURING, tone: "muted", label: `${r.name}${method}: measuring` };
    }
    const value = fmtRate(r.rate);
    const share = r.fraction !== null ? pct(r.fraction, 1) + " of canvas rate" : null;
    const parts = [value, share, r.note].filter((p): p is string => p !== null);
    return { ...base, value, share, note: r.note, tone: "ok", label: `${r.name}${method}: ${parts.join(", ")}` };
  }

  if (r.status === "ok" && r.kind === "async") {
    if (r.inputFps === null || r.renderedFps === null) {
      return { ...base, value: MEASURING, tone: "muted", label: `${r.name}${method}: measuring` };
    }
    const value = `in ${fmtRate(r.inputFps)} · out ${fmtRate(r.renderedFps)}`;
    const warn = r.below ? "Rendering fewer frames than it receives" : null;
    const spoken = `receiving ${fmtRate(r.inputFps)}, rendering ${fmtRate(r.renderedFps)}`;
    return {
      ...base,
      value,
      warn,
      tone: warn ? "warn" : "ok",
      label: `${r.name}${method}: ${spoken}` + (warn ? `. Warning: ${warn.toLowerCase()}` : ""),
    };
  }

  // Showing, with no counter behind it: BitBlt, a deinterlaced async source, a window
  // capture on "auto" without a live WGC session, a hooked game capture.
  const why =
    r.kind === "gameHook" ? "game capture frames are not counted yet" : "this capture method reports no frames";
  return {
    ...base,
    value: "Unmeasurable",
    note: why,
    tone: "muted",
    label: `${r.name}${method}: unmeasurable, ${why}`,
  };
}

/** The "since reset" window in words: how long it was measured, and the share of
 * that it spent locked (display capture) or below (async). "—" before any. */
export function fmtSinceReset(r: CaptureRateRow): string {
  const { liveSec, lockedSec, belowSec } = r.sinceReset;
  if (!(liveSec > 0)) {
    return "—";
  }
  let s = fmtDuration(liveSec * 1000);
  if (lockedSec > 0) {
    s += ` · locked ${pct(lockedSec, liveSec)}`;
  }
  if (belowSec > 0) {
    s += ` · below ${pct(belowSec, liveSec)}`;
  }
  return s;
}
