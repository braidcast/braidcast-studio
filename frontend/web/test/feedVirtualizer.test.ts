import { describe, expect, test } from "bun:test";
import { flushSync as flush } from "svelte";
import { FeedVirtualizer } from "$lib/utils/feedVirtualizer.svelte";
import { FakeHost, NUMBERS, Scroller, frame, harness, items, measure, range, settle, underEye, useFrameMocks } from "./feedHarness";
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

  test("pinned, the box shrinking before the pin's scroll event lands keeps it pinned", async () => {
    const h = await loaded(range(1, 15));
    // A bar mounts above the feed after the pin, before its scroll event: no observer has
    // seen the box shrink yet, and the view now sits 40px short of the bottom.
    h.el.clientHeight = 60;
    h.el.scrollEvent();
    expect(h.v.autoStick).toBe(true);
    expect(h.el.scrollTop).toBe(h.el.scrollHeight - h.el.clientHeight);
    h.stop();
  });

  test("pinned, content growing before the pin's scroll event lands keeps it pinned", async () => {
    const h = await loaded(range(1, 15));
    // The newest row outgrew its measure and overflows the sizer by 60px.
    h.el.domTotal += 60;
    h.el.scrollEvent();
    expect(h.v.autoStick).toBe(true);
    expect(h.el.scrollTop).toBe(h.el.scrollHeight - h.el.clientHeight);
    h.stop();
  });

  test("pinned, only the reader scrolling up past stickPx lets go", async () => {
    const h = await loaded(range(1, 15));
    const bottom = h.el.scrollHeight - h.el.clientHeight;
    h.el.userScroll(bottom - 10);
    expect(h.v.autoStick).toBe(true);
    h.el.userScroll(bottom - 30);
    expect(h.v.autoStick).toBe(false);
    h.el.userScroll(bottom);
    expect(h.v.autoStick).toBe(true);
    // Up while the content grows under it is still the reader's.
    h.el.domTotal += 60;
    h.el.userScroll(bottom - 5);
    expect(h.v.autoStick).toBe(false);
    h.stop();
  });

  test("pinned, a nudge within stickPx moves where up is measured from", async () => {
    const h = await loaded(range(1, 15));
    const bottom = h.el.scrollHeight - h.el.clientHeight;
    h.el.userScroll(bottom - 10);
    expect(h.v.autoStick).toBe(true);
    // Growth then lands an event with the view where the reader left it: not a move up.
    h.el.domTotal += 60;
    h.el.scrollEvent();
    expect(h.v.autoStick).toBe(true);
    expect(h.el.scrollTop).toBe(h.el.scrollHeight - h.el.clientHeight);
    h.stop();
  });

  test("pinned, scrollTop reading back a fraction under the pin is not a move up", async () => {
    const h = await loaded(range(1, 15));
    // Chromium snaps scrollTop to device pixels, so the offset it reports can sit a
    // fraction under the one the pin read back.
    h.el.scrollTop -= 0.25;
    h.el.domTotal += 60;
    h.el.scrollEvent();
    expect(h.v.autoStick).toBe(true);
    expect(h.el.scrollTop).toBe(h.el.scrollHeight - h.el.clientHeight);
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

  test("rows measured in the flush that pins still land it on the real bottom", async () => {
    // As in a component: the rows' measure actions run ahead of the feed's pin effect, and
    // the sizer they grow is redrawn in a follow-up batch, so the pin first meets the old
    // height. 21px rows against a 20px estimate leave it 15px short: inside stickPx, so the
    // pin's scroll event reads it as at the bottom and never pins it again.
    const el = new Scroller();
    const host = new FakeHost<number>((a, b) => a - b);
    let feed!: FeedVirtualizer<number>;
    const stop = root(() => {
      feed = new FeedVirtualizer<number>({ ...STILL, fetch: host.fetch });
      pre(() => {
        el.domTotal = feed.layout.total;
      });
      pre(() => {
        for (const row of feed.display) {
          if (row.kind === "item") {
            feed.measureRow({ offsetHeight: 21 } as HTMLElement, row.clientKey);
          }
        }
      });
    });
    feed.scroll(el as unknown as HTMLDivElement);
    flush();
    host.store = range(1, 15);
    feed.load();
    await host.reply();
    await settle();
    expect(feed.autoStick).toBe(true);
    expect(el.scrollHeight).toBe(325);
    expect(el.scrollTop).toBe(225);
    stop();
  });
});
