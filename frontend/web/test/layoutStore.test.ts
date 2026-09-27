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
const { createLayoutPersister, layoutStore } = await import("$lib/docking/layoutStore.svelte");

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
  let nextId = 1;
  let layout: { grid: string; activeGroup?: string };
  let written: string[];

  beforeEach(() => {
    timers = new Map();
    layout = { grid: "default", activeGroup: "1" };
    written = [];
    (globalThis as Record<string, unknown>).setTimeout = (fn: () => void) => {
      timers.set(nextId, fn);
      return nextId++;
    };
    (globalThis as Record<string, unknown>).clearTimeout = (id: number) => timers.delete(id);
  });
  afterEach(() => {
    globalThis.setTimeout = realSetTimeout;
    globalThis.clearTimeout = realClearTimeout;
  });

  function elapse(): void {
    const due = [...timers.values()];
    timers.clear();
    for (const fn of due) {
      fn();
    }
  }

  // As StudioPage sets it up after a restore: the layout on screen is the baseline.
  function persister(trustGestures = true) {
    const p = createLayoutPersister(
      () => layout as never,
      (s) => written.push(s),
    );
    p.settle();
    if (trustGestures) p.allowGestures();
    return p;
  }
  function change(to: Partial<typeof layout>, p: ReturnType<typeof persister>): void {
    layout = { ...layout, ...to };
    p.changed();
  }

  test("a restore or fallback default is never written before the user arms it", () => {
    const p = persister();
    change({ grid: "reconciled" }, p);
    elapse();
    expect(written).toEqual([]);
  });

  test("once armed, a burst of changes is one trailing save of the latest layout", () => {
    const p = persister();
    p.armFromGesture();
    change({ grid: "a" }, p);
    change({ grid: "b" }, p);
    elapse();
    expect(written.map((w) => JSON.parse(w).grid)).toEqual(["b"]);
  });

  test("a group taking focus is no reason to write", () => {
    const p = persister();
    p.armExplicit();
    change({ activeGroup: "2" }, p);
    elapse();
    expect(written).toEqual([]);
    change({ grid: "moved", activeGroup: "3" }, p);
    elapse();
    expect(JSON.parse(written[0])).toEqual({ grid: "moved", activeGroup: "3" }); // saved whole
  });

  test("an unchanged arrangement is not written twice", () => {
    const p = persister();
    p.armExplicit();
    change({ grid: "x" }, p);
    elapse();
    change({ activeGroup: "9" }, p);
    elapse();
    expect(written.length).toBe(1);
  });

  test("with a failed layout not copied aside, gestures cannot arm; explicit actions can", () => {
    const p = persister(false);
    p.armFromGesture();
    change({ grid: "dragged" }, p);
    elapse();
    expect(written).toEqual([]);
    p.armExplicit();
    change({ grid: "reset" }, p);
    elapse();
    expect(written.length).toBe(1);
  });

  test("dispose drops a pending save", () => {
    const p = persister();
    p.armExplicit();
    change({ grid: "y" }, p);
    p.dispose();
    elapse();
    expect(written).toEqual([]);
  });
});
