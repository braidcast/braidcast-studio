import { describe, expect, test } from "bun:test";
import {
  SessionTally,
  audienceTotal,
  countEvents,
  easeOutCubic,
  effectiveAnimation,
  templateParts,
  tweenValue,
  viewerTotal,
  withOffset,
  type TallyEvent,
} from "../src/overlay/counter";

const ALL = new Set(["twitch", "youtube", "kick", "facebook"]);
const OPEN = { since: null, until: null };

function ev(id: string, type: string, platform: string, ts: number, over: Partial<TallyEvent> = {}): TallyEvent {
  return { id, type: type as TallyEvent["type"], platform: platform as TallyEvent["platform"], ts, ...over };
}

describe("countEvents", () => {
  const events = [
    ev("f1", "follow", "twitch", 10),
    ev("f2", "follow", "youtube", 11),
    ev("s1", "sub", "twitch", 12),
    ev("s2", "resub", "kick", 13),
    ev("g1", "subgift", "twitch", 14, { count: 5 }),
    ev("g2", "subgift", "kick", 15),
    ev("c1", "cheer", "twitch", 16, { amount: 500 }),
    ev("c2", "cheer", "twitch", 17),
    ev("k1", "kicks", "kick", 18, { amount: 100 }),
    ev("r1", "raid", "twitch", 19, { amount: 40 }),
  ];

  test("follow counts events, and a YouTube subscriber is a youtube follow", () => {
    expect(countEvents(events, "follow", ALL, OPEN)).toBe(2);
    expect(countEvents(events, "follow", new Set(["youtube"]), OPEN)).toBe(1);
  });

  test("sub counts resubs too", () => {
    expect(countEvents(events, "sub", ALL, OPEN)).toBe(2);
  });

  test("gifted subs count gifts, with an unreported count as one", () => {
    expect(countEvents(events, "subgift", ALL, OPEN)).toBe(6);
  });

  test("bits and Kicks sum the amount, an omitted amount being zero", () => {
    expect(countEvents(events, "cheer", ALL, OPEN)).toBe(500);
    expect(countEvents(events, "kicks", ALL, OPEN)).toBe(100);
  });

  test("raids count raids, not raiders", () => {
    expect(countEvents(events, "raid", ALL, OPEN)).toBe(1);
  });

  test("the platform filter excludes unticked platforms", () => {
    expect(countEvents(events, "sub", new Set(["kick"]), OPEN)).toBe(1);
    expect(countEvents(events, "sub", new Set(), OPEN)).toBe(0);
  });

  test("the window bounds are inclusive", () => {
    expect(countEvents(events, "follow", ALL, { since: 11, until: null })).toBe(1);
    expect(countEvents(events, "follow", ALL, { since: null, until: 10 })).toBe(1);
  });

  test("an unknown source counts nothing", () => {
    expect(countEvents(events, "likes", ALL, OPEN)).toBe(0);
  });
});

describe("SessionTally", () => {
  test("is not ready until seeded", () => {
    const t = new SessionTally();
    expect(t.ready).toBe(false);
    t.seed({ since: null, until: null, live: false, events: [] });
    expect(t.ready).toBe(true);
    expect(t.count("follow", ALL)).toBe(0);
  });

  test("a seed rebuilds a live broadcast's count, and a live event already in it counts once", () => {
    const t = new SessionTally();
    t.seed({
      since: 100,
      until: null,
      live: true,
      events: [ev("a", "follow", "twitch", 120), ev("b", "follow", "youtube", 130)],
    });
    expect(t.count("follow", ALL)).toBe(2);
    expect(t.add(ev("b", "follow", "youtube", 130))).toBe(false);
    expect(t.add(ev("c", "follow", "kick", 140))).toBe(true);
    expect(t.count("follow", ALL)).toBe(3);
  });

  test("a seed's events before its window never count", () => {
    const t = new SessionTally();
    t.seed({ since: 100, until: null, live: true, events: [ev("old", "follow", "twitch", 99)] });
    expect(t.count("follow", ALL)).toBe(0);
  });

  test("an ended broadcast holds its count instead of resetting or counting on", () => {
    const t = new SessionTally();
    t.seed({ since: null, until: null, live: false, events: [] });
    expect(t.onStream({ active: true, startedAt: 100 }, 100)).toBe(true);
    t.add(ev("a", "follow", "twitch", 150));
    expect(t.onStream({ active: false, startedAt: null }, 200)).toBe(true);
    expect(t.window).toEqual({ since: 100, until: 200 });
    t.add(ev("late", "follow", "twitch", 250));
    expect(t.count("follow", ALL)).toBe(1);
  });

  test("an off-air seed restores the most recent broadcast's count", () => {
    const t = new SessionTally();
    t.seed({
      since: 100,
      until: 200,
      live: false,
      events: [ev("a", "follow", "twitch", 150), ev("after", "follow", "twitch", 250)],
    });
    expect(t.count("follow", ALL)).toBe(1);
  });

  test("a new broadcast starts over, keeping only what falls inside it", () => {
    const t = new SessionTally();
    t.seed({ since: 100, until: 200, live: false, events: [ev("a", "follow", "twitch", 150)] });
    // Arrived just before the start frame did, inside the new broadcast.
    t.add(ev("early", "follow", "twitch", 310));
    expect(t.onStream({ active: true, startedAt: 300 }, 300)).toBe(true);
    expect(t.count("follow", ALL)).toBe(1);
    expect(t.onStream({ active: true, startedAt: 300 }, 400)).toBe(false);
  });

  test("before any broadcast it counts since load, and an end frame closes nothing", () => {
    const t = new SessionTally();
    t.seed({ since: null, until: null, live: false, events: [] });
    t.add(ev("a", "follow", "twitch", 5));
    expect(t.onStream({ active: false, startedAt: null }, 10)).toBe(false);
    expect(t.count("follow", ALL)).toBe(1);
  });

  test("a broadcast still connecting opens no window", () => {
    const t = new SessionTally();
    expect(t.onStream({ active: true, startedAt: null }, 10)).toBe(false);
    expect(t.window).toEqual({ since: null, until: null });
  });

  test("an event without an id is ignored", () => {
    const t = new SessionTally();
    expect(t.add(ev("", "follow", "twitch", 1))).toBe(false);
  });
});

describe("totals", () => {
  test("a platform that did not report is left out, never read as zero", () => {
    expect(viewerTotal({ twitch: 10 }, new Set(["twitch", "youtube"]))).toBe(10);
    expect(viewerTotal({ twitch: 10, youtube: 0 }, new Set(["twitch", "youtube"]))).toBe(10);
  });

  test("no selected platform reporting is no figure", () => {
    expect(viewerTotal({ twitch: 10 }, new Set(["youtube"]))).toBeNull();
    expect(viewerTotal(undefined, ALL)).toBeNull();
    expect(viewerTotal({}, ALL)).toBeNull();
  });

  test("a genuine zero is a figure", () => {
    expect(viewerTotal({ kick: 0 }, ALL)).toBe(0);
  });

  test("audience totals skip withheld platforms and sum the rest", () => {
    const per = { twitch: { count: 1200 }, youtube: { count: null }, kick: { count: 30 } };
    expect(audienceTotal(per, ALL)).toBe(1230);
    expect(audienceTotal(per, new Set(["youtube"]))).toBeNull();
  });
});

describe("offset", () => {
  test("adds a whole offset, typed or numeric, and leaves no figure alone", () => {
    expect(withOffset(5, 10)).toBe(15);
    expect(withOffset(5, "10")).toBe(15);
    expect(withOffset(5, -2)).toBe(3);
    expect(withOffset(5, 2.9)).toBe(7);
    expect(withOffset(5, "abc")).toBe(5);
    expect(withOffset(5, "")).toBe(5);
    expect(withOffset(5, undefined)).toBe(5);
    expect(withOffset(null, 10)).toBeNull();
  });
});

describe("animation", () => {
  test("reduced motion makes every change instant", () => {
    for (const a of ["none", "countup", "odometer", "pop"]) {
      expect(effectiveAnimation(a, true)).toBe("none");
    }
  });

  test("the chosen animation plays otherwise, and an unknown one is instant", () => {
    expect(effectiveAnimation("odometer", false)).toBe("odometer");
    expect(effectiveAnimation("spin", false)).toBe("none");
    expect(effectiveAnimation(undefined, false)).toBe("none");
  });

  test("the count-up eases out and lands exactly", () => {
    expect(easeOutCubic(0)).toBe(0);
    expect(easeOutCubic(1)).toBe(1);
    expect(easeOutCubic(2)).toBe(1);
    expect(easeOutCubic(0.5)).toBeGreaterThan(0.5);
    expect(tweenValue(10, 20, 0)).toBe(10);
    expect(tweenValue(10, 20, 1)).toBe(20);
    expect(tweenValue(20, 10, 1)).toBe(10);
  });
});

describe("templateParts", () => {
  test("splits around every {n}, keeping the text around it", () => {
    expect(templateParts("{n}")).toEqual(["", ""]);
    expect(templateParts("YouTube subs today: {n}")).toEqual(["YouTube subs today: ", ""]);
    expect(templateParts("{n} of {n}")).toEqual(["", " of ", ""]);
  });

  test("a format without {n} shows no number, and other tokens stay verbatim", () => {
    expect(templateParts("Subs")).toEqual(["Subs"]);
    expect(templateParts("{name}: {n}")).toEqual(["{name}: ", ""]);
  });
});
