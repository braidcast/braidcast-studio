// Calendar geometry and conflict math. Pure functions over epoch milliseconds and
// plain records: nothing here touches the DOM, reads a store, or calls the bridge,
// so the packing and the destination-conflict rule can be exercised on their own.
//
// Everything is computed in LOCAL time via Date, never by adding 86_400_000: a
// calendar shows the user's own days, and a DST boundary makes a day 23 or 25
// hours long. Epoch arithmetic here would silently shift a whole column.

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

/** Two blocks closer together than this are packed side by side even though their
 * times do not strictly overlap: below it the taller one's minimum height would
 * cover the shorter one and the grid would look like it lost an entry. */
const MIN_LAYOUT_MIN = 20;

/** States the runner has not settled. Only these can still be edited, and only
 * these can conflict -- a missed or canceled entry will never occupy a profile. */
export const LIVE_STATES: readonly ScheduleState[] = ["planned", "armed", "live"];

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
  profileIds: string[];
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
      profileIds: e.destinations.map((d) => d.profileId),
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
      profileIds: s.destinations.map((d) => d.profileId),
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
  if (item.countdownCanceled) {
    return "canceled";
  }
  if (item.state !== "armed") {
    return "none";
  }
  if (item.autoStart && nowMs >= item.start - COUNTDOWN_LEAD_MS && nowMs < item.start) {
    return "countdown";
  }
  return "armed";
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

/** Minutes from the start of the local day containing `dayStart`. Computed from
 * the real day length so a DST day maps onto the grid without a one-hour skew. */
export function minutesInto(dayStart: number, ms: number): number {
  return Math.round((ms - dayStart) / MS_MIN);
}

/** `dayStart` plus `minutes`, snapped to SNAP_MIN. */
export function atMinute(dayStart: number, minutes: number): number {
  const snapped = Math.round(minutes / SNAP_MIN) * SNAP_MIN;
  return dayStart + Math.min(Math.max(snapped, 0), MINUTES_PER_DAY) * MS_MIN;
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
export function daySegments(items: readonly CalendarItem[], dayStart: number): DaySegment[] {
  const dayEnd = addDays(dayStart, 1);
  const clipped = items
    .filter((i) => i.start < dayEnd && i.end > dayStart)
    .map((i) => ({
      key: i.key,
      item: i,
      topMin: Math.max(0, minutesInto(dayStart, i.start)),
      endMin: Math.min(MINUTES_PER_DAY, minutesInto(dayStart, i.end)),
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

/** Accepts "4h", "90m", "1h30", "2" (a bare number is hours, since that is what
 * people mean when they type it into a stream planner). */
export function parseDuration(text: string): number {
  const s = text.trim().toLowerCase();
  if (s === "") {
    return 60;
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

/** The inverse of the two above. */
export function fromDateTimeInput(date: string, time: string): number {
  const [y, m, d] = date.split("-").map(Number);
  const [hh, mm] = time.split(":").map(Number);
  return new Date(y, (m || 1) - 1, d || 1, hh || 0, mm || 0, 0, 0).getTime();
}

/** "19:00" for a block label. */
export function clockLabel(ms: number): string {
  return toTimeInput(ms);
}
