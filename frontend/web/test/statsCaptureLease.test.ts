import { afterAll, beforeEach, describe, expect, mock, test } from "bun:test";
import { flushSync } from "svelte";

// The bridge module wires window globals on import; the store needs call and on.
const calls: string[] = [];
let failWith: Error | null = null;
// When set, stats.watchCaptures stays unanswered until release() runs.
let hold: Promise<void> | null = null;
mock.module("$lib/api/bridge", () => ({
  obs: {
    call: async (method: string) => {
      calls.push(method);
      if (hold && method === "stats.watchCaptures") {
        await hold;
      }
      if (failWith) {
        throw failWith;
      }
      return method === "stats.get" ? null : { ok: true, leaseMs: 5000 };
    },
    on: () => () => {},
  },
}));

// Just enough document for the window-visibility gate.
let visibility: "visible" | "hidden" = "visible";
const listeners = new Set<() => void>();
const realDocument = (globalThis as Record<string, unknown>).document;
(globalThis as Record<string, unknown>).document = {
  get visibilityState() {
    return visibility;
  },
  addEventListener: (_: string, fn: () => void) => listeners.add(fn),
  removeEventListener: (_: string, fn: () => void) => listeners.delete(fn),
};
function setVisibility(v: "visible" | "hidden"): void {
  visibility = v;
  for (const fn of [...listeners]) {
    fn();
  }
}

// Manual intervals, so renewals are counted rather than waited for.
const intervals = new Map<number, () => void>();
let nextId = 1;
const realSetInterval = globalThis.setInterval;
const realClearInterval = globalThis.clearInterval;
(globalThis as Record<string, unknown>).setInterval = (fn: () => void) => {
  intervals.set(nextId, fn);
  return nextId++;
};
(globalThis as Record<string, unknown>).clearInterval = (id: number) => intervals.delete(id);
afterAll(() => {
  (globalThis as Record<string, unknown>).document = realDocument;
  globalThis.setInterval = realSetInterval;
  globalThis.clearInterval = realClearInterval;
});

const { statsStore } = await import("$lib/stores/statsStore.svelte");

const settle = () => new Promise((r) => setTimeout(r, 0));
const renewals = () => calls.filter((c) => c === "stats.watchCaptures").length;
function tick(): void {
  for (const fn of [...intervals.values()]) {
    fn();
  }
}

beforeEach(() => {
  calls.length = 0;
  failWith = null;
  hold = null;
  visibility = "visible";
});

describe("statsStore capture lease", () => {
  test("two viewers share one renewal, and one leaving does not end the other's lease", async () => {
    const a = statsStore.watchCaptures();
    const b = statsStore.watchCaptures();
    expect(renewals()).toBe(1);
    expect(intervals.size).toBe(1);
    await settle();
    tick();
    expect(renewals()).toBe(2);
    await settle();
    a();
    a();
    tick();
    expect(renewals()).toBe(3);
    b();
    expect(intervals.size).toBe(0);
    expect(listeners.size).toBe(0);
  });

  test("renews only while the window is visible, and at once when it returns", async () => {
    const off = statsStore.watchCaptures();
    expect(renewals()).toBe(1);
    await settle();
    setVisibility("hidden");
    expect(intervals.size).toBe(0);
    setVisibility("visible");
    expect(renewals()).toBe(2);
    expect(intervals.size).toBe(1);
    off();
  });

  test("an empty list is settled only by a sample the lease covered", async () => {
    statsStore.stats = {
      general: {} as never,
      outputs: [],
      captures: [],
      sampledAtMs: Date.now() - 10,
    };
    const off = statsStore.watchCaptures();
    await settle();
    flushSync();
    expect(statsStore.capturesSettled).toBe(false);
    statsStore.stats = { ...statsStore.stats, sampledAtMs: Date.now() + 1000 };
    flushSync();
    expect(statsStore.capturesSettled).toBe(true);
    off();
    flushSync();
    expect(statsStore.capturesSettled).toBe(false);
  });

  test("a host that stops answering is not sent a queue of renewals", async () => {
    let release = (): void => {};
    hold = new Promise((r) => (release = r));
    const off = statsStore.watchCaptures();
    tick();
    tick();
    expect(renewals()).toBe(1);
    release();
    await settle();
    tick();
    expect(renewals()).toBe(2);
    off();
    await settle();
  });

  test("an unanswered renewal is taken as lost after one lease, so renewal resumes", async () => {
    hold = new Promise(() => {});
    const realNow = Date.now;
    let now = realNow();
    Date.now = () => now;
    try {
      const off = statsStore.watchCaptures();
      tick();
      expect(renewals()).toBe(1);
      now += 4999;
      tick();
      expect(renewals()).toBe(1);
      now += 1;
      tick();
      expect(renewals()).toBe(2);
      tick();
      expect(renewals()).toBe(2);
      off();
    } finally {
      Date.now = realNow;
    }
  });

  test("a failed renewal drops the grant, so only a sample after the next grant settles", async () => {
    const realNow = Date.now;
    let now = realNow();
    Date.now = () => now;
    try {
      statsStore.stats = { general: {} as never, outputs: [], captures: [], sampledAtMs: now - 10 };
      const off = statsStore.watchCaptures();
      await settle();
      const firstGrant = now;
      statsStore.stats = { ...statsStore.stats, sampledAtMs: firstGrant + 1000 };
      flushSync();
      expect(statsStore.capturesSettled).toBe(true);

      now = firstGrant + 2000;
      failWith = new Error("lost");
      tick();
      await settle();
      flushSync();
      expect(statsStore.capturesSettled).toBe(false);

      // The host lease lapsed meanwhile; a sample taken before the re-grant, though after
      // the first grant, must not read as settled.
      now = firstGrant + 9000;
      failWith = null;
      tick();
      await settle();
      statsStore.stats = { ...statsStore.stats, sampledAtMs: firstGrant + 8000 };
      flushSync();
      expect(statsStore.capturesSettled).toBe(false);
      statsStore.stats = { ...statsStore.stats, sampledAtMs: firstGrant + 10000 };
      flushSync();
      expect(statsStore.capturesSettled).toBe(true);
      off();
    } finally {
      Date.now = realNow;
    }
  });

  test("a failed renewal is reported, and cleared when the lease is dropped", async () => {
    failWith = new Error("no such method");
    const off = statsStore.watchCaptures();
    await settle();
    expect(statsStore.captureWatchError).toBe("no such method");
    off();
    expect(statsStore.captureWatchError).toBeNull();
  });
});
