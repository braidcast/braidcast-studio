import { afterEach, beforeEach, describe, expect, test } from "bun:test";
import { flushSync as flush } from "svelte";
import { FeedVirtualizer, type FeedRow } from "$lib/utils/feedVirtualizer.svelte";
import { pre, root } from "./harness.svelte";

// rAF and ResizeObserver are browser globals the engine schedules through; the tests run
// frames by hand so a trim lands exactly when asserted.
let frames: FrameRequestCallback[] = [];
const g = globalThis as Record<string, unknown>;
const saved = {
  raf: g.requestAnimationFrame,
  caf: g.cancelAnimationFrame,
  ro: g.ResizeObserver,
};

beforeEach(() => {
  frames = [];
  g.requestAnimationFrame = (cb: FrameRequestCallback) => frames.push(cb);
  g.cancelAnimationFrame = () => {};
  g.ResizeObserver = class {
    observe(): void {}
    disconnect(): void {}
  };
});

afterEach(() => {
  g.requestAnimationFrame = saved.raf;
  g.cancelAnimationFrame = saved.caf;
  g.ResizeObserver = saved.ro;
});

function frame(): void {
  const run = frames;
  frames = [];
  for (const cb of run) {
    cb(0);
  }
  flush();
}

// A scroll container whose content height is the sizer's height IN THE DOM -- updated in
// the render phase, like the component's template, not the moment the layout derives it.
// So a scrollTop past the new maximum clamps exactly as Chromium clamps it once the sizer
// shrinks.
class Scroller {
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
    for (const fn of this.#listeners) {
      fn();
    }
  }
}

interface Harness<T> {
  v: FeedVirtualizer<T>;
  el: Scroller;
  stop: () => void;
}

function harness<T>(config: ConstructorParameters<typeof FeedVirtualizer<T>>[0]): Harness<T> {
  const el = new Scroller();
  let v!: FeedVirtualizer<T>;
  const stop = root(() => {
    v = new FeedVirtualizer<T>(config);
    pre(() => {
      el.domTotal = v.layout.total;
    });
  });
  v.scroll(el as unknown as HTMLDivElement);
  flush();
  return { v, el, stop };
}

/** The row at the top of the viewport and how far into it the viewport starts. */
function underEye<T>(h: Harness<T>): { item: T; offset: number } {
  const { tops, total } = h.v.layout;
  const rows: FeedRow<T>[] = h.v.rows;
  const top = h.el.scrollTop;
  for (let i = 0; i < rows.length; i++) {
    const end = i + 1 < rows.length ? tops[i + 1] : total;
    if (end > top) {
      return { item: rows[i].item, offset: top - tops[i] };
    }
  }
  throw new Error("viewport past the end of the feed");
}

function fill(h: Harness<number>, from: number, to: number): void {
  for (let i = from; i <= to; i++) {
    h.v.enqueue(i);
  }
  frame();
}

function measure<T>(h: Harness<T>, item: T, height: number): void {
  const row = h.v.rows.find((r) => r.item === item);
  if (!row) {
    throw new Error("no such row");
  }
  h.v.measureRow({ offsetHeight: height } as HTMLElement, row.clientKey);
  flush();
}

describe("FeedVirtualizer trim compensation", () => {
  test("a trim above the viewport leaves the row under the eye where it was", () => {
    const h = harness<number>({ max: 10, estimate: 20 });
    fill(h, 1, 10);
    expect(h.v.autoStick).toBe(true);
    h.el.userScroll(70);
    expect(h.v.autoStick).toBe(false);
    expect(underEye(h)).toEqual({ item: 4, offset: 10 });

    fill(h, 11, 12); // trims 1 and 2
    expect(h.v.rows.map((r) => r.item)).toEqual([3, 4, 5, 6, 7, 8, 9, 10, 11, 12]);
    expect(underEye(h)).toEqual({ item: 4, offset: 10 });
    expect(h.v.autoStick).toBe(false);
    h.stop();
  });

  test("compensates before the shrunken sizer can clamp a position near the bottom", () => {
    const h = harness<number>({ max: 10, estimate: 20, stickPx: 0 });
    fill(h, 1, 10);
    measure(h, 1, 100);
    measure(h, 2, 100); // total 360
    h.el.userScroll(240);
    expect(h.v.autoStick).toBe(false);
    expect(underEye(h)).toEqual({ item: 5, offset: 0 });

    // Trimming the two tall rows drops the sizer to 200: 240 is past the new maximum, so
    // a correction made only after the DOM update would start from a clamped 100.
    fill(h, 11, 12);
    expect(underEye(h)).toEqual({ item: 5, offset: 0 });
    h.stop();
  });

  test("a row above the viewport growing is absorbed, even past the old maximum", () => {
    const h = harness<number>({ max: 10, estimate: 20, stickPx: 0 });
    fill(h, 1, 10);
    h.el.userScroll(90);
    expect(underEye(h)).toEqual({ item: 5, offset: 10 });

    measure(h, 1, 60); // +40: the anchor now needs 130, past the old sizer's max of 100
    expect(h.el.scrollTop).toBe(130);
    expect(underEye(h)).toEqual({ item: 5, offset: 10 });
    h.stop();
  });

  test("a row above shrinking is absorbed too", () => {
    const h = harness<number>({ max: 10, estimate: 20 });
    fill(h, 1, 10);
    measure(h, 2, 50);
    h.el.userScroll(80);
    expect(underEye(h)).toEqual({ item: 3, offset: 10 });

    measure(h, 2, 20);
    expect(underEye(h)).toEqual({ item: 3, offset: 10 });
    h.stop();
  });

  test("a scroll not yet reported is honored, not snapped back to the last anchor", () => {
    const h = harness<number>({ max: 10, estimate: 20 });
    fill(h, 1, 10);
    h.el.userScroll(70);
    h.el.scrollTop = 40; // the reader moved; the scroll event is still queued
    fill(h, 11, 12);
    expect(underEye(h)).toEqual({ item: 3, offset: 0 });
    h.stop();
  });

  test("pinned to the bottom, new rows keep it pinned", () => {
    const h = harness<number>({ max: 10, estimate: 20 });
    fill(h, 1, 10);
    fill(h, 11, 15);
    expect(h.v.autoStick).toBe(true);
    expect(h.el.scrollTop).toBe(h.el.scrollHeight - h.el.clientHeight);
    h.stop();
  });
});

describe("FeedVirtualizer setFeed", () => {
  type Item = { id: string };
  const items = (...ids: string[]): Item[] => ids.map((id) => ({ id }));
  const ten = (): Item[] => items("1", "2", "3", "4", "5", "6", "7", "8", "9", "10");

  test("scrolled up, a keyed replace keeps the reader on the same row and unstuck", () => {
    const h = harness<Item>({ max: 20, estimate: 20, key: (i) => i.id });
    h.v.setFeed(ten());
    flush();
    h.el.userScroll(70);
    expect(underEye(h)).toEqual({ item: { id: "4" }, offset: 10 });
    const key4 = h.v.rows[3].clientKey;

    // Two older rows arrive ahead of everything (a backfill of the full store).
    h.v.setFeed(items("a", "b").concat(ten()));
    flush();
    expect(h.v.autoStick).toBe(false);
    expect(underEye(h).item.id).toBe("4");
    expect(underEye(h).offset).toBe(10);
    expect(h.v.rows[5].clientKey).toBe(key4);
    h.stop();
  });

  test("a surviving row keeps its measured height across a replace", () => {
    const h = harness<Item>({ max: 20, estimate: 20, key: (i) => i.id });
    const list = ten();
    h.v.setFeed(list);
    flush();
    measure(h, list[0], 55);
    h.v.setFeed(items("x").concat(ten()));
    flush();
    expect(h.v.layout.tops[2] - h.v.layout.tops[1]).toBe(55);
    h.stop();
  });

  test("scrolled up, an unkeyed replace does not force the view back to the bottom", () => {
    const h = harness<number>({ max: 20, estimate: 20 });
    h.v.setFeed([1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
    flush();
    h.el.userScroll(40);
    h.v.setFeed([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]);
    flush();
    expect(h.v.autoStick).toBe(false);
    expect(h.el.scrollTop).toBeLessThan(h.el.scrollHeight - h.el.clientHeight);
    h.stop();
  });

  test("at the bottom, a replace stays pinned to the newest row", () => {
    const h = harness<Item>({ max: 20, estimate: 20, key: (i) => i.id });
    h.v.setFeed(ten());
    flush();
    h.v.setFeed(ten().concat(items("11", "12")));
    flush();
    expect(h.v.autoStick).toBe(true);
    expect(h.el.scrollTop).toBe(h.el.scrollHeight - h.el.clientHeight);
    h.stop();
  });

  test("an empty replace leaves nothing to be scrolled up in", () => {
    const h = harness<number>({ max: 20, estimate: 20 });
    h.v.setFeed([1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
    flush();
    h.el.userScroll(0);
    expect(h.v.autoStick).toBe(false);
    h.v.setFeed([]);
    flush();
    expect(h.v.autoStick).toBe(true);
    h.stop();
  });

  test("the cap applies to a replace, keeping the newest rows in either source order", () => {
    const h = harness<number>({ max: 3, estimate: 20 });
    h.v.setFeed([1, 2, 3, 4, 5]);
    expect(h.v.rows.map((r) => r.item)).toEqual([3, 4, 5]);
    h.v.setFeed([5, 4, 3, 2, 1], true);
    expect(h.v.rows.map((r) => r.item)).toEqual([3, 4, 5]);
    h.stop();
  });

  test("rows are replaced wholesale, never mutated in place", () => {
    const h = harness<number>({ max: 5, estimate: 20 });
    fill(h, 1, 3);
    const before = h.v.rows;
    fill(h, 4, 4);
    expect(h.v.rows).not.toBe(before);
    expect(before.map((r) => r.item)).toEqual([1, 2, 3]);
    h.stop();
  });
});

describe("FeedVirtualizer construction", () => {
  // A dock constructs its feed first and declares the display set after it, because the
  // set derives from feed.rows. So nothing may call getDisplay while the constructor runs.
  test("does not read the display set before the consumer has declared it", () => {
    const el = new Scroller();
    let feed!: FeedVirtualizer<number>;
    const stop = root(() => {
      feed = new FeedVirtualizer<number>({ max: 10, estimate: 20, getDisplay: () => shown() });
      const shown = (): FeedRow<number>[] => feed.rows.filter((r) => r.item % 2 === 1);
      pre(() => {
        el.domTotal = feed.layout.total;
      });
    });
    feed.scroll(el as unknown as HTMLDivElement);
    flush();
    feed.setFeed([1, 2, 3, 4, 5]);
    flush();
    expect(feed.visible.map((r) => r.item)).toEqual([1, 3, 5]);
    expect(el.scrollTop).toBe(0);
    stop();
  });
});
