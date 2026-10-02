import { describe, expect, test } from "bun:test";
import {
  SessionTally,
  audienceTotal,
  countEvents,
  countTotals,
  easeOutCubic,
  effectiveAnimation,
  eventKind,
  templateParts,
  tweenValue,
  viewerTotal,
  withOffset,
  type TallyEvent,
  type TallyFrame,
  type TallyTotal,
} from "../src/overlay/counter";

const ALL = new Set(["twitch", "youtube", "kick", "facebook"]);
const OPEN = { since: null, until: null };

function ev(id: string, type: string, platform: string, ts: number, over: Partial<TallyEvent> = {}): TallyEvent {
  return { id, type: type as TallyEvent["type"], platform: platform as TallyEvent["platform"], ts, ...over };
}

function total(platform: string, type: string, events: number, over: Partial<TallyTotal> = {}): TallyTotal {
  return { platform, type, kind: "", events, units: events, amount: 0, ...over };
}

function frame(since: number | null, until: number | null, totals: TallyTotal[] = [], recentIds: string[] = []): TallyFrame {
  return { since, until, totals, recentIds };
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

  test("new members count new members only, never a milestone", () => {
    const members = [
      ev("m1", "member", "youtube", 1),
      ev("m2", "member", "youtube", 2, { months: 12 }),
      ev("m3", "member", "youtube", 3, { months: 0 }),
    ];
    expect(countEvents(members, "member", ALL, OPEN)).toBe(2);
  });
});

describe("eventKind", () => {
  test("a membership is new or a milestone, everything else has no kind", () => {
    expect(eventKind({ type: "member" })).toBe("new");
    expect(eventKind({ type: "member", months: 0 })).toBe("new");
    expect(eventKind({ type: "member", months: 3 })).toBe("milestone");
    expect(eventKind({ type: "resub", months: 3 })).toBe("");
  });
});

describe("countTotals", () => {
  const totals = [
    total("twitch", "follow", 3),
    total("youtube", "follow", 2),
    total("twitch", "subgift", 2, { units: 7 }),
    total("twitch", "cheer", 2, { amount: 750 }),
    total("youtube", "member", 4, { kind: "new" }),
    total("youtube", "member", 9, { kind: "milestone" }),
    total("twitch", "resub", 1),
    total("twitch", "sub", 1),
  ];

  test("each source reads its own measure off the host's sums", () => {
    expect(countTotals(totals, "follow", ALL)).toBe(5);
    expect(countTotals(totals, "subgift", ALL)).toBe(7);
    expect(countTotals(totals, "cheer", ALL)).toBe(750);
    expect(countTotals(totals, "sub", ALL)).toBe(2);
  });

  test("new members leave the milestones out", () => {
    expect(countTotals(totals, "member", ALL)).toBe(4);
  });

  test("the platform filter applies, and an unknown source counts nothing", () => {
    expect(countTotals(totals, "follow", new Set(["youtube"]))).toBe(2);
    expect(countTotals(totals, "likes", ALL)).toBe(0);
  });
});

describe("SessionTally", () => {
  test("is not ready until the host's tally arrives", () => {
    const t = new SessionTally();
    expect(t.ready).toBe(false);
    expect(t.seed(frame(null, null))).toBe("seed");
    expect(t.ready).toBe(true);
    expect(t.count("follow", ALL)).toBe(0);
  });

  test("a live broadcast's count is the host's totals plus what arrives after them", () => {
    const t = new SessionTally();
    t.connected();
    t.seed(frame(100, null, [total("twitch", "follow", 40)], ["a"]));
    expect(t.count("follow", ALL)).toBe(40);
    expect(t.add(ev("c", "follow", "kick", 140))).toBe(true);
    expect(t.count("follow", ALL)).toBe(41);
  });

  test("an event both counted by the host and delivered live counts once", () => {
    const t = new SessionTally();
    t.connected();
    t.seed(frame(100, null, [total("twitch", "follow", 2)], ["a", "b"]));
    t.add(ev("b", "follow", "twitch", 130));
    expect(t.count("follow", ALL)).toBe(2);
    expect(t.add(ev("b", "follow", "twitch", 130))).toBe(false);
  });

  test("a reconnect's tally covers what earlier connections delivered", () => {
    const t = new SessionTally();
    t.connected();
    t.seed(frame(100, null));
    // Far more than recentIds names, all counted by the host by the time it reconnects.
    for (let i = 0; i < 100; i++) {
      t.add(ev("e" + i, "follow", "twitch", 200 + i));
    }
    expect(t.count("follow", ALL)).toBe(100);
    t.connected();
    t.seed(frame(100, null, [total("twitch", "follow", 100)], ["e99"]));
    expect(t.count("follow", ALL)).toBe(100);
  });

  test("events before the window are not counted, but not dropped either", () => {
    const t = new SessionTally();
    t.connected();
    t.seed(frame(null, null));
    t.add(ev("early", "follow", "twitch", 90));
    t.add(ev("inside", "follow", "twitch", 110));
    expect(t.count("follow", ALL)).toBe(2);
    // The host's start, in the same connection: what it already counted is named.
    expect(t.seed(frame(100, null, [total("twitch", "follow", 1)], ["inside"]))).toBe("window");
    expect(t.count("follow", ALL)).toBe(1);
  });

  test("a start's tally keeps an event that reached the page between its read and its send", () => {
    const t = new SessionTally();
    t.connected();
    t.seed(frame(10, 20));
    t.add(ev("raced", "follow", "twitch", 105));
    expect(t.seed(frame(100, null, [], []))).toBe("window");
    expect(t.count("follow", ALL)).toBe(1);
  });

  test("an ended broadcast holds the host's figure and never counts on", () => {
    const t = new SessionTally();
    t.connected();
    t.seed(frame(100, null, [total("twitch", "follow", 3)]));
    t.add(ev("x", "follow", "twitch", 150));
    expect(t.count("follow", ALL)).toBe(4);
    expect(t.seed(frame(100, 200, [total("twitch", "follow", 4)], ["x"]))).toBe("window");
    expect(t.count("follow", ALL)).toBe(4);
    // Late, with a time inside the closed window: the host did not count it either.
    t.add(ev("late", "follow", "twitch", 190));
    t.add(ev("after", "follow", "twitch", 250));
    expect(t.count("follow", ALL)).toBe(4);
    expect(t.window).toEqual({ since: 100, until: 200 });
  });

  test("off air after a restart shows the finished broadcast from totals alone", () => {
    const t = new SessionTally();
    t.connected();
    // A restarted host names no ids: it has no live page to dedupe against.
    t.seed(frame(100, 200, [total("twitch", "follow", 12), total("youtube", "member", 2, { kind: "new" })]));
    expect(t.count("follow", ALL)).toBe(12);
    expect(t.count("member", ALL)).toBe(2);
  });

  test("before any broadcast was recorded it counts since load, across reconnects", () => {
    const t = new SessionTally();
    t.connected();
    t.seed(frame(null, null));
    t.add(ev("a", "follow", "twitch", 5));
    t.connected();
    t.seed(frame(null, null));
    expect(t.count("follow", ALL)).toBe(1);
  });

  test("a test event counts on top until the next tally or clear, and never twice", () => {
    const t = new SessionTally();
    t.connected();
    t.seed(frame(100, 200, [total("twitch", "follow", 4)]));
    expect(t.addTest(ev("test-1", "follow", "twitch", 9999))).toBe(true);
    expect(t.addTest(ev("test-1", "follow", "twitch", 9999))).toBe(false);
    expect(t.count("follow", ALL)).toBe(5);
    expect(t.clearTests()).toBe(true);
    expect(t.clearTests()).toBe(false);
    expect(t.count("follow", ALL)).toBe(4);
    t.addTest(ev("test-2", "follow", "twitch", 9999));
    t.seed(frame(100, 200, [total("twitch", "follow", 4)]));
    expect(t.count("follow", ALL)).toBe(4);
  });

  test("a malformed tally frame counts nothing rather than throwing", () => {
    const t = new SessionTally();
    t.seed({ since: "x", until: null, totals: [{ platform: "twitch" }], recentIds: [7] } as unknown as TallyFrame);
    expect(t.ready).toBe(true);
    expect(t.window).toEqual({ since: null, until: null });
    expect(t.count("follow", ALL)).toBe(0);
  });

  test("an event without an id or a time is ignored", () => {
    const t = new SessionTally();
    expect(t.add(ev("", "follow", "twitch", 1))).toBe(false);
    expect(t.add({ id: "n", type: "follow", platform: "twitch" } as unknown as TallyEvent)).toBe(false);
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

  test("a format without {n}, or a blank one, shows the number alone", () => {
    expect(templateParts("Subs")).toEqual(["", ""]);
    expect(templateParts("")).toEqual(["", ""]);
  });

  test("other tokens stay verbatim", () => {
    expect(templateParts("{name}: {n}")).toEqual(["{name}: ", ""]);
  });
});
