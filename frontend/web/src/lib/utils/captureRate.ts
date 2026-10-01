// How a capture-rate row reads, shared by the Stats dock and the Monitor page so the
// two cannot word one reading differently. The rules are the host's (see
// frontend/src/diag/capture_rate.hpp); this only chooses words for them:
//   - WGC and DXGI count a frame only when the screen changes, so their rate is a
//     fact about the content and never a warning. A steady lock is the host's neutral
//     note, shown as written.
//   - Async sources and game capture are the kinds that can warn: the ticks that
//     brought a new frame fell below what the producer offered (frames received,
//     game presents) and the canvas could show. Game capture also shows the hook's
//     copies, which can run ahead of the new frames, but the warning is never about
//     them. That warning always carries words, never colour alone.
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
  gameHook: "Game capture: frames the game presented, frames copied for capture, and new frames the canvas could show",
  none: "",
};

export type CaptureTone = "ok" | "warn" | "muted";

export interface CaptureView {
  /** Short method badge ("WGC"); "" when the method is unknown. */
  kind: string;
  kindTitle: string;
  /** The reading: a rate, "in … · out …", "presents … · copies … · new …",
   * "Measuring…", "Unmeasurable" or "—". */
  value: string;
  /** The value's "label rate" pieces when it has several, so a view can wrap between
   * them without splitting a label from its number; absent for a single reading. */
  segments?: string[];
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

interface ProducerWords {
  input: string;
  output: string;
  spokenInput: string;
  spokenOutput: string;
  /** Between input and output, where the producer path has a stage of its own. */
  copies?: { word: string; spoken: string };
  warn: string;
}

/** The kinds whose producer rate is known, each read in its own words: the row shows
 * what the producer offered, then the new frames that reached capture, the number
 * "below" judges. */
const PRODUCER_WORDS = {
  async: {
    input: "in",
    output: "out",
    spokenInput: "receiving",
    spokenOutput: "rendering",
    warn: "Rendering fewer frames than it receives",
  },
  gameHook: {
    input: "presents",
    output: "new",
    spokenInput: "game presenting",
    spokenOutput: "new to the canvas",
    copies: { word: "copies", spoken: "copying" },
    warn: "Fewer new game frames than the canvas could show",
  },
} satisfies Partial<Record<Kind, ProducerWords>>;

type ProducerKind = keyof typeof PRODUCER_WORDS;

function isProducerKind(kind: Kind): kind is ProducerKind {
  return Object.hasOwn(PRODUCER_WORDS, kind);
}

/** Why a showing source reads unmeasurable, when the host gives no reason. */
const NO_COUNTER = "this capture method reports no frames";

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

  if (r.status === "ok" && isProducerKind(r.kind)) {
    if (r.inputFps === null || r.renderedFps === null) {
      return { ...base, value: MEASURING, tone: "muted", label: `${r.name}${method}: measuring` };
    }
    const words: ProducerWords = PRODUCER_WORDS[r.kind];
    const shown = [`${words.input} ${fmtRate(r.inputFps)}`];
    const said = [`${words.spokenInput} ${fmtRate(r.inputFps)}`];
    if (words.copies && r.copiesFps !== null) {
      shown.push(`${words.copies.word} ${fmtRate(r.copiesFps)}`);
      said.push(`${words.copies.spoken} ${fmtRate(r.copiesFps)}`);
    }
    shown.push(`${words.output} ${fmtRate(r.renderedFps)}`);
    said.push(`${words.spokenOutput} ${fmtRate(r.renderedFps)}`);
    const value = shown.join(" · ");
    // The host decides "below"; this never re-derives it from the rates.
    const warn = r.below ? words.warn : null;
    const spoken = said.join(", ");
    return {
      ...base,
      value,
      segments: shown,
      warn,
      tone: warn ? "warn" : "ok",
      label: `${r.name}${method}: ${spoken}` + (warn ? `. Warning: ${warn.toLowerCase()}` : ""),
    };
  }

  // Showing, with no counter behind it: BitBlt, a deinterlaced async source, a window
  // capture on "auto" without a live WGC session, a game capture whose hook predates
  // frame counting (the host says so in its note).
  const why = r.note ?? NO_COUNTER;
  return {
    ...base,
    value: "Unmeasurable",
    note: why,
    tone: "muted",
    label: `${r.name}${method}: unmeasurable, ${why}`,
  };
}

/** The "since reset" window in words: how long it was measured, and the share of
 * that it spent locked (display capture) or below (async, game). "—" before any. */
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
