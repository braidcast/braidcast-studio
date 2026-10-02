import { afterAll, beforeEach, describe, expect, mock, test } from "bun:test";
import type { LiveGoal } from "$lib/api/bridge";
import {
  GOAL_LINGER_MS,
  goalOver,
  goalProgress,
  goalTitle,
  nextExpiryMs,
  shownGoals,
} from "$lib/docks/multichat/goalView";

// Goals as goals.changed carries them, after the host registry has merged the partial updates.
// The values follow replay captures of real gift goals (2026-10-02): "15 gifts" overshot to 26,
// and a 50-gift goal ended at 2.
function goal(over: Partial<LiveGoal> = {}): LiveGoal {
  return {
    id: "youtube:UCa@profile-a|goal-A",
    platform: "youtube",
    accountId: "youtube:UCa",
    profileUuid: "profile-a",
    key: "goal-A",
    state: "CREATOR_GOAL_STATE_ACTIVE",
    phase: "active",
    description: "Send gifts for a jumpscare",
    target: "50 gifts",
    headline: "Goal in progress",
    current: 12,
    total: 50,
    endedAtMs: null,
    heldUntilMs: null,
    ...over,
  };
}

describe("goalProgress", () => {
  test("pairs the count with the target text when it states the total", () => {
    expect(goalProgress(goal())).toEqual({ fraction: 0.24, label: "12 / 50 gifts", spoken: "12 of 50 gifts" });
  });

  test("an overshoot fills the bar and still says the real count", () => {
    const p = goalProgress(goal({ current: 26, total: 15, target: "15 gifts", phase: "achieved" }));
    expect(p.fraction).toBe(1);
    expect(p.label).toBe("26 / 15 gifts");
  });

  test("a target that is not the total (a money goal) keeps the bare numbers", () => {
    expect(goalProgress(goal({ current: 25, total: 100, target: "$100.00" })).label).toBe("25 / 100");
  });

  test("a grouped target still matches its total", () => {
    expect(goalProgress(goal({ current: 1200, total: 1000, target: "1,000 gifts" })).label).toBe("1,200 / 1,000 gifts");
  });

  test("a goal joined part-way, with no total, shows the count alone and no fill", () => {
    expect(goalProgress(goal({ total: null, target: "" }))).toEqual({ fraction: null, label: "12", spoken: "12" });
  });

  test("a zero or unreadable total never divides", () => {
    for (const total of [0, Number.NaN, -5, Number.POSITIVE_INFINITY]) {
      const p = goalProgress(goal({ total }));
      expect(p.fraction).toBeNull();
      expect(p.label).toBe("12");
    }
  });

  test("no count shows the target and never NaN", () => {
    const p = goalProgress(goal({ current: null }));
    expect(p.fraction).toBeNull();
    expect(p.label).toBe("50 gifts");
    expect(JSON.stringify(p)).not.toContain("NaN");
    expect(goalProgress(goal({ current: null, total: null, target: "" })).label).toBe("");
  });
});

describe("goal title and phase", () => {
  test("the description, else YouTube's headline, else a plain word", () => {
    expect(goalTitle(goal())).toBe("Send gifts for a jumpscare");
    expect(goalTitle(goal({ description: "  " }))).toBe("Goal in progress");
    expect(goalTitle(goal({ description: "", headline: "" }))).toBe("Goal");
  });

  test("achieved and ended are over; an unknown state still runs", () => {
    expect(goalOver(goal({ phase: "achieved" }))).toBe(true);
    expect(goalOver(goal({ phase: "ended" }))).toBe(true);
    expect(goalOver(goal({ phase: "unknown" }))).toBe(false);
    expect(goalOver(goal())).toBe(false);
  });
});

describe("finished goals linger, then go", () => {
  const running = goal({ id: "a" });
  const done = goal({ id: "b", phase: "achieved", endedAtMs: 10_000 });

  test("a finished goal shows for the linger and not after", () => {
    expect(shownGoals([running, done], 10_000 + GOAL_LINGER_MS - 1).map((g) => g.id)).toEqual(["a", "b"]);
    expect(shownGoals([running, done], 10_000 + GOAL_LINGER_MS).map((g) => g.id)).toEqual(["a"]);
  });

  test("the next wake-up is the soonest linger still to run out", () => {
    const later = goal({ id: "c", phase: "ended", endedAtMs: 20_000 });
    expect(nextExpiryMs([running, done, later], 15_000)).toBe(10_000 + GOAL_LINGER_MS - 15_000);
    expect(nextExpiryMs([running, done, later], 10_000 + GOAL_LINGER_MS)).toBe(10_000);
    expect(nextExpiryMs([running], 0)).toBeNull();
  });

  test("a held goal goes when its hold runs out, or its linger if that is sooner", () => {
    const held = goal({ id: "h", heldUntilMs: 30_000 });
    const heldDone = goal({ id: "d", phase: "achieved", endedAtMs: 0, heldUntilMs: 90_000 });
    expect(shownGoals([held, heldDone], 29_999).map((g) => g.id)).toEqual(["h", "d"]);
    expect(shownGoals([held, heldDone], 30_000).map((g) => g.id)).toEqual(["d"]);
    expect(nextExpiryMs([held, heldDone], 30_000)).toBe(GOAL_LINGER_MS - 30_000);
  });
});

// The store, over a bridge stand-in: goals.changed replaces the list, a stale goals.list does
// not undo it, and a finished goal leaves on a timer.
let listAnswer: (() => void) | null = null;
let listGoals: LiveGoal[] = [];
let onChanged: ((p: { goals: LiveGoal[] }) => void) | null = null;
mock.module("$lib/api/bridge", () => ({
  obs: {
    call: async (method: string) => {
      if (method !== "goals.list") {
        throw new Error("unexpected " + method);
      }
      const snapshot = listGoals;
      await new Promise<void>((r) => (listAnswer = r));
      return { goals: snapshot };
    },
    on: (event: string, fn: (p: { goals: LiveGoal[] }) => void) => {
      if (event === "goals.changed") {
        onChanged = fn;
      }
      return () => {};
    },
  },
}));

// Manual timeouts, so the linger is stepped rather than waited for.
const timeouts = new Map<number, { fn: () => void; ms: number }>();
let nextTimer = 1;
const realSetTimeout = globalThis.setTimeout;
const realClearTimeout = globalThis.clearTimeout;
(globalThis as Record<string, unknown>).setTimeout = (fn: () => void, ms: number) => {
  timeouts.set(nextTimer, { fn, ms });
  return nextTimer++;
};
(globalThis as Record<string, unknown>).clearTimeout = (id: number) => timeouts.delete(id);
afterAll(() => {
  globalThis.setTimeout = realSetTimeout;
  globalThis.clearTimeout = realClearTimeout;
});

const { goalStore } = await import("$lib/stores/goalStore.svelte");
const settle = () => new Promise<void>((r) => queueMicrotask(r));

describe("goalStore", () => {
  beforeEach(() => {
    timeouts.clear();
  });

  test("an event that lands while the first list is in flight wins over it", async () => {
    listGoals = [goal({ current: 1 })];
    goalStore.start();
    onChanged?.({ goals: [goal({ current: 2 })] });
    listAnswer?.();
    await settle();
    await settle();
    expect(goalStore.goals.map((g) => g.current)).toEqual([2]);
  });

  test("a finished goal is shown, then dropped when its linger runs out", () => {
    const endedAt = Date.now();
    onChanged?.({ goals: [goal({ id: "a" }), goal({ id: "b", phase: "achieved", endedAtMs: endedAt })] });
    expect(goalStore.shown.map((g) => g.id)).toEqual(["a", "b"]);
    expect(timeouts.size).toBe(1);
    const [[, timer]] = [...timeouts];
    expect(timer.ms).toBeGreaterThan(0);
    expect(timer.ms).toBeLessThanOrEqual(GOAL_LINGER_MS);

    const realNow = Date.now;
    Date.now = () => endedAt + GOAL_LINGER_MS;
    try {
      timeouts.clear();
      timer.fn();
    } finally {
      Date.now = realNow;
    }
    expect(goalStore.shown.map((g) => g.id)).toEqual(["a"]);
    expect(timeouts.size).toBe(0);
  });

  test("the list emptying when a chat read stops clears the row", () => {
    onChanged?.({ goals: [goal()] });
    expect(goalStore.shown).toHaveLength(1);
    onChanged?.({ goals: [] });
    expect(goalStore.shown).toHaveLength(0);
  });
});
