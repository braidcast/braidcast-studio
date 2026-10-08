// Shared scaffolding for the FeedVirtualizer tests: hand-run animation frames, a scroll
// container that clamps like Chromium, a fake host serving pages under test control, and
// a probe for the row under the reader's eye.
import { afterEach, beforeEach } from "bun:test";
import { flushSync as flush } from "svelte";
import type { FeedPage } from "$lib/api/bridge";
import { FeedVirtualizer, type FeedConfig } from "$lib/utils/feedVirtualizer.svelte";
import { pre, root } from "./harness.svelte";

// rAF, timers and ResizeObserver are browser globals the engine schedules through; the
// tests run frames and timers by hand so each lands exactly when asserted.
let frames = new Map<number, FrameRequestCallback>();
let timers = new Map<number, () => void>();
let nextHandle = 0;
const g = globalThis as Record<string, unknown>;

/** Install the frame, timer and ResizeObserver stand-ins around every test in the file. */
export function useFrameMocks(): void {
  const saved = {
    raf: g.requestAnimationFrame,
    caf: g.cancelAnimationFrame,
    st: g.setTimeout,
    ct: g.clearTimeout,
    ro: g.ResizeObserver,
  };
  beforeEach(() => {
    frames = new Map();
    timers = new Map();
    g.requestAnimationFrame = (cb: FrameRequestCallback) => {
      frames.set(++nextHandle, cb);
      return nextHandle;
    };
    g.cancelAnimationFrame = (handle: number) => frames.delete(handle);
    g.setTimeout = (cb: () => void) => {
      timers.set(++nextHandle, cb);
      return nextHandle;
    };
    g.clearTimeout = (handle: number) => timers.delete(handle);
    g.ResizeObserver = class {
      observe(): void {}
      disconnect(): void {}
    };
  });
  afterEach(() => {
    g.requestAnimationFrame = saved.raf;
    g.cancelAnimationFrame = saved.caf;
    g.setTimeout = saved.st;
    g.clearTimeout = saved.ct;
    g.ResizeObserver = saved.ro;
  });
}

/** Run the queued animation frames, then settle the effects they caused. */
export function frame(): void {
  const run = [...frames.values()];
  frames.clear();
  for (const cb of run) {
    cb(0);
  }
  flush();
}

/** Run the timers due so far, however long each was set for; true if any ran. */
export function timer(): boolean {
  const run = [...timers.values()];
  timers.clear();
  for (const cb of run) {
    cb();
  }
  flush();
  return run.length > 0;
}

/** Let resolved fetch replies reach their handlers, then settle the effects. */
export async function settle(): Promise<void> {
  for (let i = 0; i < 4; i++) {
    await Promise.resolve();
  }
  flush();
}

// A scroll container whose content height is the sizer's height IN THE DOM -- updated in
// the render phase, like the component's template, not the moment the layout derives it.
// So a scrollTop past the new maximum clamps exactly as Chromium clamps it once the sizer
// shrinks.
export class Scroller {
  clientHeight = 100;
  domTotal = 0;
  #top = 0;
  #listeners: (() => void)[] = [];
  get scrollHeight(): number {
    return Math.max(this.domTotal, this.clientHeight);
  }
  get scrollTop(): number {
    return this.#top;
  }
  set scrollTop(v: number) {
    this.#top = Math.max(0, Math.min(v, this.scrollHeight - this.clientHeight));
  }
  addEventListener(_type: string, fn: () => void): void {
    this.#listeners.push(fn);
  }
  removeEventListener(): void {}
  /** The reader scrolls: position changes, then the scroll event is delivered. */
  userScroll(top: number): void {
    this.scrollTop = top;
    this.scrollEvent();
  }
  /** A scroll event lands for wherever the box is now: Chromium delivers the one a
   * programmatic scroll queued at the next frame, after anything that changed meanwhile. */
  scrollEvent(): void {
    for (const fn of this.#listeners) {
      fn();
    }
  }
}

interface Call<T> {
  before?: T;
  limit: number;
  resolve: (page: FeedPage<T>) => void;
  reject: (e: Error) => void;
}

/** A host holding `store` (sorted by the feed's order) that answers each fetch only when
 * the test says so, reading the store as it stands at that moment. */
export class FakeHost<T> {
  store: T[] = [];
  epoch = 0;
  /** A page never holds more than this, whatever was asked (a filtered host, say). */
  maxLimit = Infinity;
  calls: Call<T>[] = [];
  readonly #compare: (a: T, b: T) => number;

  constructor(compare: (a: T, b: T) => number) {
    this.#compare = compare;
  }

  fetch = (q: { before?: T; limit: number }): Promise<FeedPage<T>> =>
    new Promise((resolve, reject) => this.calls.push({ ...q, resolve, reject }));

  page(before: T | undefined, limit: number): FeedPage<T> {
    const older = before === undefined ? this.store : this.store.filter((x) => this.#compare(x, before) < 0);
    const n = Math.min(limit, this.maxLimit);
    return { items: older.slice(Math.max(0, older.length - n)), more: older.length > n, epoch: this.epoch };
  }

  /** Answer the oldest outstanding call, from the store as it is now unless `page` says
   * otherwise. */
  async reply(page?: Partial<FeedPage<T>>): Promise<void> {
    const call = this.calls.shift();
    if (!call) {
      throw new Error("no fetch in flight");
    }
    call.resolve({ ...this.page(call.before, call.limit), ...page });
    await settle();
  }

  async fail(): Promise<void> {
    const call = this.calls.shift();
    if (!call) {
      throw new Error("no fetch in flight");
    }
    call.reject(new Error("host refused"));
    await settle();
  }
}

export interface Harness<T> {
  v: FeedVirtualizer<T>;
  el: Scroller;
  host: FakeHost<T>;
  stop: () => void;
}

export type TestConfig<T> = Omit<FeedConfig<T>, "fetch">;

/** A numeric feed: items are numbers, keyed by value, in ascending order. */
export const NUMBERS: TestConfig<number> = {
  estimate: 20,
  topHeight: 10,
  key: (n) => String(n),
  compare: (a, b) => a - b,
  timeOf: (n) => n,
  base: 5,
  page: 5,
  highWater: 20,
  hardMax: 40,
};

export function harness<T>(config: TestConfig<T>, attach = true): Harness<T> {
  const el = new Scroller();
  const host = new FakeHost<T>(config.compare);
  let v!: FeedVirtualizer<T>;
  const stop = root(() => {
    v = new FeedVirtualizer<T>({ ...config, fetch: host.fetch });
    pre(() => {
      el.domTotal = v.layout.total;
    });
  });
  if (attach) {
    v.scroll(el as unknown as HTMLDivElement);
  }
  flush();
  return { v, el, host, stop };
}

/** The first item row intersecting the viewport and the viewport top's offset into it --
 * negative while the top row is still above it. */
export function underEye<T>(h: Harness<T>): { item: T; offset: number } {
  const { tops, total } = h.v.layout;
  const rows = h.v.display;
  const top = h.el.scrollTop;
  for (let i = 0; i < rows.length; i++) {
    const row = rows[i];
    const end = i + 1 < rows.length ? tops[i + 1] : total;
    if (row.kind === "item" && end > top) {
      return { item: row.item, offset: top - tops[i] };
    }
  }
  throw new Error("viewport past the end of the feed");
}

export function items<T>(h: Harness<T>): T[] {
  return h.v.rows.map((r) => r.item);
}

export function measure<T>(h: Harness<T>, item: T, height: number): void {
  const row = h.v.rows.find((r) => r.item === item);
  if (!row) {
    throw new Error("no such row");
  }
  h.v.measureRow({ offsetHeight: height } as HTMLElement, row.clientKey);
  flush();
}

/** 1..n */
export function range(from: number, to: number, step = 1): number[] {
  const out: number[] = [];
  for (let i = from; i <= to; i += step) {
    out.push(i);
  }
  return out;
}
