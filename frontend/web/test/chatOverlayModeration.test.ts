import { describe, expect, test } from "bun:test";
import type { ChatMessage, ChatModeration } from "$lib/api/bridge";
import { chatIdentity, moderationMatcher } from "$lib/docks/multichat/chatModeration";

// The default chatbox and chat leaderboard are bundler-free overlay scripts, so they are run
// here the way the overlay page runs them: as source, against stand-ins for the globals they
// touch, with the real chatIdentity and moderationMatcher the runtime hands them. Timers are
// stubbed never to fire; the tests read the scripts' own state, not the drawn DOM.

const ACCOUNT = "twitch:1";
const OTHER_ACCOUNT = "twitch:2";
// Past every seq a test admits unless it says otherwise.
const LATER = 1_000_000;

function msg(seq: number, author: string, over: Partial<ChatMessage> = {}): ChatMessage {
  return {
    platform: "twitch",
    accountId: ACCOUNT,
    channelId: "c",
    id: "m" + seq,
    ts: seq,
    seq,
    rx: seq,
    author: { name: author, id: author, color: "", badges: [] },
    fragments: [{ type: "text", text: "hello " + seq }],
    ...over,
  };
}

function op(over: Partial<ChatModeration>): ChatModeration {
  return { platform: "twitch", accountId: ACCOUNT, action: "all", before: LATER, ...over };
}

interface FakeEl {
  children: FakeEl[];
  style: Record<string, unknown> & { setProperty(): void };
  classList: { add(): void };
  className: string;
  textContent: string;
  hidden: boolean;
  scrollTop: number;
  scrollHeight: number;
  parentNode: FakeEl | null;
  readonly childElementCount: number;
  readonly firstElementChild: FakeEl | undefined;
  appendChild(c: FakeEl): FakeEl;
  removeChild(c: FakeEl): FakeEl;
}

function fakeEl(): FakeEl {
  const el: FakeEl = {
    children: [],
    style: { setProperty() {} },
    classList: { add() {} },
    className: "",
    textContent: "",
    hidden: false,
    scrollTop: 0,
    scrollHeight: 0,
    parentNode: null,
    get childElementCount() {
      return el.children.length;
    },
    get firstElementChild() {
      return el.children[0];
    },
    appendChild(c) {
      el.children.push(c);
      c.parentNode = el;
      return c;
    },
    removeChild(c) {
      el.children.splice(el.children.indexOf(c), 1);
      c.parentNode = null;
      return c;
    },
  };
  return el;
}

type ChatFn = (m: ChatMessage) => void;
type ModerationFn = (o: ChatModeration, removes: ReturnType<typeof moderationMatcher>) => void;

/** Run `template.js` of `widget` and return its chat and moderation handlers plus whatever
 * `expose` names from its top-level scope. */
async function load<T>(widget: string, expose: string) {
  const source = await Bun.file(new URL(`../public/overlay/${widget}/template.js`, import.meta.url)).text();
  let chat: ChatFn = () => {};
  let moderation: ModerationFn = () => {};
  const root = fakeEl();
  const document = {
    getElementById: () => root,
    createElement: () => fakeEl(),
    createTextNode: () => fakeEl(),
    documentElement: fakeEl(),
    body: fakeEl(),
  };
  const overlay = {
    chatIdentity,
    onLoad() {},
    onChat(fn: ChatFn) {
      chat = fn;
    },
    onChatModeration(fn: ModerationFn) {
      moderation = fn;
    },
    onStream() {},
  };
  const run = new Function(
    "document",
    "OBSOverlay",
    "setTimeout",
    "setInterval",
    "clearInterval",
    source + `\n;return ${expose};`,
  );
  const state = run(document, overlay, () => 0, () => 0, () => {}) as T;
  return {
    state,
    root,
    chat: (m: ChatMessage) => chat(m),
    moderate: (o: ChatModeration) => moderation(o, moderationMatcher(o)),
  };
}

interface Chatter {
  count: number;
  stamps: number[];
  lines: unknown[];
  evicted: Map<string, unknown>;
}

async function leaderboard() {
  const lb = await load<{ chatters: Map<string, Chatter> }>("default-chatleaderboard", "{ chatters }");
  return {
    ...lb,
    /** Each chatter's count, keyed by author id. */
    counts(): Record<string, number> {
      const out: Record<string, number> = {};
      for (const [key, c] of lb.state.chatters) {
        out[key.split("\n")[2]] = c.count;
      }
      return out;
    },
  };
}

describe("chat leaderboard moderation", () => {
  test("a delete takes that one message off its author", async () => {
    const lb = await leaderboard();
    lb.chat(msg(1, "ann"));
    lb.chat(msg(2, "ann"));
    lb.chat(msg(3, "bob"));
    lb.moderate(op({ action: "message", msgId: "m2" }));
    expect(lb.counts()).toEqual({ ann: 1, bob: 1 });
    expect(lb.state.chatters.values().next().value!.stamps).toEqual([1]);
  });

  test("a delete of a chatter's only message drops the row", async () => {
    const lb = await leaderboard();
    lb.chat(msg(1, "ann"));
    lb.chat(msg(2, "bob"));
    lb.moderate(op({ action: "message", msgId: "m1" }));
    expect(lb.counts()).toEqual({ bob: 1 });
  });

  test("a ban takes off only what the op names: its destination, before its bounds", async () => {
    const lb = await leaderboard();
    lb.chat(msg(1, "ann"));
    lb.chat(msg(2, "ann"));
    lb.chat(msg(3, "ann", { accountId: OTHER_ACCOUNT }));
    lb.chat(msg(4, "ann")); // said after the timeout ran out
    lb.chat(msg(5, "bob"));
    lb.moderate(op({ action: "user", authorId: "ann", before: 4 }));
    expect(lb.counts()).toEqual({ ann: 2, bob: 1 });
  });

  test("a ban that names every counted message drops the row", async () => {
    const lb = await leaderboard();
    lb.chat(msg(1, "ann"));
    lb.chat(msg(2, "ann"));
    lb.chat(msg(3, "bob"));
    lb.moderate(op({ action: "user", authorId: "ann" }));
    expect(lb.counts()).toEqual({ bob: 1 });
  });

  test("a ban compares the raw author id, as the host and the dock do, and still finds the row", async () => {
    const padded = { name: "ann", id: " ann ", color: "", badges: [] };
    const lb = await leaderboard();
    lb.chat(msg(1, "ann", { author: padded }));
    lb.moderate(op({ action: "user", authorId: "ann" }));
    expect(lb.counts()).toEqual({ ann: 1 }); // grouped under the trimmed id, not named by it
    lb.moderate(op({ action: "user", authorId: " ann " }));
    expect(lb.counts()).toEqual({});
  });

  test("a ban removes a heavy chatter entirely, however far back their messages go", async () => {
    const lb = await leaderboard();
    for (let seq = 1; seq <= 6000; seq++) {
      lb.chat(msg(seq, seq % 20 === 0 ? "ann" : "bob" + (seq % 7)));
    }
    expect(lb.counts().ann).toBe(300);
    lb.moderate(op({ action: "user", authorId: "ann" }));
    expect(lb.counts().ann).toBeUndefined();
    expect(Object.values(lb.counts()).reduce((a, b) => a + b, 0)).toBe(5700);
  });

  test("a ban bounded by beforeTs keeps what the author said after it", async () => {
    const lb = await leaderboard();
    for (let seq = 1; seq <= 5; seq++) {
      lb.chat(msg(seq, "ann"));
    }
    lb.moderate(op({ action: "user", authorId: "ann", beforeTs: 3 }));
    expect(lb.counts()).toEqual({ ann: 2 });
  });

  test("a delete takes back only the destination's copy of a message two accounts read", async () => {
    const lb = await leaderboard();
    lb.chat(msg(1, "ann"));
    lb.chat(msg(1, "ann", { accountId: OTHER_ACCOUNT }));
    lb.moderate(op({ action: "message", msgId: "m1" }));
    expect(lb.counts()).toEqual({ ann: 1 });
    lb.moderate(op({ action: "message", msgId: "m1", accountId: OTHER_ACCOUNT }));
    expect(lb.counts()).toEqual({});
  });

  describe("past the held-line bound", () => {
    // Enough later chat to evict every early line: the bound plus its eviction batch.
    const FLOOD = 110_000;

    async function flooded(early: ChatMessage[]) {
      const lb = await leaderboard();
      for (const m of early) {
        lb.chat(m);
      }
      for (let i = 0; i < FLOOD; i++) {
        const seq = 1000 + i;
        lb.chat(msg(seq, "bob" + (i % 40)));
      }
      return lb;
    }

    test("a ban still takes every evicted line on its destination, and none elsewhere", async () => {
      const lb = await flooded([
        msg(1, "ann"),
        msg(2, "ann"),
        msg(3, "ann", { accountId: OTHER_ACCOUNT }),
      ]);
      expect(lb.counts().ann).toBe(3);
      const ann = lb.state.chatters.get("twitch\ni\nann")!;
      expect(ann.lines.length).toBe(0); // every one of them is only in a tally now
      expect(ann.evicted.size).toBe(2);
      lb.moderate(op({ action: "user", authorId: "ann" }));
      expect(lb.counts().ann).toBe(1);
      lb.moderate(op({ action: "user", authorId: "ann", accountId: OTHER_ACCOUNT }));
      expect(lb.counts().ann).toBeUndefined();
    });

    test("a beforeTs ban takes evicted lines only once all of them were said by then", async () => {
      const lb = await flooded([msg(1, "ann"), msg(2, "ann")]);
      lb.moderate(op({ action: "user", authorId: "ann", beforeTs: 1 }));
      expect(lb.counts().ann).toBe(2);
      lb.moderate(op({ action: "user", authorId: "ann", beforeTs: 2 }));
      expect(lb.counts().ann).toBeUndefined();
    });
  });

  test("a clear of the whole chat leaves the standings alone", async () => {
    const lb = await leaderboard();
    lb.chat(msg(1, "ann"));
    lb.chat(msg(2, "bob"));
    lb.moderate(op({ action: "all" }));
    expect(lb.counts()).toEqual({ ann: 1, bob: 1 });
  });

  test("a repeated op debits nothing twice", async () => {
    const lb = await leaderboard();
    lb.chat(msg(1, "ann"));
    lb.chat(msg(2, "ann"));
    lb.moderate(op({ action: "message", msgId: "m1" }));
    lb.moderate(op({ action: "message", msgId: "m1" }));
    expect(lb.counts()).toEqual({ ann: 1 });
  });
});

describe("chatbox moderation", () => {
  async function chatbox() {
    const cb = await load<object>("default-chatbox", "{}");
    return {
      ...cb,
      /** The drawn lines, as the seq of the message each one shows. */
      seqs(): number[] {
        return cb.root.children.map((row) => drawn.get(row)!);
      },
      add(m: ChatMessage) {
        cb.chat(m);
        drawn.set(cb.root.children[cb.root.children.length - 1], m.seq);
      },
    };
  }
  const drawn = new Map<FakeEl, number>();

  test("a delete drops the one line", async () => {
    const cb = await chatbox();
    cb.add(msg(1, "ann"));
    cb.add(msg(2, "ann"));
    cb.moderate(op({ action: "message", msgId: "m1" }));
    expect(cb.seqs()).toEqual([2]);
  });

  test("a ban drops only the lines its bounds and destination name", async () => {
    const cb = await chatbox();
    cb.add(msg(1, "ann"));
    cb.add(msg(2, "ann", { accountId: OTHER_ACCOUNT }));
    cb.add(msg(3, "bob"));
    cb.add(msg(4, "ann"));
    cb.moderate(op({ action: "user", authorId: "ann", before: 4 }));
    expect(cb.seqs()).toEqual([2, 3, 4]);
  });

  test("a clear drops every line from its destination, and none said after it", async () => {
    const cb = await chatbox();
    cb.add(msg(1, "ann"));
    cb.add(msg(2, "bob", { accountId: OTHER_ACCOUNT }));
    cb.add(msg(3, "bob"));
    cb.moderate(op({ action: "all", before: 3 }));
    expect(cb.seqs()).toEqual([2, 3]);
  });
});
