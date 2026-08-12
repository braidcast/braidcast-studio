// Calendar geometry and conflict math. Pure functions over epoch milliseconds and
// plain records: nothing here touches the DOM, reads a store, or calls the bridge,
// so the packing and the destination-conflict rule can be exercised on their own.
//
// Day boundaries and clock positions are computed through the local Date
// constructor, never by adding 86_400_000: a calendar shows the user's own days,
// and a DST boundary makes a day 23 or 25 hours long, so epoch arithmetic puts an
// afternoon entry an hour off its own row twice a year. Durations stay epoch
// arithmetic -- four hours is four hours whatever the clock did meanwhile.

import type { ScheduleEntryInfo, ScheduleState, SessionInfo } from "$lib/api/bridge";

export const MS_MIN = 60_000;
export const MINUTES_PER_DAY = 1440;

/** Drag granularity. Fifteen minutes is what a stream is planned to; a finer snap
 * makes a two-pixel pointer jitter change the plan. */
export const SNAP_MIN = 15;

/** The runner arms an entry at T-5 and starts the cancellable countdown at T-60s
 * (design §5). Mirrored here because the chip has to render both phases, and the
 * server sends no per-second event -- the countdown is computed client-side from
 * `startsAt` while the entry is armed. */
export const ARM_LEAD_MS = 5 * 60_000;
export const COUNTDOWN_LEAD_MS = 60_000;

/** Height of one hour row in the Week/Day grid, and the floor a block is drawn at
 * however short it is. Both live here rather than in the view because the packer
 * has to agree with them: MIN_LAYOUT_MIN below is what they come to in minutes. */
export const HOUR_PX = 44;
export const MIN_BLOCK_PX = 20;

/** Two blocks closer together than this are packed side by side even though their
 * times do not strictly overlap: below it the taller one's drawn minimum would
 * cover the shorter one and the grid would look like it lost an entry. Derived
 * from the pixel floor rather than guessed alongside it -- a hand-picked 20 next
 * to a 20px floor left five pixels of overlap the packer believed was clear. */
const MIN_LAYOUT_MIN = (MIN_BLOCK_PX / HOUR_PX) * 60;

/** States the runner has not settled. Only these can still be edited, and only
 * these can conflict -- a missed or canceled entry will never occupy a profile. */
export const LIVE_STATES: readonly ScheduleState[] = ["planned", "armed", "live"];

/** States schedule.startNow will accept. Live, done and canceled are refused by
 * the runner regardless of the click, so offering the action there would be a
 * button that always fails. */
export const GO_LIVE_STATES: readonly ScheduleState[] = ["planned", "armed", "missed"];

export type CalendarItemKind = "planned" | "session";

/** One block on the timeline, whether it is something planned or something that
 * actually ran. One type for both is the point of Task 9.1: the grid packs, sorts
 * and renders past and future through the same path instead of two parallel ones. */
export interface CalendarItem {
  /** Unique across kinds; a session and the entry it ran for are two rows. */
  key: string;
  kind: CalendarItemKind;
  /** The schedule entry: its own id for a planned item, the entry a session ran
   * for, or null for an ad-hoc broadcast nobody scheduled. */
  entryId: string | null;
  sessionId: string | null;
  title: string;
  start: number;
  end: number;
  /** What was planned, on a session that ran against an entry. Null everywhere
   * else. The pair (plannedEnd, end) is the overrun. */
  plannedStart: number | null;
  plannedEnd: number | null;
  state: ScheduleState | null;
  /** sessions.end_reason; null on a planned item. */
  endReason: string | null;
  running: boolean;
  autoStart: boolean;
  /** This occurrence was disarmed by hand. The entry is back to `planned`, so
   * nothing else in the payload tells it apart from one that never armed. */
  countdownCanceled: boolean;
  /** Why this occurrence cannot go live, or "". Broadcasting to nowhere is worse
   * than not broadcasting, so the refusal is shown rather than merely obeyed. */
  blockReason: string;
  /** The destinations this item goes out to, each with its own refusal. Carried as
   * pairs rather than as ids beside a parallel array of reasons: the two would be
   * one reorder away from naming the wrong destination. */
  destinations: { profileId: string; blockReason: string }[];
  /** Absolute thumbnail path for file.readDataUri; empty when none was chosen. */
  thumbFile: string;
  /** Movable and resizable. Nothing in the past ever is -- you cannot reschedule
   * yesterday, and a grid that lets you try is lying. */
  editable: boolean;
}

export function entryEnd(entry: ScheduleEntryInfo): number {
  return entry.startsAt + Math.max(1, entry.durationMin) * MS_MIN;
}

/**
 * Planned entries as timeline blocks. `linkedEntryIds` are entries a recorded
 * session already covers: those are dropped here and carried by the session
 * instead, so the plan and its outcome are one block with a ghost rather than two
 * blocks claiming the same slot.
 */
export function plannedItems(
  entries: readonly ScheduleEntryInfo[],
  nowMs: number,
  linkedEntryIds: ReadonlySet<string>,
): CalendarItem[] {
  const items: CalendarItem[] = [];
  for (const e of entries) {
    if (linkedEntryIds.has(e.id)) {
      continue;
    }
    items.push({
      key: "e:" + e.id,
      kind: "planned",
      entryId: e.id,
      sessionId: null,
      title: e.title || "Untitled stream",
      start: e.startsAt,
      end: entryEnd(e),
      plannedStart: null,
      plannedEnd: null,
      state: e.state,
      endReason: null,
      running: e.state === "live",
      autoStart: e.autoStart,
      countdownCanceled: e.countdownCanceled,
      blockReason: e.blockReason,
      destinations: e.destinations.map((d) => ({
        profileId: d.profileId,
        blockReason: d.blockReason,
      })),
      thumbFile: "",
      editable: LIVE_STATES.includes(e.state) && e.startsAt > nowMs,
    });
  }
  return items;
}

/** Recorded sessions as timeline blocks, at their ACTUAL extent. A session still
 * running is drawn up to `nowMs` so it grows against the clock. */
export function sessionItems(
  sessions: readonly SessionInfo[],
  nowMs: number,
  entryById: ReadonlyMap<string, ScheduleEntryInfo>,
): CalendarItem[] {
  return sessions.map((s) => {
    const entry = s.scheduleId ? entryById.get(s.scheduleId) : undefined;
    return {
      key: "s:" + s.id,
      kind: "session" as const,
      entryId: entry?.id ?? null,
      sessionId: s.id,
      title: s.title || entry?.title || "Untitled stream",
      start: s.startedAt,
      end: s.endedAt ?? Math.max(nowMs, s.startedAt + MS_MIN),
      plannedStart: entry ? entry.startsAt : null,
      plannedEnd: entry ? entryEnd(entry) : null,
      state: null,
      endReason: s.endedAt === null ? null : s.endReason,
      running: s.endedAt === null,
      autoStart: false,
      countdownCanceled: false,
      blockReason: "",
      // A session is a record of what already went out, so nothing about it can be
      // refused -- the empty reason is the whole truth here, not a placeholder.
      destinations: s.destinations.map((d) => ({ profileId: d.profileId, blockReason: "" })),
      thumbFile: s.thumbFile,
      editable: false,
    };
  });
}

/** Entry ids a recorded session already accounts for. */
export function linkedEntryIds(sessions: readonly SessionInfo[]): Set<string> {
  const ids = new Set<string>();
  for (const s of sessions) {
    if (s.scheduleId) {
      ids.add(s.scheduleId);
    }
  }
  return ids;
}

/** Overrun in whole minutes, or 0 when the session did not run past its plan. */
export function overrunMin(item: CalendarItem): number {
  if (item.plannedEnd === null || item.end <= item.plannedEnd) {
    return 0;
  }
  return Math.round((item.end - item.plannedEnd) / MS_MIN);
}

// --- destination conflicts ----------------------------------------------------

/** Two entries overlapping in time while sharing a stream profile is not an
 * advisory clash: one profile is one RTMP key, so the second output cannot exist.
 * Surfaced while the plan is being edited rather than at go-live, which is the
 * only point at which it is still cheap to fix. */
export interface Conflict {
  /** Titles of the entries this one collides with, for the written reason. */
  others: string[];
  /** The profile ids actually shared -- the reason, not just the fact. */
  profileIds: string[];
}

export function destinationConflicts(
  entries: readonly ScheduleEntryInfo[],
  nowMs: number,
): Map<string, Conflict> {
  const live = entries.filter((e) => LIVE_STATES.includes(e.state) && entryEnd(e) > nowMs);
  const sorted = [...live].sort((a, b) => a.startsAt - b.startsAt);
  const out = new Map<string, Conflict>();

  const record = (a: ScheduleEntryInfo, b: ScheduleEntryInfo, shared: string[]): void => {
    const found = out.get(a.id);
    if (found) {
      found.others.push(b.title || "Untitled stream");
      for (const p of shared) {
        if (!found.profileIds.includes(p)) {
          found.profileIds.push(p);
        }
      }
    } else {
      out.set(a.id, { others: [b.title || "Untitled stream"], profileIds: [...shared] });
    }
  };

  for (let i = 0; i < sorted.length; i++) {
    const a = sorted[i];
    const aEnd = entryEnd(a);
    for (let j = i + 1; j < sorted.length; j++) {
      const b = sorted[j];
      // Sorted by start, so once one entry begins after `a` ends nothing later can
      // overlap it either.
      if (b.startsAt >= aEnd) {
        break;
      }
      const aProfiles = new Set(a.destinations.map((d) => d.profileId));
      const shared = b.destinations.map((d) => d.profileId).filter((p) => aProfiles.has(p));
      if (shared.length > 0) {
        record(a, b, shared);
        record(b, a, shared);
      }
    }
  }
  return out;
}

// --- arming and countdown -----------------------------------------------------

/** What an armed entry is doing right now, as far as the chip is concerned. */
export type ArmPhase = "none" | "armed" | "countdown" | "canceled";

/**
 * Cancelling puts the entry back to `planned`, so the state alone cannot tell a
 * cancelled occurrence from one that was never armed -- `countdownCanceled` is
 * checked first for exactly that reason, and it deliberately outlives the entry
 * settling as missed so the chip can still say the start was cancelled.
 */
export function armPhase(item: CalendarItem, nowMs: number): ArmPhase {
  if (item.state === "armed") {
    return item.autoStart && nowMs >= item.start - COUNTDOWN_LEAD_MS && nowMs < item.start
      ? "countdown"
      : "armed";
  }
  // The flag outlives the cancel, so it is only the operative fact while the entry
  // is still waiting or never ran. Once it went live -- by hand, which cancelling
  // the auto-start does not prevent -- or settled, what happened outranks what was
  // called off, and a `live` entry badged "Auto-start canceled" is simply wrong.
  return item.countdownCanceled && (item.state === "planned" || item.state === "missed")
    ? "canceled"
    : "none";
}

/** "T-04:32" / "T-00:47", counting down to `target`. Clamped at zero rather than
 * flipping sign: past the instant the runner owns what happens, not the label. */
export function countdownLabel(target: number, nowMs: number): string {
  const total = Math.max(0, Math.ceil((target - nowMs) / 1000));
  const m = Math.floor(total / 60);
  const s = total % 60;
  return `T-${String(m).padStart(2, "0")}:${String(s).padStart(2, "0")}`;
}

// --- local-time day helpers ---------------------------------------------------

export function startOfDay(ms: number): number {
  const d = new Date(ms);
  return new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime();
}

export function addDays(ms: number, n: number): number {
  const d = new Date(ms);
  return new Date(d.getFullYear(), d.getMonth(), d.getDate() + n).getTime();
}

/** End of the local day containing `ms`, i.e. the start of the next one. */
export function endOfDay(ms: number): number {
  return addDays(startOfDay(ms), 1);
}

export function sameDay(a: number, b: number): boolean {
  return startOfDay(a) === startOfDay(b);
}

/** Monday-first week containing `ms`. */
export function startOfWeek(ms: number): number {
  const d = new Date(startOfDay(ms));
  return addDays(d.getTime(), -((d.getDay() + 6) % 7));
}

/** Calendar day as a whole number, from the LOCAL date parts through Date.UTC —
 * which has no DST — so two dates differ by exactly the number of days between
 * them however long each of those days actually was. */
function calendarDay(ms: number): number {
  const d = new Date(ms);
  return Date.UTC(d.getFullYear(), d.getMonth(), d.getDate()) / 86_400_000;
}

/**
 * Wall-clock minutes from midnight of `dayStart`'s day to `ms`, i.e. what row of
 * that day's grid `ms` sits on. Derived from the local clock reading plus 1440 per
 * whole calendar day between the two, NOT from the elapsed epoch difference: a
 * spring-forward day is 23 hours long, so elapsed time would put a 15:00 entry on
 * the 14:00 row, and a fall-back day would push 23:30 past the bottom of the grid.
 *
 * Signed and unbounded on purpose. An instant before `dayStart` returns a negative
 * minute and one after it returns more than 1440, which is what lets a block that
 * crosses midnight keep the point it was grabbed by.
 */
export function minutesInto(dayStart: number, ms: number): number {
  const d = new Date(ms);
  const clock = d.getHours() * 60 + d.getMinutes() + d.getSeconds() / 60;
  return Math.round((calendarDay(ms) - calendarDay(dayStart)) * MINUTES_PER_DAY + clock);
}

/**
 * The inverse: the instant at `minutes` wall-clock minutes into `dayStart`'s day,
 * snapped to SNAP_MIN. Built through the local Date constructor, which normalizes
 * out-of-range minutes into the neighbouring day and applies that day's real
 * offset, so this survives both DST and a value outside [0, 1440].
 *
 * Deliberately NOT clamped to the day. Clamping here is what turned a one-pixel
 * jiggle on a midnight-crossing block into a reschedule to 00:00. A caller that
 * needs containment bounds its own input -- CalendarTimeGrid's hit test does,
 * because a create gesture genuinely belongs to the column it started in.
 */
export function atMinute(dayStart: number, minutes: number): number {
  return atExactMinute(dayStart, Math.round(minutes / SNAP_MIN) * SNAP_MIN);
}

/** `atMinute` without the snap. Minutes are counted onto the local calendar day
 * rather than added to the epoch, which is what keeps a 23- or 25-hour day from
 * shifting everything on it by an hour. */
export function atExactMinute(dayStart: number, minutes: number): number {
  const d = new Date(dayStart);
  return new Date(d.getFullYear(), d.getMonth(), d.getDate(), 0, minutes, 0, 0).getTime();
}

/** Wall-clock minutes from midnight of the day `ms` itself falls in. */
export function minuteOfDay(ms: number): number {
  return minutesInto(startOfDay(ms), ms);
}

/** `ms` moved to `dayStart`'s day, keeping its clock reading. The one way a day
 * change is expressed, so no caller re-derives it as an epoch offset and lands an
 * hour out across a DST boundary.
 *
 * Unsnapped: moving an entry to another day is not an edit of the time it starts
 * at, and rounding here would quietly rewrite a 14:07 stream to 14:00 on its way
 * across the month grid. */
export function sameTimeOnDay(dayStart: number, ms: number): number {
  return atExactMinute(dayStart, minuteOfDay(ms));
}

// --- month grid ---------------------------------------------------------------

export interface MonthCell {
  key: string;
  /** Local midnight of the day this cell stands for. */
  dayStart: number;
  dayNum: number;
  inMonth: boolean;
  isToday: boolean;
  /** The whole day is behind us, so nothing can be planned into it. */
  isPast: boolean;
}

/** Monday-first month matrix, padded to whole weeks. */
export function monthCells(year: number, month: number, nowMs: number): MonthCell[] {
  const first = new Date(year, month, 1).getTime();
  const lead = (new Date(first).getDay() + 6) % 7;
  const daysInMonth = new Date(year, month + 1, 0).getDate();
  const total = Math.ceil((lead + daysInMonth) / 7) * 7;
  const today = startOfDay(nowMs);
  const gridStart = addDays(first, -lead);

  const cells: MonthCell[] = [];
  for (let i = 0; i < total; i++) {
    const dayStart = addDays(gridStart, i);
    const d = new Date(dayStart);
    cells.push({
      key: "d" + dayStart,
      dayStart,
      dayNum: d.getDate(),
      inMonth: d.getMonth() === month,
      isToday: dayStart === today,
      isPast: dayStart < today,
    });
  }
  return cells;
}

// --- interval packing ---------------------------------------------------------

/** One item clipped to one day column, with the column slot the packer gave it. */
export interface DaySegment {
  key: string;
  item: CalendarItem;
  /** Minutes from the day's start; always within [0, MINUTES_PER_DAY]. */
  topMin: number;
  endMin: number;
  /** Zero-based column and the width divisor for its cluster. */
  col: number;
  cols: number;
  /** The item began before this day / runs past it, so the block is cut. */
  continuesBefore: boolean;
  continuesAfter: boolean;
}

/**
 * Standard interval packing: items are grouped into clusters of transitively
 * overlapping blocks, and within a cluster each takes the first column free at
 * its start. Every member of a cluster reports the same `cols`, so a row of
 * side-by-side blocks lines up instead of each choosing its own width.
 */
const clampToDay = (minutes: number): number =>
  Math.min(Math.max(minutes, 0), MINUTES_PER_DAY);

export function daySegments(items: readonly CalendarItem[], dayStart: number): DaySegment[] {
  const dayEnd = addDays(dayStart, 1);
  const clipped = items
    .filter((i) => i.start < dayEnd && i.end > dayStart)
    .map((i) => ({
      key: i.key,
      item: i,
      // Clamped at BOTH ends, so the documented [0, MINUTES_PER_DAY] range holds
      // by construction rather than by trusting the filter above it.
      topMin: clampToDay(minutesInto(dayStart, i.start)),
      endMin: clampToDay(minutesInto(dayStart, i.end)),
      col: 0,
      cols: 1,
      continuesBefore: i.start < dayStart,
      continuesAfter: i.end > dayEnd,
    }))
    .sort((a, b) => a.topMin - b.topMin || b.endMin - a.endMin);

  // Column end times for the cluster being built, and the members to stamp once
  // the cluster closes.
  let colEnds: number[] = [];
  let cluster: DaySegment[] = [];
  const closeCluster = (): void => {
    for (const seg of cluster) {
      seg.cols = colEnds.length;
    }
    colEnds = [];
    cluster = [];
  };

  for (const seg of clipped) {
    const layoutEnd = Math.max(seg.endMin, seg.topMin + MIN_LAYOUT_MIN);
    if (cluster.length > 0 && colEnds.every((end) => end <= seg.topMin)) {
      closeCluster();
    }
    let col = colEnds.findIndex((end) => end <= seg.topMin);
    if (col === -1) {
      col = colEnds.length;
      colEnds.push(layoutEnd);
    } else {
      colEnds[col] = layoutEnd;
    }
    seg.col = col;
    cluster.push(seg);
  }
  closeCluster();
  return clipped;
}

// --- duration and form parsing ------------------------------------------------

/** Duration the way a streamer says it. A round hour reads as hours; anything
 * else keeps its minutes rather than being rounded into a lie about a 90-minute
 * slot. */
export function formatDuration(min: number): string {
  if (min <= 0) {
    return "-";
  }
  if (min % 60 === 0) {
    return `${min / 60}h`;
  }
  return min < 60 ? `${min}m` : `${Math.floor(min / 60)}h ${min % 60}m`;
}

/** Accepts "4h", "90m", "1h30", "1h 30m", "2" (a bare number is hours, since that
 * is what people mean when they type it into a stream planner). */
export function parseDuration(text: string): number {
  const s = text.trim().toLowerCase();
  if (s === "") {
    return 60;
  }
  // Hours followed by bare minutes, which the h/m pair below cannot see: "1h30"
  // has no `m` for it to match, so it used to read as a flat hour.
  const hm = /^(\d+(?:\.\d+)?)\s*h\s*(\d+)\s*m?$/.exec(s);
  if (hm) {
    return Math.max(1, Math.round(parseFloat(hm[1]) * 60 + parseInt(hm[2], 10)));
  }
  const h = /(\d+(?:\.\d+)?)\s*h/.exec(s);
  const m = /(\d+)\s*m/.exec(s);
  if (h || m) {
    return Math.max(1, Math.round((h ? parseFloat(h[1]) * 60 : 0) + (m ? parseInt(m[1], 10) : 0)));
  }
  const bare = parseFloat(s);
  return Number.isFinite(bare) ? Math.max(1, Math.round(bare * 60)) : 60;
}

const pad2 = (n: number): string => String(n).padStart(2, "0");

/** "YYYY-MM-DD" for an <input type="date">, in local time. */
export function toDateInput(ms: number): string {
  const d = new Date(ms);
  return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}`;
}

/** "HH:MM" for an <input type="time">, in local time. */
export function toTimeInput(ms: number): string {
  const d = new Date(ms);
  return `${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
}

/** The inverse of the two above, or NaN when either field is not a complete value.
 * A cleared date input reports "", which read through Number() is 0 and builds a
 * silent year-1900 entry rather than an obvious failure -- so the caller is given
 * something it must check instead. */
export function fromDateTimeInput(date: string, time: string): number {
  const d = /^(\d{4})-(\d{2})-(\d{2})$/.exec(date.trim());
  const t = /^(\d{1,2}):(\d{2})/.exec(time.trim());
  if (!d || !t) {
    return NaN;
  }
  return new Date(+d[1], +d[2] - 1, +d[3], +t[1], +t[2], 0, 0).getTime();
}

/** "19:00" for a block label. */
export function clockLabel(ms: number): string {
  return toTimeInput(ms);
}
