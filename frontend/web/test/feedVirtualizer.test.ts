import { describe, expect, test } from "bun:test";
import { flushSync as flush } from "svelte";
import { FeedVirtualizer } from "$lib/utils/feedVirtualizer.svelte";
import { FakeHost, NUMBERS, Scroller, frame, harness, items, measure, range, underEye, useFrameMocks } from "./feedHarness";
import { pre, root } from "./harness.svelte";

useFrameMocks();

// No older paging in these: they are about measurement and anchoring within one window.
const STILL = { ...NUMBERS, prefetchScreens: 0 };

async function loaded(store: number[], config = STILL) {
  const h = harness<number>(config);
  h.host.store = store;
  h.v.load();
  await h.host.reply();
  return h;
}

describe("FeedVirtualizer anchoring", () => {
  test("a row above the viewport growing is absorbed, even past the old maximum", async () => {
    const h = await loaded(range(1, 15), { ...STILL, stickPx: 0 });
    h.el.userScroll(90);
    expect(underEye(h)).toEqual({ item: 5, offset: 0 });

    measure(h, 1, 60); // +40: the anchor now needs 130
    expect(h.el.scrollTop).toBe(130);
    expect(underEye(h)).toEqual({ item: 5, offset: 0 });
    h.stop();
  });

  test("a row above shrinking is absorbed too", async () => {
    const h = await loaded(range(1, 15));
    measure(h, 2, 50);
    h.el.userScroll(90);
    expect(underEye(h)).toEqual({ item: 3, offset: 10 });

    measure(h, 2, 20);
    expect(underEye(h)).toEqual({ item: 3, offset: 10 });
    h.stop();
  });

  test("a scroll not yet reported is honored, not snapped back to the last anchor", async () => {
    const h = await loaded(range(1, 15));
    h.el.userScroll(70);
    h.el.scrollTop = 40; // the reader moved; the scroll event is still queued
    h.v.live(16);
    frame();
    expect(underEye(h)).toEqual({ item: 2, offset: 10 });
    h.stop();
  });

  test("pinned to the bottom, new rows keep it pinned", async () => {
    const h = await loaded(range(1, 15));
    for (const n of range(16, 20)) {
      h.v.live(n);
    }
    frame();
    expect(h.v.autoStick).toBe(true);
    expect(h.el.scrollTop).toBe(h.el.scrollHeight - h.el.clientHeight);
    h.stop();
  });

  test("pinned to the bottom, the box shrinking keeps the newest row in view", async () => {
    // A ResizeObserver that hands its callback back, so the box can be resized by hand.
    const resized: (() => void)[] = [];
    (globalThis as Record<string, unknown>).ResizeObserver = class {
      constructor(cb: () => void) {
        resized.push(cb);
      }
      observe(): void {}
      disconnect(): void {}
    };
    const h = await loaded(range(1, 15));
    expect(h.v.autoStick).toBe(true);
    // A strip mounting under the feed (a pinned poll or goal) takes 30px from it.
    h.el.clientHeight = 70;
    for (const cb of resized) {
      cb();
    }
    flush();
    expect(h.el.scrollTop).toBe(h.el.scrollHeight - h.el.clientHeight);
    // Unpinned, a resize leaves the reader where they are.
    h.el.userScroll(40);
    expect(h.v.autoStick).toBe(false);
    h.el.clientHeight = 100;
    for (const cb of resized) {
      cb();
    }
    flush();
    expect(h.el.scrollTop).toBe(40);
    h.stop();
  });

  test("a reload keeps a surviving row's measured height and the reader's place", async () => {
    const h = await loaded(range(1, 15));
    measure(h, 12, 55);
    const key12 = h.v.rows.find((r) => r.item === 12)!.clientKey;
    h.host.store = range(1, 16);
    h.v.load();
    await h.host.reply();
    const i = h.v.display.findIndex((r) => r.kind === "item" && r.item === 12);
    expect(h.v.display[i].clientKey).toBe(key12);
    expect(h.v.layout.tops[i + 1] - h.v.layout.tops[i]).toBe(55);
    h.stop();
  });

  test("rows are replaced wholesale, never mutated in place", async () => {
    const h = await loaded(range(1, 3));
    const before = h.v.rows;
    h.v.live(4);
    frame();
    expect(h.v.rows).not.toBe(before);
    expect(before.map((r) => r.item)).toEqual([1, 2, 3]);
    expect(items(h)).toEqual([1, 2, 3, 4]);
    h.stop();
  });
});

describe("FeedVirtualizer construction", () => {
  // The docks construct their feed before the state its fetch closes over, so nothing may
  // run against the display set until the scroll container attaches.
  test("stays inert until the container attaches, then pins to the newest row", async () => {
    const el = new Scroller();
    const host = new FakeHost<number>((a, b) => a - b);
    let feed!: FeedVirtualizer<number>;
    const stop = root(() => {
      feed = new FeedVirtualizer<number>({ ...STILL, fetch: host.fetch });
      pre(() => {
        el.domTotal = feed.layout.total;
      });
    });
    flush();
    expect(host.calls.length).toBe(0);
    host.store = range(1, 15);
    feed.load();
    await host.reply();
    expect(el.scrollTop).toBe(0);
    feed.scroll(el as unknown as HTMLDivElement);
    flush();
    expect(el.scrollTop).toBe(el.scrollHeight - el.clientHeight);
    stop();
  });
});
