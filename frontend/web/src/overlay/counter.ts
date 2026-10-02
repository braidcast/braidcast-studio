// The Counter widget's pure logic: what each source counts, the per-broadcast session tally
// that survives a source reload, the absent-is-not-zero totals, and the small decisions the
// template's renderer makes. Kept out of runtime.ts, which carries page side effects and
// cannot be imported outside an overlay document, so the tests run this exact code.

import type { EventType, NormalizedEvent, StreamState } from "$lib/api/bridge";
import { fillTemplate } from "./fillTemplate";

/** What one event contributes to a session count. */
type Measure = "events" | "amount" | "gifts";

interface EventSource {
  types: readonly EventType[];
  measure: Measure;
}

/** The session sources, keyed by the Counter's `source` field value. One row per source, so
 * adding one is a row here and an option in fields.json. `amount` sums what the host carries
 * there (bits cheered, Kicks sent); `gifts` counts the subs a gift event gave rather than the
 * gift events themselves. */
export const COUNTER_EVENT_SOURCES: Readonly<Record<string, EventSource>> = {
  follow: { types: ["follow"], measure: "events" },
  sub: { types: ["sub", "resub"], measure: "events" },
  subgift: { types: ["subgift"], measure: "gifts" },
  cheer: { types: ["cheer"], measure: "amount" },
  raid: { types: ["raid"], measure: "events" },
  superchat: { types: ["superchat"], measure: "events" },
  member: { types: ["member"], measure: "events" },
  kicks: { types: ["kicks"], measure: "amount" },
};

/** The fields of an event a count reads: what the host's `tally` frame carries per event,
 * and all a live event is reduced to before it is held. */
export type TallyEvent = Pick<NormalizedEvent, "id" | "platform" | "type" | "ts" | "amount" | "count">;

/** The `tally` frame the overlay server sends a Counter on connect (RunSse in
 * overlay_server.cpp): the events of the current broadcast, or of the most recent one while
 * nothing is live, from the app's event store. `since` is null when no broadcast has started
 * this run, which leaves nothing to rebuild from. */
export interface TallyFrame {
  since: number | null;
  until: number | null;
  live: boolean;
  events: TallyEvent[];
}

/** The span of event time a session count covers. A null bound is open: `since` null counts
 * every event seen since the page loaded, `until` null counts up to now. */
export interface CountWindow {
  since: number | null;
  until: number | null;
}

/** Why a session tally changed, which is what decides whether the number animates: only a
 * real event arriving is a change worth showing as motion. A seed is the page catching up
 * to a count it already had, and a window change is a new broadcast starting over. */
export type TallyCause = "seed" | "event" | "window";

/** Held events beyond this are evicted oldest-first. The store a reload rebuilds from keeps
 * 500, so this only bounds a page left open across an extraordinarily busy broadcast. */
const kMaxHeldEvents = 10000;

function isFiniteNumber(v: unknown): v is number {
  return typeof v === "number" && Number.isFinite(v);
}

function slim(e: TallyEvent): TallyEvent {
  return { id: e.id, platform: e.platform, type: e.type, ts: e.ts, amount: e.amount, count: e.count };
}

function contribution(e: TallyEvent, measure: Measure): number {
  if (measure === "amount") {
    // The host omits a zero amount from the wire.
    return isFiniteNumber(e.amount) ? e.amount : 0;
  }
  if (measure === "gifts") {
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

/** `source`'s count over `events`: those inside `w`, of the source's types, from a platform
 * in `platforms`. 0 for a source this build does not know. */
export function countEvents(
  events: Iterable<TallyEvent>,
  source: string,
  platforms: ReadonlySet<string>,
  w: CountWindow,
): number {
  const spec = Object.prototype.hasOwnProperty.call(COUNTER_EVENT_SOURCES, source)
    ? COUNTER_EVENT_SOURCES[source]
    : null;
  if (!spec) {
    return 0;
  }
  let n = 0;
  for (const e of events) {
    if (spec.types.includes(e.type) && platforms.has(e.platform) && inWindow(e.ts, w)) {
      n += contribution(e, spec.measure);
    }
  }
  return n;
}

/** Every event of the current broadcast this page knows of, keyed by id. Both halves of the
 * picture land here -- the host's tally on connect and each live event after -- so an event
 * delivered by both (one stored just as the socket registered) counts once, and a window
 * change is a refold over what is held rather than a counter that has to be reset by hand.
 *
 * The window follows the broadcast: a start opens a new one, an end closes the current one
 * at that moment so the count holds the finished broadcast's figure instead of resetting or
 * counting on, and the host's tally frame, being the server's own record, overrides both. */
export class SessionTally {
  private readonly events = new Map<string, TallyEvent>();
  private win: CountWindow = { since: null, until: null };
  private seeded = false;

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
    return countEvents(this.events.values(), source, platforms, this.win);
  }

  /** Adopt the host's tally. Repeats on every reconnect, which only re-adds what is held. */
  seed(frame: TallyFrame): void {
    this.setWindow({
      since: isFiniteNumber(frame.since) ? frame.since : null,
      until: isFiniteNumber(frame.until) ? frame.until : null,
    });
    for (const e of Array.isArray(frame.events) ? frame.events : []) {
      this.hold(e);
    }
    this.seeded = true;
  }

  /** Hold a live event. False when it was already held (or carries no id), so the caller
   * knows nothing changed. */
  add(e: TallyEvent): boolean {
    if (!e || typeof e.id !== "string" || e.id === "" || this.events.has(e.id)) {
      return false;
    }
    this.hold(e);
    return true;
  }

  /** Follow a broadcast-state frame. True when the window moved. `nowMs` closes an ended
   * broadcast's window. */
  onStream(state: Pick<StreamState, "active" | "startedAt">, nowMs: number): boolean {
    if (state.active === true) {
      // A live frame with no start yet is a broadcast still connecting; the window opens
      // when an output reports the start, not before.
      if (!isFiniteNumber(state.startedAt) || state.startedAt <= 0) {
        return false;
      }
      if (this.win.since === state.startedAt && this.win.until === null) {
        return false;
      }
      this.setWindow({ since: state.startedAt, until: null });
      return true;
    }
    // Nothing has started this run: keep counting since load rather than closing a window
    // that was never a broadcast's.
    if (this.win.since === null || this.win.until !== null) {
      return false;
    }
    this.win = { since: this.win.since, until: nowMs };
    return true;
  }

  private setWindow(w: CountWindow): void {
    this.win = w;
    // Windows only move forward, so an event before the start can never count again.
    if (w.since !== null) {
      for (const [id, e] of this.events) {
        if (e.ts < w.since) {
          this.events.delete(id);
        }
      }
    }
  }

  private hold(e: TallyEvent): void {
    if (!e || typeof e.id !== "string" || e.id === "" || !isFiniteNumber(e.ts)) {
      return;
    }
    this.events.set(e.id, slim(e));
    // Oldest-first; a Map iterates in insertion order and deleting the visited key is defined.
    for (const id of this.events.keys()) {
      if (this.events.size <= kMaxHeldEvents) {
        break;
      }
      this.events.delete(id);
    }
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
 * offset that is not a finite number adds nothing. */
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
const kNumberSlot = "";

/** The user's format split around its `{n}` tokens, filled by the shared template filler
 * first so it reads exactly as every other widget's format does. The number goes between
 * consecutive parts, so a format without `{n}` is one part and shows no number. */
export function templateParts(format: string): string[] {
  return fillTemplate(format, { n: kNumberSlot }).split(kNumberSlot);
}
