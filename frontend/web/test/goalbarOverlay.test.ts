import { describe, expect, test } from "bun:test";
import { COUNTER_EVENT_SOURCES, sourceContribution } from "../src/overlay/counter";

// The default goal bar, loaded as source against stand-ins for the globals it touches, the
// way eventRedaction.test.ts loads the ticker.

const SOURCE = await Bun.file(new URL("../public/overlay/default-goalbar/template.js", import.meta.url)).text();

class El {
  textContent = "";
  style = { width: "", setProperty() {} };
}

function goalbar(fields: Record<string, unknown> = {}, preview = false) {
  const ids: Record<string, El> = {};
  const document = { getElementById: (id: string) => (ids[id] ??= new El()), documentElement: new El() };
  let onLoad: (ctx: { fields: Record<string, unknown> }) => void = () => {};
  let onEvent: (e: unknown) => void = () => {};
  let onBackfill: (events: unknown[]) => void = () => {};
  const overlay = {
    preview,
    onLoad: (fn: typeof onLoad) => (onLoad = fn),
    onEvent: (fn: typeof onEvent) => (onEvent = fn),
    onBackfill: (fn: typeof onBackfill) => (onBackfill = fn),
    counter: { sources: COUNTER_EVENT_SOURCES, contribution: sourceContribution },
    formatCount: (n: number) => String(n),
    formatMoney: (n: number, c: string) => `${(n / 100).toFixed(2)} ${c}`,
  };
  new Function("document", "OBSOverlay", SOURCE)(document, overlay);
  onLoad({ fields: { target: 10, showPercent: false, ...fields } });
  return {
    count: () => ids["goal-count"].textContent,
    fire: (e: Record<string, unknown>) => onEvent(e),
    backfill: (events: Record<string, unknown>[]) => onBackfill(events),
  };
}

const follow = (id: string, extra: Record<string, unknown> = {}) => ({ id, type: "follow", platform: "twitch", ...extra });

describe("goal bar progress", () => {
  test("a reloaded source rebuilds its progress from the backfill on top of its seed", () => {
    const g = goalbar({ startCurrent: 2 });
    expect(g.count()).toBe("2 / 10");
    g.backfill([follow("a"), follow("b"), { id: "c", type: "cheer", amount: 100 }]);
    expect(g.count()).toBe("4 / 10");
  });

  test("an event in both the backfill and the live stream counts once", () => {
    const g = goalbar();
    g.backfill([follow("a")]);
    g.fire(follow("a"));
    g.fire(follow("b"));
    expect(g.count()).toBe("2 / 10");
  });

  test("a reconnect's backfill replaces what the page had, rather than adding to it", () => {
    const g = goalbar();
    g.backfill([follow("a")]);
    g.fire(follow("b"));
    g.backfill([follow("a"), follow("b"), follow("c")]);
    expect(g.count()).toBe("3 / 10");
  });

  test("a replay never counts again", () => {
    const g = goalbar();
    g.fire(follow("a"));
    g.fire(follow("a2", { replay: true }));
    expect(g.count()).toBe("1 / 10");
  });

  test("a test frame counts only in the editor preview, until the next backfill", () => {
    const onStream = goalbar();
    onStream.fire(follow("t1", { test: true }));
    expect(onStream.count()).toBe("0 / 10");

    const preview = goalbar({}, true);
    preview.fire(follow("t1", { test: true }));
    preview.fire(follow("t1", { test: true })); // the host reuses no id, but a test is never deduped
    expect(preview.count()).toBe("2 / 10");
    preview.backfill([follow("a")]);
    expect(preview.count()).toBe("1 / 10");
  });

  test("gifted subs and bits count by the Counter's rule", () => {
    const gifts = goalbar({ goalType: "giftedsubs" });
    gifts.fire({ id: "a", type: "subgift", count: 5 });
    gifts.fire({ id: "b", type: "subgift" }); // count unreported: still at least one sub
    expect(gifts.count()).toBe("6 / 10");
    const bits = goalbar({ goalType: "bits", target: 1000 });
    bits.fire({ id: "a", type: "cheer", amount: 250 });
    bits.fire({ id: "b", type: "follow" });
    expect(bits.count()).toBe("250 / 1000");
  });

  test("an unknown or inherited goal type falls back to followers", () => {
    const g = goalbar({ goalType: "toString" });
    g.fire({ id: "a", type: "follow" });
    expect(g.count()).toBe("1 / 10");
  });

  test("a donations goal counts only its own currency, from the backfill too", () => {
    const g = goalbar({ goalType: "donations", currency: "usd", target: 20 });
    g.backfill([
      { id: "a", type: "superchat", amount: 500, currency: "USD" },
      { id: "b", type: "superchat", amount: 50000, currency: "INR" },
    ]);
    g.fire({ id: "c", type: "supersticker", amount: 250, currency: "USD" });
    expect(g.count()).toBe("7.50 USD / 20.00 USD");
  });
});
