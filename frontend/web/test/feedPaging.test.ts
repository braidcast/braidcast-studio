import { describe, expect, mock, test } from "bun:test";
import { flushSync as flush } from "svelte";
import type { FeedFilter } from "$lib/api/bridge";
import { compareCodePoints } from "$lib/docks/events/eventOrder";
import { chatKey, spansDestinations } from "$lib/docks/multichat/chatIntake";
import type { DestinationIdentity } from "$lib/stores/destinationIdentityStore.svelte";
import type { DestinationSelection } from "$lib/ui/destinationSelection";
import { fmtStreamStarted } from "$lib/utils/format";
import type { Boundary } from "$lib/utils/feedVirtualizer.svelte";
import {
  NUMBERS,
  frame,
  harness,
  items,
  measure,
  range,
  timer,
  underEye,
  useFrameMocks,
  type Harness,
} from "./feedHarness";
import { pre, root } from "./harness.svelte";

// The bridge module wires window globals on import, and destinationSelection reaches it
// through the stores; the predicates under test never call it.
mock.module("$lib/api/bridge", () => ({ obs: { call: async () => undefined, on: () => () => {} } }));
const { filterOf, matchesSelection } = await import("$lib/ui/destinationSelection");

useFrameMocks();

// Where an item's row sits in the viewport, in px from its top edge.
function screenTop<T>(h: Harness<T>, item: T): number {
  const i = h.v.display.findIndex((r) => r.kind === "item" && r.item === item);
  return h.v.layout.tops[i] - h.el.scrollTop;
}

// NUMBERS: viewport 100px, rows 20px, top row 10px. A load asks for three screens (15
// rows), an older page for 5, a window stuck to the bottom trims past 60 rows, and one the
// reader is scrolled up in holds at most 40, letting its newest end go past that.

describe("paged window: loading", () => {
  test("load fills three screens by chaining pages, stops on more:false, and shows the start", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = range(1, 12);
    h.host.maxLimit = 5; // a host that serves short pages
    h.v.load();
    expect(h.v.fetching).toBe(true);
    await h.host.reply(); // 8..12
    expect(h.host.calls.length).toBe(1); // under 1.5 screens from the top: the next page
    await h.host.reply(); // 3..7
    await h.host.reply(); // 1..2, more:false
    expect(items(h)).toEqual(range(1, 12));
    expect(h.host.calls.length).toBe(0);
    expect(h.v.more).toBe(false);
    expect(h.v.top).toBe("start");
    h.stop();
  });

  test("scrolling within 1.5 screens of the top fetches once, however often it scrolls", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = range(1, 100);
    h.v.load();
    await h.host.reply(); // 86..100: three screens, nothing more asked for
    expect(h.host.calls.length).toBe(0);
    h.el.userScroll(160);
    expect(h.host.calls.length).toBe(0);
    h.el.userScroll(100);
    expect(h.host.calls.length).toBe(1);
    expect(h.host.calls[0].before).toBe(86);
    h.el.userScroll(90);
    h.el.userScroll(20);
    expect(h.host.calls.length).toBe(1);
    h.stop();
  });

  test("a prepended page keeps the row under the reader's eye, even at the very top", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = range(1, 100);
    h.v.load();
    await h.host.reply();
    h.el.userScroll(100);
    const eye = underEye(h);
    await h.host.reply(); // 81..85 above
    expect(items(h)[0]).toBe(81);
    expect(underEye(h)).toEqual(eye);

    // At scrollTop 0 the top row is what the viewport starts on. It is pinned at index 0,
    // so it cannot be the anchor: the first item keeps its place below it instead, and the
    // new page opens up above it.
    h.el.userScroll(0);
    expect(underEye(h)).toEqual({ item: 81, offset: -10 });
    await h.host.reply(); // 76..80
    expect(items(h)[0]).toBe(76);
    expect(h.el.scrollTop).toBeGreaterThan(0);
    expect(screenTop(h, 81)).toBe(10);
    h.stop();
  });

  test("a failed load still shows what arrived while it was in flight", async () => {
    const h = harness<number>(NUMBERS);
    h.v.load();
    h.v.live(7);
    frame();
    expect(items(h)).toEqual([]);
    await h.host.fail();
    frame();
    expect(items(h)).toEqual([7]);
    h.stop();
  });

  test("a failed older page is asked for again after a pause, never straight away", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = range(1, 100);
    h.v.load();
    await h.host.reply(); // 86..100
    h.el.userScroll(100);
    expect(h.host.calls.length).toBe(1);
    await h.host.fail(); // the host could not read its history yet
    expect(h.v.top).toBe("idle"); // older rows are still there: not the start
    h.el.userScroll(90);
    frame();
    expect(h.host.calls.length).toBe(0); // nothing asked until the pause is over
    expect(timer()).toBe(true);
    expect(h.host.calls.length).toBe(1);
    expect(h.host.calls[0].before).toBe(86);
    await h.host.reply();
    expect(items(h)[0]).toBe(81);
    h.stop();
  });
});

describe("paged window: filter switches", () => {
  test("a frame still queued under the old filter does not join the new window", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = range(1, 10);
    h.v.load();
    await h.host.reply();
    h.v.live(999); // passed the old filter; its frame has not run yet
    h.host.store = [2, 4, 6];
    h.v.load();
    frame();
    await h.host.reply();
    expect(items(h)).toEqual([2, 4, 6]);
    h.stop();
  });

  test("a failed load drops the old filter's rows, keeps what arrived for the new one, and retries", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = range(1, 10);
    h.v.load();
    await h.host.reply();
    h.host.store = [2, 4, 6];
    h.v.load();
    h.v.live(8); // passed the new filter while the load was in flight
    frame();
    await h.host.fail();
    expect(items(h)).toEqual([8]);
    expect(h.v.top).toBe("retrying");
    expect(h.host.calls.length).toBe(0); // no older page off a window that failed to load

    expect(timer()).toBe(true);
    expect(h.host.calls.length).toBe(1);
    expect(h.host.calls[0].before).toBeUndefined();
    await h.host.fail();
    expect(timer()).toBe(true); // it keeps asking
    h.host.store = [2, 4, 6, 8];
    await h.host.reply();
    expect(items(h)).toEqual([2, 4, 6, 8]);
    expect(h.v.top).not.toBe("retrying");
    expect(timer()).toBe(false);
    h.stop();
  });

  test("a retry that fails again keeps the rows that already match its filter", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = range(1, 10);
    h.v.load();
    await h.host.reply();
    h.host.store = [2, 4, 6];
    h.v.load();
    await h.host.fail(); // the old filter's rows go
    expect(items(h)).toEqual([]);
    h.v.live(50);
    h.v.live(51);
    frame();
    expect(items(h)).toEqual([50, 51]);

    expect(timer()).toBe(true);
    await h.host.fail(); // the retry, under the filter 50 and 51 already match
    expect(items(h)).toEqual([50, 51]);
    expect(h.v.top).toBe("retrying");
    expect(timer()).toBe(true);
    h.host.store = [2, 4, 6, 50, 51];
    await h.host.reply();
    expect(items(h)).toEqual([2, 4, 6, 50, 51]);
    h.stop();
  });

  test("a load called from an effect does not make the effect depend on the feed", async () => {
    const h = harness<number>({ ...NUMBERS, prefetchScreens: 0 });
    h.host.store = range(1, 100);
    let runs = 0;
    const stop = root(() =>
      pre(() => {
        runs++;
        h.v.load();
      }),
    );
    flush();
    expect(runs).toBe(1);
    await h.host.reply();
    h.el.clientHeight = 200; // viewH, which the load's size is read from
    h.el.userScroll(40); // viewTop and autoStick
    flush();
    expect(runs).toBe(1);
    stop();
    h.stop();
  });

  test("a load that keeps none of the rows lands on the newest, even after the reader scrolled", async () => {
    const h = harness<number>({ ...NUMBERS, prefetchScreens: 0 });
    h.host.store = range(1, 100);
    h.v.load();
    await h.host.reply();
    h.host.store = range(1001, 1100);
    h.v.load();
    h.el.userScroll(40); // while the new filter's page is in flight
    expect(h.v.autoStick).toBe(false);
    await h.host.reply();
    expect(h.v.autoStick).toBe(true);
    expect(h.el.scrollTop).toBe(h.el.scrollHeight - h.el.clientHeight);
    h.stop();
  });

  test("the top row spins only for an older page, never for a load", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = range(1, 100);
    h.v.load();
    expect(h.v.top).not.toBe("loading");
    await h.host.reply();
    h.v.load(); // a filter switch: the old window stays until the page lands
    expect(h.v.top).toBe("idle");
    await h.host.reply();
    h.el.userScroll(100);
    expect(h.v.top).toBe("loading");
    await h.host.reply();
    expect(h.v.top).toBe("idle");
    h.stop();
  });
});

describe("paged window: the page / live seam", () => {
  test("a page that lands after a live frame of the same row shows it once", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = range(1, 20);
    h.v.load();
    h.host.store = range(1, 21); // stored while the page is in flight, and read by it
    h.v.live(21);
    frame();
    await h.host.reply();
    expect(items(h)).toEqual(range(7, 21));
    h.stop();
  });

  test("a live frame that lands after a page containing it shows it once", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = range(1, 20);
    h.v.load();
    await h.host.reply();
    h.v.live(20);
    frame();
    expect(items(h)).toEqual(range(6, 20));
    h.stop();
  });

  test("a frame that lands during the load but sorts before its page waits for a later page", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = range(10, 100, 10);
    h.v.load();
    h.v.live(5);
    frame();
    await h.host.reply({ items: [90, 100], more: true });
    expect(items(h)).toEqual([90, 100]); // the host has 5; a later page brings it
    h.stop();
  });

  test("with nothing older on the host, a frame sorting before the page is kept", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = [90, 100];
    h.v.load();
    h.v.live(5);
    frame();
    await h.host.reply();
    expect(items(h)).toEqual([5, 90, 100]);
    h.stop();
  });
});

describe("paged window: old rows while a page is in flight", () => {
  async function scrolledToTop() {
    const h = harness<number>(NUMBERS);
    h.host.store = range(10, 300, 10); // 30 rows
    h.v.load();
    await h.host.reply(); // 160..300
    h.el.userScroll(100); // older page in flight: before 160
    expect(h.host.calls.length).toBe(1);
    return h;
  }

  test("held until the page lands, then merged if in range and dropped if not", async () => {
    const h = await scrolledToTop();
    // A backfill stored these after the page was queried, so the page does not carry them.
    h.v.merge([155, 5]);
    frame();
    expect(items(h)).not.toContain(155);
    await h.host.reply(); // 110..150, more:true
    expect(items(h).slice(0, 7)).toEqual([110, 120, 130, 140, 150, 155, 160]);
    expect(items(h)).not.toContain(5);
    h.stop();
  });

  test("when the page reaches the start of history, even the oldest is kept", async () => {
    const h = await scrolledToTop();
    h.v.merge([5]);
    frame();
    await h.host.reply({ items: [110, 120, 130, 140, 150], more: false });
    expect(items(h)[0]).toBe(5);
    expect(h.v.top).toBe("start");
    h.stop();
  });

  test("with no page in flight and more on the host, an old row is left for paging", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = range(10, 300, 10);
    h.v.load();
    await h.host.reply();
    h.v.merge([5]);
    frame();
    expect(items(h)).not.toContain(5);
    h.stop();
  });
});

describe("paged window: scrolled up", () => {
  test("a backfill merge keeps the rows above anchored and counts the rows below as unseen", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = range(10, 1000, 10);
    h.v.load();
    await h.host.reply(); // 860..1000
    h.el.userScroll(160); // no older page: 1.6 screens from the top
    expect(h.host.calls.length).toBe(0);
    const eye = underEye(h);
    expect(eye).toEqual({ item: 930, offset: 10 });
    const key930 = h.v.rows.find((r) => r.item === 930)!.clientKey;

    h.v.merge([875, 985, 1010]); // one above the viewport, one below it, one newest
    frame();
    expect(underEye(h)).toEqual(eye);
    expect(h.v.rows.find((r) => r.item === 930)!.clientKey).toBe(key930);
    expect(items(h)).toContain(875);
    expect(h.v.unseen).toBe(2);
    expect(h.v.autoStick).toBe(false);

    h.el.userScroll(h.el.scrollHeight);
    expect(h.v.autoStick).toBe(true);
    expect(h.v.unseen).toBe(0);
    h.stop();
  });

  test("past hardMax the window detaches; jumping to latest reloads it", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = range(1, 15);
    h.v.load();
    await h.host.reply();
    h.el.userScroll(160);
    for (const n of range(16, 45)) {
      h.v.live(n);
    }
    frame(); // 45 rows > 40
    expect(h.v.detached).toBe(true);
    const held = h.v.rows.length;
    h.v.live(46);
    frame();
    expect(h.v.rows.length).toBe(held);
    expect(h.v.unseen).toBe(31);
    h.v.merge([5.5]); // a backfilled row above the viewport: not one the reader missed
    frame();
    expect(h.v.unseen).toBe(31);

    h.host.store = range(1, 46);
    h.v.jumpToLatest();
    expect(h.host.calls.length).toBe(1);
    expect(h.host.calls[0].before).toBeUndefined();
    await h.host.reply();
    expect(h.v.detached).toBe(false);
    expect(h.v.autoStick).toBe(true);
    expect(items(h)).toEqual(range(32, 46));
    h.stop();
  });

  test("a failed reload of a detached window keeps its rows, its chip and its unseen count", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = range(1, 15);
    h.v.load();
    await h.host.reply();
    h.el.userScroll(160);
    for (const n of range(16, 45)) {
      h.v.live(n);
    }
    frame();
    expect(h.v.detached).toBe(true);
    const held = items(h);
    const unseen = h.v.unseen;

    h.v.jumpToLatest();
    expect(h.v.autoStick).toBe(false); // still detached until the page lands
    await h.host.fail();
    expect(items(h)).toEqual(held);
    expect(h.v.detached).toBe(true);
    expect(h.v.autoStick).toBe(false);
    expect(h.v.unseen).toBe(unseen);

    h.el.userScroll(100);
    h.v.live(500);
    frame();
    expect(h.v.autoStick).toBe(false);
    expect(h.v.unseen).toBe(unseen + 1); // not shown, but counted
    expect(items(h)).not.toContain(500);

    h.host.store = [...range(1, 45), 500];
    expect(timer()).toBe(true);
    await h.host.reply();
    expect(h.v.detached).toBe(false);
    expect(items(h)).toContain(500);
    h.stop();
  });
});

describe("paged window: detached", () => {
  // Scrolled up with 55 rows arrived: the window keeps its oldest 40 and lets 226..240 go.
  async function detachedAt160(): Promise<Harness<number>> {
    const h = harness<number>(NUMBERS);
    h.host.store = range(1, 240);
    h.host.store.length = 200; // the host has 1..200 when the dock opens
    h.v.load();
    await h.host.reply(); // 186..200
    h.el.userScroll(160); // 1.6 screens down: no older page yet
    h.host.store = range(1, 240);
    for (const n of range(201, 240)) {
      h.v.live(n);
    }
    frame();
    return h;
  }

  test("the newest end goes past hardMax, and the rows the reader sees keep their place", async () => {
    const h = await detachedAt160();
    expect(h.v.detached).toBe(true);
    expect(items(h)).toEqual(range(186, 225));
    expect(underEye(h)).toEqual({ item: 193, offset: 10 });
    expect(h.v.unseen).toBe(40); // every live row arrived below the viewport
    h.stop();
  });

  test("older pages still load, all the way to the start, and the window never passes hardMax", async () => {
    const h = await detachedAt160();
    h.el.userScroll(100); // within 1.5 screens of the top
    let pages = 0;
    while (h.host.calls.length > 0 && pages < 100) {
      const eye = underEye(h);
      const before = h.host.calls[0].before;
      await h.host.reply();
      pages++;
      expect(h.v.rows.length).toBeLessThanOrEqual(NUMBERS.hardMax);
      // The page opened above the reader and the newest end went below: neither moved them.
      expect(underEye(h)).toEqual(eye);
      expect(items(h)[0]).toBeLessThan(before!);
      if (h.host.calls.length === 0 && h.v.more) {
        h.el.userScroll(100);
      }
    }
    expect(items(h)[0]).toBe(1);
    expect(h.v.more).toBe(false);
    expect(h.v.top).toBe("start");
    expect(h.v.detached).toBe(true);
    expect(items(h)).toEqual(range(1, 40));
    h.stop();
  });

  test("live rows past the window only count as unseen; jumping to latest reloads the newest page", async () => {
    const h = await detachedAt160();
    h.el.userScroll(100);
    await h.host.reply(); // 181..185 above; 221..225 let go below
    const unseen = h.v.unseen;
    h.v.live(241);
    h.v.merge([185.5]); // sorts inside the window: it joins, above the reader
    frame();
    expect(items(h)).not.toContain(241);
    expect(items(h)).toContain(185.5);
    expect(h.v.rows.length).toBeLessThanOrEqual(NUMBERS.hardMax);
    expect(h.v.unseen).toBe(unseen + 1);

    h.host.store = [...range(1, 241)];
    while (h.host.calls.length > 0) {
      await h.host.reply(); // any older page the settle asked for
    }
    h.v.jumpToLatest();
    expect(h.host.calls.at(-1)!.before).toBeUndefined();
    await h.host.reply();
    expect(h.v.detached).toBe(false);
    expect(h.v.autoStick).toBe(true);
    expect(h.v.unseen).toBe(0);
    expect(items(h).at(-1)).toBe(241);
    h.stop();
  });
});

describe("paged window: full and scrolled up", () => {
  test("a backfill into a full window never lets go of a row the reader can see", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = range(10, 400, 10);
    h.v.load();
    await h.host.reply(); // 260..400
    for (const n of range(410, 650, 10)) {
      h.v.live(n);
    }
    frame(); // stuck to the bottom: 260..650, exactly hardMax
    expect(h.v.rows.length).toBe(NUMBERS.hardMax);
    h.el.userScroll(h.el.scrollTop - 30); // just past stickPx: the newest rows still show
    expect(h.v.autoStick).toBe(false);
    const shown = items(h).filter((n) => screenTop(h, n) + NUMBERS.estimate > 0 && screenTop(h, n) < 100);
    expect(shown).toEqual(range(590, 640, 10));
    const at = shown.map((n) => screenTop(h, n));

    h.v.merge([265, 275, 285]); // backfilled above the reader, into a window already full
    frame();
    expect(h.v.rows.length).toBe(NUMBERS.hardMax);
    expect(shown.map((n) => screenTop(h, n))).toEqual(at);
    // 650, below the viewport, went first; the rest came off the oldest end.
    expect(items(h)).not.toContain(650);
    expect(items(h).slice(0, 4)).toEqual([270, 275, 280, 285]);
    expect(h.v.detached).toBe(true);
    expect(h.v.more).toBe(true);
    expect(h.v.unseen).toBe(0);
    h.stop();
  });
});

describe("paged window: trims and clears", () => {
  test("a trim at the bottom during an older page discards the page and leaves no gap", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = range(1, 100);
    h.host.maxLimit = 5;
    h.v.load();
    await h.host.reply(); // 96..100; under 1.5 screens, so an older page goes out
    expect(h.host.calls.length).toBe(1);
    expect(h.v.autoStick).toBe(true);
    for (const n of range(101, 160)) {
      h.v.live(n);
    }
    frame(); // 65 rows > 60: trimmed back to 15
    expect(items(h)).toEqual(range(146, 160));
    expect(h.v.more).toBe(true);
    expect(h.v.fetching).toBe(false);

    await h.host.reply(); // the page for "before 96" lands late
    expect(items(h)).toEqual(range(146, 160));
    h.stop();
  });

  test("a trim does not cancel a load: the new filter's rows still arrive", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = range(1, 100);
    h.v.load();
    await h.host.reply();
    h.v.load(); // a filter switch
    for (const n of range(101, 170)) {
      h.v.live(n); // buffered for the load, never trimmed against the old window
    }
    frame();
    h.host.store = range(1, 170);
    await h.host.reply();
    expect(items(h)).toEqual(range(156, 170));
    h.stop();
  });

  test("a clear during a fetch wins, and the page from before it is discarded", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = range(1, 100);
    h.v.load();
    await h.host.reply();
    h.el.userScroll(100); // older page in flight
    expect(h.host.calls.length).toBe(1);
    h.v.reset(1);
    expect(items(h)).toEqual([]);
    expect(h.v.top).toBe("start");
    await h.host.reply({ epoch: 0 });
    expect(items(h)).toEqual([]);

    h.v.live(101);
    frame();
    h.v.reset(1); // the clear call's own reply, after its push: already applied
    expect(items(h)).toEqual([101]);
    h.stop();
  });

  test("a page read after a clear outlives that clear's late push", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = range(1, 10);
    h.v.load();
    await h.host.reply();
    h.v.load();
    h.host.epoch = 1; // the host cleared, then stored 7 and 8, before reading the page
    h.host.store = [7, 8];
    await h.host.reply();
    h.v.reset(1); // the push for that clear lands after the page
    expect(items(h)).toEqual([7, 8]);
    h.v.reset(2);
    expect(items(h)).toEqual([]);
    h.stop();
  });

  test("a load reply from before a clear is discarded", async () => {
    const h = harness<number>(NUMBERS);
    h.host.store = range(1, 20);
    h.v.load();
    h.v.reset(1);
    await h.host.reply({ epoch: 0 });
    expect(items(h)).toEqual([]);
    h.stop();
  });
});

describe("stream dividers", () => {
  // Items are their own time, so a boundary "at 90.5" sits between rows 90 and 91.
  function withBounds(bounds: Boundary[], hardMax = NUMBERS.hardMax) {
    return harness<number>({
      ...NUMBERS,
      hardMax,
      dividerHeight: 8,
      boundaries: () => bounds,
      dividerLabel: (b) => b.id,
    });
  }

  // The window as items and "|id" dividers, top row left out.
  function shape(h: Harness<number>): (number | string)[] {
    return h.v.display.slice(1).map((r) => (r.kind === "item" ? r.item : r.kind === "divider" ? "|" + r.label : "?"));
  }

  function dividerKey(h: Harness<number>, id: string): number {
    const row = h.v.display.find((r) => r.kind === "divider" && r.label === id);
    if (!row) {
      throw new Error("no divider " + id);
    }
    return row.clientKey;
  }

  test("each shows before the first row after it, once rows before it or the start are loaded", async () => {
    const h = withBounds([
      { id: "a", at: 5.5, live: false }, // before the whole store
      { id: "b", at: 50.5, live: false },
      { id: "c", at: 91.2, live: false }, // c and d have nothing between them:
      { id: "d", at: 91.6, live: false }, // only d shows
      { id: "e", at: 150, live: false }, // ended, and nothing after it
    ], 100); // room to page back to the start without detaching
    h.host.store = range(41, 100);
    h.v.load();
    await h.host.reply(); // 86..100, more rows on the host
    expect(shape(h)).toEqual([...range(86, 91), "|d", ...range(92, 100)]);
    const keyD = dividerKey(h, "d");
    expect(keyD).toBeLessThan(-1);

    h.el.userScroll(100);
    await h.host.reply(); // 81..85
    expect(dividerKey(h, "d")).toBe(keyD);
    for (let pages = 0; h.v.more && pages < 20; pages++) {
      h.el.userScroll(0); // on up to 41, where the host runs out
      if (h.host.calls.length > 0) {
        await h.host.reply();
      }
    }
    expect(h.v.more).toBe(false);
    // With nothing older on the host, a divider before the oldest row is placed correctly.
    expect(shape(h).slice(0, 2)).toEqual(["|a", 41]);
    expect(shape(h)).toContain("|b");
    expect(shape(h).indexOf("|b")).toBe(shape(h).indexOf(51) - 1);
    expect(dividerKey(h, "d")).toBe(keyD);
    h.stop();
  });

  test("a running broadcast shows at the bottom before anything follows it, not while detached", async () => {
    const h = withBounds([{ id: "live", at: 130.5, live: true }]);
    h.host.store = range(1, 100);
    h.v.load();
    await h.host.reply();
    expect(shape(h).at(-1)).toBe("|live");

    h.el.userScroll(160);
    for (const n of range(101, 130)) {
      h.v.live(n);
    }
    frame(); // past hardMax: detached before anything of the stream arrived
    expect(h.v.detached).toBe(true);
    expect(shape(h)).not.toContain("|live");
    h.stop();
  });

  test("a trim keeps a divider's measured height", async () => {
    const h = withBounds([{ id: "s", at: 150.5, live: true }]);
    h.host.store = range(1, 100);
    h.v.load();
    await h.host.reply();
    const key = dividerKey(h, "s");
    h.v.measureRow({ offsetHeight: 40 } as HTMLElement, key);
    measure(h, 100, 20); // settles the effects
    for (const n of range(101, 160)) {
      h.v.live(n);
    }
    frame(); // trimmed back to 146..160
    expect(items(h)).toEqual(range(146, 160));
    const i = h.v.display.findIndex((r) => r.clientKey === key);
    expect(h.v.display[i + 1]).toMatchObject({ kind: "item", item: 151 });
    expect(h.v.layout.tops[i + 1] - h.v.layout.tops[i]).toBe(40);
    h.stop();
  });

  test("the label reads the clock time, and the date once it is not today", () => {
    const at = new Date(2026, 8, 28, 14, 5).getTime();
    expect(fmtStreamStarted(at, new Date(2026, 8, 28, 23, 59).getTime())).toBe("Stream started 14:05");
    const later = fmtStreamStarted(at, new Date(2026, 8, 29, 0, 1).getTime());
    expect(later.endsWith(" · Stream started 14:05")).toBe(true);
    expect(later).not.toContain("2026");
    expect(fmtStreamStarted(at, new Date(2027, 0, 2).getTime())).toContain("2026");
  });
});

describe("order and filter parity with the host", () => {
  test("compareCodePoints agrees with UTF-8 byte order, where < does not", () => {
    const ids = ["b", "\u{1F600}", "a", "Ａ", "ab", "", "\u{10000}", "￿", "z\u{1F600}", "zＡ"];
    const bytes = (s: string) => Buffer.from(s, "utf8");
    const byCodePoint = [...ids].sort(compareCodePoints);
    const byBytes = [...ids].sort((a, b) => Buffer.compare(bytes(a), bytes(b)));
    expect(byCodePoint).toEqual(byBytes);
    expect("\u{1F600}" < "Ａ").toBe(true); // UTF-16 order, which the host does not use
    expect(compareCodePoints("\u{1F600}", "Ａ")).toBeGreaterThan(0);
  });

  // Feed::Filter::Matches (frontend/src/chat/feed_query.hpp), transcribed.
  function hostMatches(f: FeedFilter, platform: string, profileUuid: string, accountId: string): boolean {
    switch (f.kind) {
      case "all":
        return true;
      case "platform":
        return platform.trim().toLowerCase() === f.platform.trim().toLowerCase();
      case "destination":
        return profileUuid === f.profileUuid || (profileUuid === "" && accountId === f.accountId);
    }
  }

  // The same table RunEventsPagingSelfTest runs on the host (obs_bootstrap.cpp). An empty
  // string on the host is an absent field here.
  const CASES: [string, string, string, string, string, string, string, boolean][] = [
    ["all", "", "", "", "kick", "", "k1", true],
    ["platform", "twitch", "", "", " Twitch ", "", "t1", true],
    ["platform", "youtube", "", "", "twitch", "", "t1", false],
    ["destination", "", "P1", "A1", "youtube", "P1", "A1", true],
    ["destination", "", "P1", "A1", "youtube", "P2", "A1", false],
    ["destination", "", "P1", "A1", "youtube", "", "A1", true],
    ["destination", "", "P1", "A1", "youtube", "", "A2", false],
    ["destination", "", "P1", "", "youtube", "", "", true],
    ["destination", "", "P1", "", "youtube", "", "A1", false],
  ];

  test.each(CASES)("%s %s%s/%s vs '%s' %s/%s", (kind, platform, profileUuid, accountId, ip, iprof, iacct, match) => {
    const sel: DestinationSelection =
      kind === "all" ? { kind: "all" } : kind === "platform" ? { kind: "platform", platform } : { kind: "destination", profileUuid };
    const destByUuid = new Map<string, DestinationIdentity>(
      accountId ? [[profileUuid, { profileUuid, accountId } as DestinationIdentity]] : [],
    );
    const item = { platform: ip, profileUuid: iprof || undefined, accountId: iacct || undefined };
    expect(matchesSelection(item, sel, destByUuid)).toBe(match);
    expect(hostMatches(filterOf(sel, destByUuid), ip, iprof, iacct)).toBe(match);
  });
});

describe("chat identity", () => {
  test("rows from two destinations span destinations, whatever is armed now", () => {
    expect(spansDestinations([{ accountId: "yt:1", profileUuid: "a" }, { accountId: "yt:1", profileUuid: "b" }])).toBe(true);
    expect(spansDestinations([{ accountId: "tw:1" }, { accountId: "tw:1" }])).toBe(false);
  });

  test("the key is destination plus id, spelled like the host's", () => {
    expect(chatKey({ accountId: "yt:1", profileUuid: "p", id: "m" })).toBe("yt:1@p:m");
    expect(chatKey({ accountId: "tw:1", id: "m" })).toBe("tw:1:m");
  });
});
