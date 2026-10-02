// The Counter widget's pure logic: what each source counts, the per-broadcast session tally
// that survives a source reload, the absent-is-not-zero totals, and the small decisions the
// template's renderer makes. Kept out of runtime.ts, which carries page side effects and
// cannot be imported outside an overlay document, so the tests run this exact code.

import type { EventType, NormalizedEvent } from "$lib/api/bridge";
import { fillTemplate } from "./fillTemplate";

/** What one event contributes to a session count: one per event, the units it names (the
 * subs a gift gave, or 1), or the amount it carries (bits cheered, Kicks sent). The host
 * keeps all three per (platform, type, kind), so a count picks one without a host change. */
type Measure = "events" | "units" | "amount";

/** How the host tells events of one type apart for counting: a YouTube membership is "new"
 * or a "milestone" (months held); every other event is "". */
export type TallyKind = "" | "new" | "milestone";

interface EventSource {
  types: readonly EventType[];
  measure: Measure;
  /** The kinds counted; every kind when absent. */
  kinds?: readonly TallyKind[];
}

/** The session sources, keyed by the Counter's `source` field value. One row per source, so
 * adding one is a row here and an option in fields.json. */
export const COUNTER_EVENT_SOURCES: Readonly<Record<string, EventSource>> = {
  follow: { types: ["follow"], measure: "events" },
  sub: { types: ["sub", "resub"], measure: "events" },
  subgift: { types: ["subgift"], measure: "units" },
  cheer: { types: ["cheer"], measure: "amount" },
  raid: { types: ["raid"], measure: "events" },
  superchat: { types: ["superchat"], measure: "events" },
  member: { types: ["member"], measure: "events", kinds: ["new"] },
  kicks: { types: ["kicks"], measure: "amount" },
};

/** `e`'s kind. Mirrors TallyKind in frontend/src/overlay/broadcast_tally.cpp, which files the
 * host's totals under the same rule. */
export function eventKind(e: Pick<NormalizedEvent, "type" | "months">): TallyKind {
  if (e.type === "member") {
    return isFiniteNumber(e.months) && e.months > 0 ? "milestone" : "new";
  }
  return "";
}

/** The fields of a live event a count reads, and all one is reduced to before it is held. */
export type TallyEvent = Pick<NormalizedEvent, "id" | "platform" | "type" | "ts" | "amount" | "count" | "months">;

/** One row of the host's totals: every event of `type` and `kind` from `platform` the
 * broadcast counted, how many units they named, and what they carried. */
export interface TallyTotal {
  platform: string;
  type: string;
  kind: string;
  events: number;
  units: number;
  amount: number;
}

/** The `tally` frame the overlay server sends a Counter (BroadcastTally::Snapshot in
 * overlay/broadcast_tally.cpp): on connect, and again whenever a broadcast starts or ends. The
 * totals of the current broadcast, or of the most recent one while nothing is live -- kept
 * across an app restart. `since` is null before any broadcast was recorded; `until` is null
 * while it is live. `recentIds` are the last events counted, so one that also reaches the page
 * live counts once. */
export interface TallyFrame {
  since: number | null;
  until: number | null;
  totals: TallyTotal[];
  recentIds: string[];
}

/** The span of event time a session count covers. A null bound is open: `since` null counts
 * every event seen since the page loaded, `until` null counts up to now. */
export interface CountWindow {
  since: number | null;
  until: number | null;
}

/** Why a session tally changed, which is what decides whether the number animates: only an
 * event arriving is a change worth showing as motion. "seed" is the page catching up to the
 * host's figure, "window" a broadcast starting or ending. */
export type TallyCause = "seed" | "event" | "window";

/** Live events held beyond this are evicted oldest-first. A tally frame drops only what earlier
 * connections delivered, so this bounds a page that stays connected for a long time -- across
 * several broadcasts, or with no broadcast ever recorded. */
const kMaxHeldEvents = 10000;

function isFiniteNumber(v: unknown): v is number {
  return typeof v === "number" && Number.isFinite(v);
}

function sourceSpec(source: string): EventSource | null {
  return Object.prototype.hasOwnProperty.call(COUNTER_EVENT_SOURCES, source) ? COUNTER_EVENT_SOURCES[source] : null;
}

function counts(spec: EventSource, platform: string, type: string, kind: string, platforms: ReadonlySet<string>) {
  return (
    (spec.types as readonly string[]).includes(type) &&
    platforms.has(platform) &&
    (!spec.kinds || (spec.kinds as readonly string[]).includes(kind))
  );
}

function contribution(e: TallyEvent, measure: Measure): number {
  if (measure === "amount") {
    // The host omits a zero amount from the wire.
    return isFiniteNumber(e.amount) ? e.amount : 0;
  }
  if (measure === "units") {
    // A gift event names how many subs it gave; one whose count the platform did not report
    // still gave at least one.
    return isFiniteNumber(e.count) && e.count > 0 ? e.count : 1;
  }
  return 1;
}

/** Whether `ts` falls inside `w`. */
export function inWindow(ts: number, w: CountWindow): boolean {
  return (w.since === null || ts >= w.since) && (w.until === null || ts <= w.until);
}

/** `source`'s count over live `events`: those inside `w`, of the source's types and kinds,
 * from a platform in `platforms`. 0 for a source this build does not know. */
export function countEvents(
  events: Iterable<TallyEvent>,
  source: string,
  platforms: ReadonlySet<string>,
  w: CountWindow,
): number {
  const spec = sourceSpec(source);
  if (!spec) {
    return 0;
  }
  let n = 0;
  for (const e of events) {
    if (counts(spec, e.platform, e.type, eventKind(e), platforms) && inWindow(e.ts, w)) {
      n += contribution(e, spec.measure);
    }
  }
  return n;
}

/** `source`'s count over the host's totals, from a platform in `platforms`. */
export function countTotals(totals: Iterable<TallyTotal>, source: string, platforms: ReadonlySet<string>): number {
  const spec = sourceSpec(source);
  if (!spec) {
    return 0;
  }
  let n = 0;
  for (const t of totals) {
    if (counts(spec, t.platform, t.type, t.kind, platforms)) {
      n += t[spec.measure];
    }
  }
  return n;
}

function isTotal(v: unknown): v is TallyTotal {
  const t = v as TallyTotal;
  return (
    !!t &&
    typeof t.platform === "string" &&
    typeof t.type === "string" &&
    typeof t.kind === "string" &&
    isFiniteNumber(t.events) &&
    isFiniteNumber(t.units) &&
    isFiniteNumber(t.amount)
  );
}

function slim(e: TallyEvent): TallyEvent {
  return {
    id: e.id,
    platform: e.platform,
    type: e.type,
    ts: e.ts,
    amount: e.amount,
    count: e.count,
    months: e.months,
  };
}

function isCountable(e: TallyEvent): boolean {
  return !!e && typeof e.id === "string" && e.id !== "" && isFiniteNumber(e.ts);
}

/** A broadcast's count as this page knows it: the host's totals for its window, plus the live
 * events that reached the page after them.
 *
 * The window moves only when the host says so -- a tally frame on connect, at a broadcast's
 * start and at its end -- so the page counts exactly what the host does. Live events are held
 * with the connection they arrived on. A frame with a window covers every event broadcast
 * before it, so it drops what earlier connections delivered; what this connection delivered
 * stays, and one the frame already counted is skipped by its id (`recentIds`). Nothing held is
 * dropped because the window moved: the count filters by the window instead. A closed window
 * is final, so it counts the host's totals alone. With no broadcast recorded there are no
 * totals and every held event counts, from page load.
 *
 * Test events are held apart and count on top of the rest until the next tally frame or real
 * stream frame. The runtime hands them over only in the editor's preview, so a source on
 * stream is never moved by one. */
export class SessionTally {
  private base: TallyTotal[] = [];
  private covered = new Set<string>();
  private win: CountWindow = { since: null, until: null };
  private seeded = false;
  private connection = 0;
  private readonly live = new Map<string, { e: TallyEvent; connection: number }>();
  private readonly tests = new Map<string, TallyEvent>();

  /** Whether the host's tally frame has arrived. Until it has, a count would be the
   * since-load figure standing in for a broadcast's real one -- typically a 0 flashed on a
   * reload mid-broadcast. */
  get ready(): boolean {
    return this.seeded;
  }

  get window(): CountWindow {
    return { ...this.win };
  }

  count(source: string, platforms: ReadonlySet<string>): number {
    let n = countTotals(this.base, source, platforms);
    if (this.win.until === null) {
      const fresh: TallyEvent[] = [];
      for (const { e } of this.live.values()) {
        if (!this.covered.has(e.id)) {
          fresh.push(e);
        }
      }
      n += countEvents(fresh, source, platforms, this.win);
    }
    return n + countEvents(this.tests.values(), source, platforms, { since: null, until: null });
  }

  /** The page's event stream (re)connected. Everything held from before is covered by the
   * tally frame this connection opens with. */
  connected(): void {
    this.connection += 1;
  }

  /** Adopt a tally frame. Returns "window" when it moved the window, else "seed". */
  seed(frame: TallyFrame): TallyCause {
    const w: CountWindow = {
      since: isFiniteNumber(frame.since) ? frame.since : null,
      until: isFiniteNumber(frame.until) ? frame.until : null,
    };
    const moved = this.seeded && (w.since !== this.win.since || w.until !== this.win.until);
    if (w.since !== null) {
      for (const [id, held] of this.live) {
        if (held.connection < this.connection) {
          this.live.delete(id);
        }
      }
    }
    this.base = Array.isArray(frame.totals) ? frame.totals.filter(isTotal) : [];
    this.covered = new Set(
      Array.isArray(frame.recentIds) ? frame.recentIds.filter((id): id is string => typeof id === "string") : [],
    );
    this.win = w;
    this.tests.clear();
    this.seeded = true;
    return moved ? "window" : "seed";
  }

  /** Hold a live event. False when it was already held (or cannot be counted), so the caller
   * knows nothing changed. */
  add(e: TallyEvent): boolean {
    if (!isCountable(e) || this.live.has(e.id)) {
      return false;
    }
    this.live.set(e.id, { e: slim(e), connection: this.connection });
    // Oldest-first; a Map iterates in insertion order and deleting the visited key is defined.
    for (const id of this.live.keys()) {
      if (this.live.size <= kMaxHeldEvents) {
        break;
      }
      this.live.delete(id);
    }
    return true;
  }

  /** Hold a test event on top of the count. False when nothing changed. */
  addTest(e: TallyEvent): boolean {
    if (!isCountable(e) || this.tests.has(e.id)) {
      return false;
    }
    this.tests.set(e.id, slim(e));
    return true;
  }

  /** Drop every test event. True when there were any. */
  clearTests(): boolean {
    const had = this.tests.size > 0;
    this.tests.clear();
    return had;
  }
}

/** Sum the figures of the selected platforms that reported one, or null when none did. A
 * platform that said nothing -- disconnected, not live, errored, withholding its figure -- is
 * not a zero, so it is left out of the sum, and a sum of nothing is no figure at all. */
function sumReported(values: Iterable<number | null | undefined>): number | null {
  let sum: number | null = null;
  for (const v of values) {
    if (isFiniteNumber(v)) {
      sum = (sum ?? 0) + v;
    }
  }
  return sum;
}

function* selected<T>(per: Record<string, T> | undefined, platforms: ReadonlySet<string>): Generator<T> {
  for (const id of platforms) {
    if (per && Object.prototype.hasOwnProperty.call(per, id)) {
      yield per[id];
    }
  }
}

/** Current viewers over the selected platforms, from a viewer snapshot's `perPlatform` (a
 * platform that did not report is absent from it). Null when none of them reported. */
export function viewerTotal(perPlatform: Record<string, number> | undefined, platforms: ReadonlySet<string>): number | null {
  return sumReported(selected(perPlatform, platforms));
}

/** Followers/subscribers over the selected platforms, from a channel-stats snapshot's
 * `perPlatform` groups, whose `count` is null when no account of the platform gave one. Null
 * when none of them reported. The runtime never sums across platforms on its own, since a
 * follower and a subscriber are different things; this does only because the user ticked
 * those platforms and their format names what the sum is. */
export function audienceTotal(
  perPlatform: Record<string, { count: number | null }> | undefined,
  platforms: ReadonlySet<string>,
): number | null {
  return sumReported(Array.from(selected(perPlatform, platforms), (g) => (g ? g.count : null)));
}

/** The figure shown: `n` plus the user's start offset, or null when there is no figure. An
 * offset that is not a finite number adds nothing. The Counter applies it to session counts
 * only: a live total is already the whole figure. */
export function withOffset(n: number | null, offset: unknown): number | null {
  if (n === null) {
    return null;
  }
  const o = typeof offset === "string" && offset.trim() !== "" ? Number(offset) : offset;
  return n + (isFiniteNumber(o) ? Math.trunc(o) : 0);
}

export type CounterAnimation = "none" | "countup" | "odometer" | "pop";
const kAnimations: readonly CounterAnimation[] = ["none", "countup", "odometer", "pop"];

/** The animation a change actually gets. Reduced motion makes every change instant, and an
 * unknown choice (a hand-edited or newer document) falls back to instant rather than guess. */
export function effectiveAnimation(choice: unknown, reducedMotion: boolean): CounterAnimation {
  if (reducedMotion) {
    return "none";
  }
  return kAnimations.find((a) => a === choice) ?? "none";
}

/** The count-up curve: ease-out cubic, so the figure moves fast and settles. `t` is clamped
 * to 0..1. */
export function easeOutCubic(t: number): number {
  const c = Math.min(1, Math.max(0, t));
  return 1 - Math.pow(1 - c, 3);
}

/** The whole number a count-up shows at progress `t` from `from` to `to`. */
export function tweenValue(from: number, to: number, t: number): number {
  return Math.round(from + (to - from) * easeOutCubic(t));
}

// A private-use character, so a user's own text cannot be mistaken for the number's place.
const kNumberSlot = "\uE000";

/** The user's format split around its `{n}` tokens, filled by the shared template filler
 * first so it reads exactly as every other widget's format does. The number goes between
 * consecutive parts. A format with no `{n}` -- a blank one included -- shows the number alone,
 * since a counter that never shows its number is not a counter. */
export function templateParts(format: string): string[] {
  const parts = fillTemplate(format, { n: kNumberSlot }).split(kNumberSlot);
  return parts.length > 1 ? parts : ["", ""];
}
