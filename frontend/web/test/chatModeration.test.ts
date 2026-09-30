import { afterEach, describe, expect, setSystemTime, test } from "bun:test";
import type { ChatMessage, ChatModeration } from "$lib/api/bridge";
import { destinationKey, isDestinationKey } from "$lib/api/destinationKeys";
import { chatKey } from "$lib/docks/multichat/chatIntake";
import { moderationMatcher, redactChat, removedLabel, TOMBSTONE_TEXT } from "$lib/docks/multichat/chatModeration";
import { frame, harness, measure, useFrameMocks, type Harness, type TestConfig } from "./feedHarness";

useFrameMocks();

const ACCOUNT = "twitch:1";

function msg(seq: number, over: Partial<ChatMessage> = {}): ChatMessage {
  return {
    platform: "twitch",
    accountId: ACCOUNT,
    channelId: "c",
    id: "m" + seq,
    ts: seq,
    seq,
    rx: seq,
    author: { name: "a" + (seq % 3), id: "u" + (seq % 3), color: "", badges: [] },
    fragments: [{ type: "text", text: "hello " + seq }],
    ...over,
  };
}

// Past every seq a test admits unless it says otherwise.
const LATER = 1_000_000;

function op(over: Partial<ChatModeration>): ChatModeration {
  return { platform: "twitch", accountId: ACCOUNT, action: "all", before: LATER, ...over };
}

describe("moderationMatcher", () => {
  const m = msg(1); // id m1, author u1, account-wide
  const cases: [string, ChatModeration, Partial<ChatMessage>, boolean][] = [
    ["all: same destination", op({ action: "all" }), {}, true],
    ["admitted before the op", op({ before: 2 }), {}, true],
    ["admitted at the op's bound", op({ before: 1 }), {}, false],
    ["admitted after the op", op({ before: 1 }), { seq: 5 }, false],
    ["user: the author's line after the op", op({ action: "user", authorId: "u1", before: 1 }), {}, false],
    ["message: its id", op({ action: "message", msgId: "m1" }), {}, true],
    ["message: another id", op({ action: "message", msgId: "m2" }), {}, false],
    ["message: no msgId names nothing", op({ action: "message" }), {}, false],
    ["user: its author", op({ action: "user", authorId: "u1" }), {}, true],
    ["user: another author", op({ action: "user", authorId: "u2" }), {}, false],
    ["user: no authorId names nothing", op({ action: "user" }), {}, false],
    [
      "user: an author with no id is never named",
      op({ action: "user", authorId: "u1" }),
      { author: { name: "x", color: "", badges: [] } },
      false,
    ],
    [
      "user: neither side has an author id",
      op({ action: "user" }),
      { author: { name: "x", color: "", badges: [] } },
      false,
    ],
    ["another account", op({ accountId: "twitch:2" }), {}, false],
    ["op absent profile, row empty profile", op({}), { profileUuid: "" }, true],
    ["op empty profile, row absent profile", op({ profileUuid: "" }), {}, true],
    ["op names a profile, row account-wide", op({ profileUuid: "p1" }), {}, false],
    ["op account-wide, row under a profile", op({}), { profileUuid: "p1" }, false],
    ["same profile", op({ profileUuid: "p1" }), { profileUuid: "p1" }, true],
    ["another profile", op({ profileUuid: "p2" }), { profileUuid: "p1" }, false],
    ["no beforeTs: any time", op({ action: "user", authorId: "u1" }), { ts: 9_999_999 }, true],
    ["said before beforeTs", op({ action: "user", authorId: "u1", beforeTs: 100 }), { ts: 99 }, true],
    ["said at beforeTs", op({ action: "user", authorId: "u1", beforeTs: 100 }), { ts: 100 }, true],
    ["said after beforeTs", op({ action: "user", authorId: "u1", beforeTs: 100 }), { ts: 101 }, false],
    ["no time under a beforeTs", op({ action: "user", authorId: "u1", beforeTs: 100 }), { ts: 0 }, false],
    ["all: said after beforeTs", op({ beforeTs: 100 }), { ts: 101 }, false],
    ["message: its id at its own time", op({ action: "message", msgId: "m1", beforeTs: 1 }), {}, true],
    ["beforeTs never widens the seq bound", op({ before: 1, beforeTs: 100 }), { ts: 5 }, false],
  ];
  for (const [name, o, row, want] of cases) {
    test(name, () => {
      expect(moderationMatcher(o)({ ...m, ...row })).toBe(want);
    });
  }
});

describe("isDestinationKey", () => {
  const cases: [string, string | undefined][] = [
    ["twitch:1", undefined],
    ["twitch:1", ""],
    ["twitch:1", "p1"],
    ["twitch:1@p", "1"],
    ["twitch:1xp1", undefined],
    ["twitch:", "1@p1"],
    ["", "x"],
  ];
  test("agrees with destinationKey on every pair", () => {
    for (const [a, pa] of cases) {
      for (const [b, pb] of cases) {
        const key = destinationKey(a, pa);
        expect(isDestinationKey(key, b, pb)).toBe(key === destinationKey(b, pb));
      }
    }
  });
});

describe("redactChat", () => {
  const said = [{ type: "text" as const, text: "hello 1" }];

  test("empties the body, marks the action and keeps what it said", () => {
    const r = redactChat(msg(1, { paid: { kind: "cheer", amount: "100 bits" } }), { action: "user" });
    expect(r.fragments).toEqual([]);
    expect(r.deleted).toBe("user");
    expect(r.retracted).toEqual(said);
    expect(r.deletedLabel).toBeUndefined();
    expect(chatKey(r)).toBe(chatKey(msg(1)));
  });

  test("takes the platform's label", () => {
    const r = redactChat(msg(1), { action: "message", label: "[message retracted]" });
    expect(r.deletedLabel).toBe("[message retracted]");
    expect(removedLabel(r)).toBe("[message retracted]");
  });

  test("a later op keeps the text and the label an earlier one kept", () => {
    const first = redactChat(msg(1), { action: "message", label: "[message deleted]" });
    const later = redactChat(first, { action: "user" });
    expect(later.deleted).toBe("user");
    expect(later.retracted).toEqual(said);
    expect(later.deletedLabel).toBe("[message deleted]");
  });

  test("a row the host already redacted keeps the text the host kept", () => {
    const fromHost = msg(1, { fragments: [], deleted: "message", retracted: said });
    const r = redactChat(fromHost, { action: "all" });
    expect(r.retracted).toEqual(said);
    expect(r.deleted).toBe("all");
  });

  test("a line with no text keeps nothing", () => {
    const r = redactChat(msg(1, { fragments: [], paid: { kind: "cheer", amount: "100 bits" } }), { action: "message" });
    expect("retracted" in r).toBe(false);
  });

  test("a row already redacted the same way comes back as itself", () => {
    const r = redactChat(msg(1), { action: "message" });
    expect(redactChat(r, { action: "message" })).toBe(r);
    const labelled = redactChat(msg(1), { action: "message", label: "[message deleted]" });
    expect(redactChat(labelled, { action: "message" })).toBe(labelled);
  });

  test("with no platform label, the label says what happened", () => {
    expect(removedLabel(redactChat(msg(1), { action: "message" }))).toBe(TOMBSTONE_TEXT.message);
    expect(removedLabel(redactChat(msg(1), { action: "user" }))).toBe("Messages removed by a moderator");
    expect(removedLabel(redactChat(msg(1), { action: "all" }))).toBe("Chat cleared");
  });
});

// NUMBERS-sized window over chat rows: viewport 100px, rows 20px, top row 10px.
const CHAT: TestConfig<ChatMessage> = {
  estimate: 20,
  topHeight: 10,
  key: chatKey,
  compare: (a, b) => a.seq - b.seq,
  timeOf: (m) => m.rx,
  base: 5,
  page: 5,
  highWater: 20,
  hardMax: 40,
  prefetchScreens: 0,
};

function moderate(h: Harness<ChatMessage>, o: ChatModeration): void {
  h.v.patch(
    moderationMatcher(o),
    (m) => redactChat(m, o),
  );
  frame();
}

async function loaded(n: number): Promise<Harness<ChatMessage>> {
  const h = harness<ChatMessage>(CHAT);
  h.host.store = Array.from({ length: n }, (_, i) => msg(i + 1));
  h.v.load();
  await h.host.reply();
  return h;
}

describe("FeedVirtualizer.patch with moderation", () => {
  test("a redacted row keeps its clientKey and its place", async () => {
    const h = await loaded(15);
    const before = h.v.rows.map((r) => [r.clientKey, r.item.seq]);
    moderate(h, op({ action: "message", msgId: "m7" }));
    expect(h.v.rows.map((r) => [r.clientKey, r.item.seq])).toEqual(before);
    const row = h.v.rows.find((r) => r.item.id === "m7")!;
    expect(row.item.deleted).toBe("message");
    expect(row.item.fragments).toEqual([]);
    expect(row.item.retracted).toEqual([{ type: "text", text: "hello 7" }]);
    expect(h.v.rows.filter((r) => r.item.deleted).length).toBe(1);
    h.stop();
  });

  test("a user op redacts every row by that author and no other", async () => {
    const h = await loaded(15);
    moderate(h, op({ action: "user", authorId: "u2" }));
    for (const r of h.v.rows) {
      expect(r.item.deleted).toBe(r.item.author.id === "u2" ? "user" : undefined);
    }
    h.stop();
  });

  test("a shrinking redacted row above the viewport leaves the reader in place", async () => {
    const h = await loaded(15);
    const tall = h.v.rows.find((r) => r.item.seq === 3)!;
    measure(h, tall.item, 60);
    h.el.userScroll(150); // row 3 spans 50-110, above the viewport
    expect(h.v.autoStick).toBe(false);
    const eye = h.v.rows.find((r) => r.item.seq === 8)!.clientKey;
    const topOf = () => {
      const i = h.v.display.findIndex((r) => r.clientKey === eye);
      return h.v.layout.tops[i] - h.el.scrollTop;
    };
    const at = topOf();
    moderate(h, op({ action: "message", msgId: "m3" }));
    expect(topOf()).toBe(at);
    const redacted = h.v.rows.find((r) => r.item.seq === 3)!;
    expect(redacted.clientKey).toBe(tall.clientKey);
    measure(h, redacted.item, 20);
    expect(topOf()).toBe(at);
    h.stop();
  });

  test("an op matching nothing leaves the window untouched", async () => {
    const h = await loaded(10);
    const before = h.v.rows;
    moderate(h, op({ accountId: "kick:9" }));
    moderate(h, op({ action: "message", msgId: "nope" }));
    expect(h.v.rows).toBe(before);
    h.stop();
  });

  test("a live row still queued for the next frame is redacted too", async () => {
    const h = await loaded(5);
    h.v.live(msg(6));
    moderate(h, op({ action: "message", msgId: "m6" }));
    expect(h.v.rows.at(-1)!.item.deleted).toBe("message");
    h.stop();
  });

  test("a page read before the op lands redacted", async () => {
    const h = harness<ChatMessage>(CHAT);
    h.host.store = Array.from({ length: 5 }, (_, i) => msg(i + 1));
    h.v.load();
    moderate(h, op({ action: "all" }));
    await h.host.reply(); // the host answers from its pre-op copy
    expect(h.v.rows.length).toBe(5);
    expect(h.v.rows.every((r) => r.item.deleted === "all" && r.item.fragments.length === 0)).toBe(true);
    expect(h.v.rows.every((r) => r.item.retracted?.[0]?.type === "text")).toBe(true);
    h.stop();
  });

  // Review A, exactly: the op, then a message admitted after it, reach the dock while a
  // load is in flight, and the page the host read after both holds that message intact.
  for (const [action, extra] of [
    ["all", {}],
    ["user", { authorId: "u0" }],
  ] as const) {
    test(`a message admitted after a ${action} op on the same page stays intact`, async () => {
      const h = harness<ChatMessage>(CHAT);
      h.host.store = Array.from({ length: 5 }, (_, i) => msg(i + 1));
      h.v.load();
      moderate(h, op({ action, ...extra, before: 6 }));
      const late = msg(6); // author u0
      h.v.live(late);
      frame();
      h.host.store = [...h.host.store, late];
      await h.host.reply();
      expect(h.v.rows.map((r) => r.item.seq)).toEqual([1, 2, 3, 4, 5, 6]);
      expect(h.v.rows.at(-1)!.item.deleted).toBeUndefined();
      expect(h.v.rows.at(-1)!.item.fragments.length).toBe(1);
      expect(h.v.rows.find((r) => r.item.seq === 3)!.item.deleted).toBe(action);
      h.stop();
    });
  }

  test("a user op lands on a page read before it, only on that author", async () => {
    const h = harness<ChatMessage>(CHAT);
    h.host.store = Array.from({ length: 6 }, (_, i) => msg(i + 1));
    h.v.load();
    moderate(h, op({ action: "user", authorId: "u2" }));
    await h.host.reply();
    for (const r of h.v.rows) {
      expect(r.item.deleted).toBe(r.item.author.id === "u2" ? "user" : undefined);
    }
    h.stop();
  });

  test("an op while an older page is in flight lands on the page and on a held row", async () => {
    const h = harness<ChatMessage>({ ...CHAT, prefetchScreens: 1.5 });
    // 13 is missing from the host's pages, so arriving live it sorts inside the next page.
    h.host.store = Array.from({ length: 30 }, (_, i) => msg(i + 1)).filter((m) => m.seq !== 13);
    h.v.load();
    await h.host.reply();
    expect(h.v.rows[0].item.seq).toBe(16);
    h.el.userScroll(0);
    expect(h.host.calls.length).toBe(1); // the older page
    moderate(h, op({ action: "user", authorId: "u1" }));
    h.v.live(msg(13)); // held for the older page
    frame();
    await h.host.reply();
    expect(h.v.rows.slice(0, 6).map((r) => r.item.seq)).toEqual([10, 11, 12, 13, 14, 15]);
    for (const r of h.v.rows) {
      expect(r.item.deleted).toBe(r.item.author.id === "u1" ? "user" : undefined);
    }
    h.stop();
  });

  test("a trim that drops an older page in flight keeps the ops for the next page", async () => {
    const h = harness<ChatMessage>({ ...CHAT, prefetchScreens: 1.5 });
    h.host.store = Array.from({ length: 10 }, (_, i) => msg(i + 1));
    h.host.maxLimit = 3; // a short window, so the older page is asked for while stuck
    h.v.load();
    await h.host.reply();
    expect(h.host.calls.length).toBe(1);
    expect(h.v.autoStick).toBe(true);
    moderate(h, op({ action: "all", before: 11 }));
    for (let seq = 11; seq <= 80; seq++) {
      h.v.live(msg(seq));
    }
    frame(); // past highWater: trims to base and drops the older page
    expect(h.v.rows[0].item.seq).toBeGreaterThan(10);
    await h.host.reply(); // the dropped page lands and is discarded
    expect(h.v.rows[0].item.seq).toBeGreaterThan(10);
    h.host.maxLimit = Infinity;
    h.host.store = Array.from({ length: 12 }, (_, i) => msg(i + 1));
    h.v.load();
    await h.host.reply(); // read before the op: 1..10 intact on the host's side
    for (const r of h.v.rows) {
      expect(r.item.deleted).toBe(r.item.seq < 11 ? "all" : undefined);
    }
    h.stop();
  });

  // An op made before a clear has `before` at or below every seq admitted after it (the
  // host's seq never goes back down), so it cannot touch those rows, forgotten or not.
  test("an op from before a clear leaves the rows after it alone", async () => {
    const h = await loaded(5);
    moderate(h, op({ action: "all", before: 6 }));
    h.v.reset(1);
    h.v.live(msg(6));
    frame();
    expect(h.v.rows.map((r) => r.item.deleted)).toEqual([undefined]);
    h.stop();
  });

  test("a page from after a clear, with no reset yet, is left alone by earlier ops", async () => {
    const h = await loaded(5);
    moderate(h, op({ action: "all", before: 6 }));
    h.host.epoch = 1;
    h.host.store = [msg(6), msg(7)];
    h.v.load();
    await h.host.reply();
    expect(h.v.rows.map((r) => r.item.deleted)).toEqual([undefined, undefined]);
    h.stop();
  });

  // A dock opened after a clear starts at epoch 0, so its first page carries a newer one.
  test("a fresh feed's first page, read before an op and from after a clear, lands redacted", async () => {
    const h = harness<ChatMessage>(CHAT);
    h.host.epoch = 3;
    h.host.store = Array.from({ length: 5 }, (_, i) => msg(i + 10));
    h.v.load();
    moderate(h, op({ action: "message", msgId: "m12", before: 15 }));
    await h.host.reply();
    expect(h.v.rows.map((r) => r.item.deleted)).toEqual([undefined, undefined, "message", undefined, undefined]);
    h.stop();
  });

  // A row admitted before the op whose frame the host posted after it.
  test("a live row admitted before an op but arriving after it lands redacted", async () => {
    const h = await loaded(5);
    moderate(h, op({ action: "all", before: 7 }));
    h.v.live(msg(6));
    h.v.live(msg(7));
    frame();
    expect(h.v.rows.slice(-2).map((r) => r.item.deleted)).toEqual(["all", undefined]);
    h.stop();
  });
});

describe("FeedVirtualizer patch memory", () => {
  const T0 = new Date("2026-09-29T12:00:00Z").getTime();
  afterEach(() => setSystemTime());

  test("a page in flight keeps every op made since it was asked for, however long", async () => {
    setSystemTime(T0);
    const h = harness<ChatMessage>(CHAT);
    h.host.store = Array.from({ length: 5 }, (_, i) => msg(i + 1));
    h.v.load();
    moderate(h, op({ action: "all", before: 6 }));
    setSystemTime(T0 + 60_000);
    moderate(h, op({ action: "message", msgId: "none", before: 6 })); // trims
    await h.host.reply();
    expect(h.v.rows.every((r) => r.item.deleted === "all")).toBe(true);
    h.stop();
  });

  test("with no page in flight, an op is kept for a late frame until the window passes", async () => {
    setSystemTime(T0);
    const h = await loaded(5);
    moderate(h, op({ action: "all", before: 8 }));
    setSystemTime(T0 + 9_000);
    moderate(h, op({ action: "message", msgId: "none", before: 8 }));
    h.v.live(msg(6));
    frame();
    expect(h.v.rows.at(-1)!.item.deleted).toBe("all");
    setSystemTime(T0 + 11_000);
    moderate(h, op({ action: "message", msgId: "none", before: 8 })); // trims the first
    h.v.live(msg(7));
    frame();
    expect(h.v.rows.at(-1)!.item.deleted).toBeUndefined();
    h.stop();
  });

  test("past the cap the oldest op goes, even one a page in flight needs", async () => {
    setSystemTime(T0);
    const h = harness<ChatMessage>(CHAT);
    h.host.store = Array.from({ length: 5 }, (_, i) => msg(i + 1));
    h.v.load();
    moderate(h, op({ action: "all", before: 6 }));
    for (let i = 0; i < 4095; i++) {
      h.v.patch(() => false, (m) => m);
    }
    await h.host.reply();
    expect(h.v.rows.every((r) => r.item.deleted === "all")).toBe(true); // 4096 held
    h.v.load();
    moderate(h, op({ action: "all", before: 6 })); // held with the rest
    for (let i = 0; i < 4096; i++) {
      h.v.patch(() => false, (m) => m);
    }
    await h.host.reply();
    expect(h.v.rows.every((r) => r.item.deleted === undefined)).toBe(true);
    h.stop();
  });
});
