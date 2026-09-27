import { describe, expect, test } from "bun:test";
import { byEventTime, eventScope, scopeRows } from "$lib/docks/events/eventScope";
import type { FeedRow } from "$lib/utils/feedVirtualizer.svelte";

const LAUNCH = 1_000_000;

describe("eventScope", () => {
  test("with no broadcast this session, the scope starts at app launch", () => {
    expect(eventScope([], LAUNCH)).toEqual({ since: LAUNCH, kind: "launch" });
  });

  test("a broadcast from an earlier launch does not count", () => {
    const sessions = [{ startedAt: LAUNCH - 5000, endedAt: LAUNCH - 1000 }];
    expect(eventScope(sessions, LAUNCH)).toEqual({ since: LAUNCH, kind: "launch" });
  });

  test("on air, the scope starts at the running broadcast", () => {
    const sessions = [
      { startedAt: LAUNCH + 9000, endedAt: null },
      { startedAt: LAUNCH + 1000, endedAt: LAUNCH + 4000 },
    ];
    expect(eventScope(sessions, LAUNCH)).toEqual({ since: LAUNCH + 9000, kind: "live" });
  });

  test("off air, the scope starts at the most recent broadcast this session, in any order", () => {
    const sessions = [
      { startedAt: LAUNCH + 1000, endedAt: LAUNCH + 4000 },
      { startedAt: LAUNCH + 6000, endedAt: LAUNCH + 8000 },
      { startedAt: LAUNCH - 100, endedAt: LAUNCH - 50 },
    ];
    expect(eventScope(sessions, LAUNCH)).toEqual({ since: LAUNCH + 6000, kind: "last" });
  });
});

function rows(...ts: number[]): FeedRow<{ ts: number; id: string }>[] {
  return ts.map((t, i) => ({ clientKey: i + 1, item: { ts: t, id: "e" + (i + 1) } }));
}

describe("byEventTime", () => {
  test("orders by the event's own time, not arrival", () => {
    // A re-seeded 2020 follower arrives last but is the oldest event.
    const sorted = byEventTime(rows(500, 700, 100));
    expect(sorted.map((r) => r.item.id)).toEqual(["e3", "e1", "e2"]);
  });

  test("ties keep arrival order", () => {
    const sorted = byEventTime(rows(300, 300, 100, 300));
    expect(sorted.map((r) => r.item.id)).toEqual(["e3", "e1", "e2", "e4"]);
  });

  test("does not reorder its input", () => {
    const input = rows(3, 1, 2);
    byEventTime(input);
    expect(input.map((r) => r.item.id)).toEqual(["e1", "e2", "e3"]);
  });
});

describe("scopeRows", () => {
  const feed = byEventTime(rows(100, 200, 1000, 1500));

  test("hides events before the scope and counts them", () => {
    const { shown, earlier } = scopeRows(feed, 1000, false);
    expect(shown.map((r) => r.item.ts)).toEqual([1000, 1500]);
    expect(earlier).toBe(2);
  });

  test("show earlier reveals everything in memory and keeps the count", () => {
    const { shown, earlier } = scopeRows(feed, 1000, true);
    expect(shown.map((r) => r.item.ts)).toEqual([100, 200, 1000, 1500]);
    expect(earlier).toBe(2);
  });

  test("an event exactly at the boundary is in scope", () => {
    expect(scopeRows(feed, 200, false).shown[0].item.ts).toBe(200);
  });
});
