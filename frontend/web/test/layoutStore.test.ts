import { afterEach, beforeEach, describe, expect, mock, test } from "bun:test";
import type { DockviewApi } from "dockview-core";

// The bridge module wires window globals on import; the store only needs obs.call.
const calls: { method: string; params?: unknown }[] = [];
let answers: Record<string, unknown> = {};
mock.module("$lib/api/bridge", () => ({
  obs: {
    call: async (method: string, params?: unknown) => {
      calls.push({ method, params });
      const a = answers[method];
      if (a instanceof Error) {
        throw a;
      }
      return a;
    },
  },
}));
const { createLayoutPersister, layoutStore, SAVE_RETRIES } = await import("$lib/docking/layoutStore.svelte");

function dockview(fromJSON: (data: unknown) => void): DockviewApi {
  return { fromJSON } as unknown as DockviewApi;
}

beforeEach(() => {
  calls.length = 0;
  answers = {};
});

describe("layoutStore.restore", () => {
  test("a layout on disk that could not be read is 'failed', not 'none'", async () => {
    answers["layout.load"] = { layout: "", present: true };
    expect(await layoutStore.restore(dockview(() => {}))).toBe("failed");
  });

  test("nothing saved is 'none', not a failure", async () => {
    answers["layout.load"] = { layout: "", present: false };
    expect(await layoutStore.restore(dockview(() => {}))).toBe("none");
    answers["layout.load"] = null;
    expect(await layoutStore.restore(dockview(() => {}))).toBe("none");
  });

  test("a saved layout that applies is 'restored'", async () => {
    answers["layout.load"] = { layout: '{"grid":1}' };
    let applied: unknown;
    expect(await layoutStore.restore(dockview((d) => (applied = d)))).toBe("restored");
    expect(applied).toEqual({ grid: 1 });
  });

  test("a saved layout that throws while applying is 'failed', and nothing is written", async () => {
    answers["layout.load"] = { layout: '{"grid":1}' };
    const r = await layoutStore.restore(
      dockview(() => {
        throw new ReferenceError("Cannot access 'F' before initialization");
      }),
    );
    expect(r).toBe("failed");
    expect(await layoutStore.restore(dockview(() => {}))).toBe("restored");
    answers["layout.load"] = { layout: "{not json" };
    expect(await layoutStore.restore(dockview(() => {}))).toBe("failed");
    expect(calls.every((c) => c.method === "layout.load")).toBe(true);
  });

  test("quarantine reports the copy's name, or '' when the host has none", async () => {
    answers["layout.quarantine"] = { file: "layout.failed-2026-09-27_22-34-05.json" };
    expect(await layoutStore.quarantine()).toBe("layout.failed-2026-09-27_22-34-05.json");
    answers["layout.quarantine"] = new Error("unknown method");
    expect(await layoutStore.quarantine()).toBe("");
  });
});

describe("createLayoutPersister", () => {
  const realSetTimeout = globalThis.setTimeout;
  const realClearTimeout = globalThis.clearTimeout;
  let timers: Map<number, () => void>;
  let delays: Map<number, number>;
  let nextId = 1;
  let layout: { grid: string; activeGroup?: string };
  let written: string[];
  let writeOk: boolean;

  beforeEach(() => {
    timers = new Map();
    delays = new Map();
    layout = { grid: "default", activeGroup: "1" };
    written = [];
    writeOk = true;
    (globalThis as Record<string, unknown>).setTimeout = (fn: () => void, ms: number) => {
      timers.set(nextId, fn);
      delays.set(nextId, ms);
      return nextId++;
    };
    (globalThis as Record<string, unknown>).clearTimeout = (id: number) => timers.delete(id);
  });
  afterEach(() => {
    globalThis.setTimeout = realSetTimeout;
    globalThis.clearTimeout = realClearTimeout;
  });

  // Fires the pending save and lets its write settle.
  async function elapse(): Promise<void> {
    const due = [...timers.values()];
    timers.clear();
    for (const fn of due) {
      fn();
    }
    await Promise.resolve();
    await Promise.resolve();
  }

  // The delay of the one timer pending, or undefined when none is.
  function pendingDelay(): number | undefined {
    const ids = [...timers.keys()];
    expect(ids.length).toBeLessThanOrEqual(1);
    return ids.length ? delays.get(ids[0]) : undefined;
  }

  // As StudioPage sets it up after a restore: the layout on screen is the baseline.
  function persister(trustGestures = true) {
    const p = createLayoutPersister(
      () => layout as never,
      async (s) => {
        written.push(s);
        return writeOk;
      },
    );
    p.settle();
    if (trustGestures) p.allowGestures();
    return p;
  }
  function change(to: Partial<typeof layout>, p: ReturnType<typeof persister>): void {
    layout = { ...layout, ...to };
    p.changed();
  }

  test("a restore or fallback default is never written before the user arms it", async () => {
    const p = persister();
    change({ grid: "reconciled" }, p);
    await elapse();
    expect(written).toEqual([]);
  });

  test("once armed, a burst of changes is one trailing save of the latest layout", async () => {
    const p = persister();
    p.armFromGesture();
    change({ grid: "a" }, p);
    change({ grid: "b" }, p);
    await elapse();
    expect(written.map((w) => JSON.parse(w).grid)).toEqual(["b"]);
  });

  test("a group taking focus is no reason to write", async () => {
    const p = persister();
    p.armExplicit();
    change({ activeGroup: "2" }, p);
    await elapse();
    expect(written).toEqual([]);
    change({ grid: "moved", activeGroup: "3" }, p);
    await elapse();
    expect(JSON.parse(written[0])).toEqual({ grid: "moved", activeGroup: "3" }); // saved whole
  });

  test("an unchanged arrangement is not written twice", async () => {
    const p = persister();
    p.armExplicit();
    change({ grid: "x" }, p);
    await elapse();
    change({ activeGroup: "9" }, p);
    await elapse();
    expect(written.length).toBe(1);
  });

  test("with a failed layout not copied aside, gestures cannot arm; explicit actions can", async () => {
    const p = persister(false);
    p.armFromGesture();
    change({ grid: "dragged" }, p);
    await elapse();
    expect(written).toEqual([]);
    p.armExplicit();
    change({ grid: "reset" }, p);
    await elapse();
    expect(written.length).toBe(1);
  });

  test("a failed write is retried for the same arrangement", async () => {
    const p = persister();
    p.armExplicit();
    writeOk = false;
    change({ grid: "x" }, p);
    await elapse();
    writeOk = true;
    change({ activeGroup: "2" }, p); // same arrangement: only focus moved
    await elapse();
    expect(written.map((w) => JSON.parse(w).grid)).toEqual(["x", "x"]);
    change({ activeGroup: "3" }, p); // now saved, so no third write
    await elapse();
    expect(written.length).toBe(2);
  });

  test("Reset always writes, even when it rebuilds the baseline", async () => {
    const p = persister(false); // a failed restore with no copy: the baseline is the default
    p.armRewrite();
    change({ grid: "default" }, p); // Reset rebuilt that same default
    await elapse();
    expect(written.map((w) => JSON.parse(w).grid)).toEqual(["default"]);
    change({ activeGroup: "2" }, p); // the rewrite was one-shot
    await elapse();
    expect(written.length).toBe(1);
  });

  test("dispose drops a pending save", async () => {
    const p = persister();
    p.armExplicit();
    change({ grid: "y" }, p);
    p.dispose();
    await elapse();
    expect(written).toEqual([]);
  });

  test("a failed write retries on its own, backing off to a cap, then gives up", async () => {
    const p = persister();
    p.armExplicit();
    writeOk = false;
    change({ grid: "x" }, p);
    const waits: number[] = [];
    await elapse(); // the save itself
    for (let d = pendingDelay(); d !== undefined; d = pendingDelay()) {
      waits.push(d);
      await elapse();
    }
    expect(waits).toEqual([1000, 2000, 4000, 8000, 16000, 30000]);
    expect(waits.length).toBe(SAVE_RETRIES);
    expect(written.length).toBe(1 + SAVE_RETRIES);
  });

  test("a retry that lands stops the run, and the saved arrangement is not rewritten", async () => {
    const p = persister();
    p.armExplicit();
    writeOk = false;
    change({ grid: "x" }, p);
    await elapse();
    writeOk = true;
    await elapse(); // the first retry
    expect(pendingDelay()).toBeUndefined();
    change({ activeGroup: "2" }, p);
    await elapse();
    expect(written.map((w) => JSON.parse(w).grid)).toEqual(["x", "x"]);
  });

  test("the next change cancels a pending retry and saves in its place", async () => {
    const p = persister();
    p.armExplicit();
    writeOk = false;
    change({ grid: "x" }, p);
    await elapse();
    expect(pendingDelay()).toBe(1000);
    writeOk = true;
    change({ grid: "y" }, p);
    expect(pendingDelay()).toBe(250); // the retry is gone; only the new save waits
    await elapse();
    expect(written.map((w) => JSON.parse(w).grid)).toEqual(["x", "y"]);
    expect(pendingDelay()).toBeUndefined();
  });

  test("a write failing after a newer change, or after dispose, schedules no retry", async () => {
    let fail: () => void = () => {};
    const p = createLayoutPersister(
      () => layout as never,
      (s) => {
        written.push(s);
        return new Promise<boolean>((resolve) => (fail = () => resolve(false)));
      },
    );
    p.settle();
    p.armExplicit();
    change({ grid: "x" }, p);
    await elapse(); // the write is in flight
    change({ grid: "y" }, p);
    fail();
    await Promise.resolve();
    await Promise.resolve();
    expect(pendingDelay()).toBe(250); // the newer save, not a retry of the old one
    await elapse();
    p.dispose();
    fail();
    await Promise.resolve();
    await Promise.resolve();
    expect(pendingDelay()).toBeUndefined();
  });

  test("dispose cancels a pending retry", async () => {
    const p = persister();
    p.armExplicit();
    writeOk = false;
    change({ grid: "x" }, p);
    await elapse();
    expect(pendingDelay()).toBe(1000);
    p.dispose();
    expect(pendingDelay()).toBeUndefined();
  });
});
