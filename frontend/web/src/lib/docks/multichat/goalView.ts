import type { LiveGoal } from "$lib/api/bridge";

// How the Chat dock reads a creator goal (goals.*): the progress it shows, and how long a
// finished goal stays up. Pure, so the store and its tests share one reading.

/** How long an achieved or ended goal stays in the dock, the way YouTube leaves the result up
 * for a while before its goal chip goes. */
export const GOAL_LINGER_MS = 60_000;

export interface GoalProgress {
  /** The filled share, 0..1 -- capped at 1, since gifts can overshoot the target. Null when
   * there is no count and total to measure, which draws no fill at all. */
  fraction: number | null;
  /** The count as shown: "12 / 50 gifts", "12 / 50", "12", "50 gifts" or "". */
  label: string;
  /** The same, worded for a screen reader: "12 of 50 gifts". */
  spoken: string;
}

function count(n: number | null | undefined): number | null {
  return typeof n === "number" && Number.isFinite(n) && n >= 0 ? Math.floor(n) : null;
}

// The target text's unit when it states the total itself ("50 gifts", "1,000 gifts"), so the
// row can read "12 / 50 gifts". Null when it says something else (a money target, say), which
// keeps the bare numbers rather than pairing them with a phrase that does not match.
function targetWithTotal(target: string, total: number): string | null {
  const lead = /^[\d.,\s  ]+/.exec(target.trim());
  if (!lead || lead[0].replace(/\D/g, "") !== String(total)) {
    return null;
  }
  return target.trim();
}

export function goalProgress(g: LiveGoal): GoalProgress {
  const current = count(g.current);
  const total = count(g.total);
  const usableTotal = total !== null && total > 0 ? total : null;
  if (current === null) {
    const target = g.target.trim();
    return { fraction: null, label: target, spoken: target ? "target " + target : "" };
  }
  if (usableTotal === null) {
    const text = current.toLocaleString();
    return { fraction: null, label: text, spoken: text };
  }
  const now = current.toLocaleString();
  const withUnit = targetWithTotal(g.target, usableTotal);
  const of = withUnit ?? usableTotal.toLocaleString();
  return {
    fraction: Math.min(1, current / usableTotal),
    label: `${now} / ${of}`,
    spoken: `${now} of ${of}`,
  };
}

/** What the row is called: the creator's description, else YouTube's status line. */
export function goalTitle(g: LiveGoal): string {
  return g.description.trim() || g.headline.trim() || "Goal";
}

/** Whether the goal is over: achieved or ended. A state not seen yet still shows as running. */
export function goalOver(g: LiveGoal): boolean {
  return g.phase === "achieved" || g.phase === "ended";
}

function lingersUntil(g: LiveGoal, lingerMs: number): number | null {
  return g.endedAtMs === null ? null : g.endedAtMs + lingerMs;
}

/** The goals to draw at `nowMs`: every running goal, and a finished one for `lingerMs`. */
export function shownGoals(goals: LiveGoal[], nowMs: number, lingerMs = GOAL_LINGER_MS): LiveGoal[] {
  return goals.filter((g) => {
    const until = lingersUntil(g, lingerMs);
    return until === null || nowMs < until;
  });
}

/** Milliseconds from `nowMs` until the next shown goal stops lingering, or null when none will. */
export function nextExpiryMs(goals: LiveGoal[], nowMs: number, lingerMs = GOAL_LINGER_MS): number | null {
  let soonest: number | null = null;
  for (const g of goals) {
    const until = lingersUntil(g, lingerMs);
    if (until !== null && until > nowMs && (soonest === null || until < soonest)) {
      soonest = until;
    }
  }
  return soonest === null ? null : soonest - nowMs;
}
