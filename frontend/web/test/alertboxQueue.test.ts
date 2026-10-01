import { describe, expect, test } from "bun:test";
import { fillTemplate } from "../src/overlay/fillTemplate";

// The default alertbox is a bundler-free overlay script, so it is loaded here the way the
// overlay page loads it: as source, run against stand-ins for the globals it touches. The
// deck's timers are stubbed never to fire, so nothing leaves the queue and the test sees
// exactly the backlog a storm builds while one alert holds the deck.

interface Burst {
  group: string | null;
  overflow?: boolean;
  summaryFrom: number;
  events: { type: string; id: string }[];
}

const SOURCE = await Bun.file(new URL("../public/overlay/default-alertbox/template.js", import.meta.url)).text();
const SCHEMA: { key: string; default?: unknown }[] = await Bun.file(
  new URL("../public/overlay/default-alertbox/fields.json", import.meta.url),
).json();
// What the host injects for a stock widget: each schema default, under the user's overrides.
const DEFAULT_FIELDS = Object.fromEntries(SCHEMA.map((f) => [f.key, f.default]));

function fakeEl(): Record<string, unknown> {
  const el: Record<string, unknown> = {
    dataset: {},
    style: { setProperty() {} },
    classList: { add() {}, remove() {}, toggle() {} },
    textContent: "",
    offsetWidth: 0,
    offsetHeight: 0,
    querySelector: () => fakeEl(),
    querySelectorAll: () => [],
    insertBefore() {},
    addEventListener() {},
    remove() {},
  };
  el.content = { firstElementChild: { cloneNode: () => fakeEl() } };
  return el;
}

function alertbox(fields: Record<string, unknown> = DEFAULT_FIELDS) {
  let now = 0;
  let handler: (e: unknown) => void = () => {};
  const document = {
    getElementById: () => fakeEl(),
    documentElement: fakeEl(),
    body: fakeEl(),
  };
  const overlay = {
    fields,
    onLoad() {},
    onEvent(fn: (e: unknown) => void) {
      handler = fn;
    },
    onEventRedaction() {},
    formatAmount: () => "",
    formatAmountText: () => "",
    formatCount: (n: number) => String(n),
    fillTemplate,
    textField: (f: Record<string, unknown>, k: string, d: string) => (f[k] != null ? String(f[k]) : d),
    playSound() {},
  };
  const run = new Function(
    "document",
    "OBSOverlay",
    "performance",
    "setTimeout",
    "clearTimeout",
    SOURCE + "\n;return { queue: () => queue, current: () => current, fillSummary, TYPES };",
  );
  const box = run(document, overlay, { now: () => now }, () => 0, () => {}) as {
    queue: () => Burst[];
    current: () => Burst | null;
    fillSummary: (el: unknown, b: Burst) => void;
    TYPES: Record<string, { msg?: string; group?: string; alone?: string }>;
  };
  let seq = 0;
  return {
    box,
    /** One event of `type`, `gapMs` after the previous one. */
    fire(type: string, gapMs = 2000) {
      now += gapMs;
      handler({ type, id: type + ++seq, actorName: "a" });
    },
    /** Every event still on the deck or waiting for it. */
    held(): number {
      const all = [box.current(), ...box.queue()].filter((b): b is Burst => b !== null);
      return all.reduce((n, b) => n + b.events.length, 0);
    },
  };
}

// The two lines a "+N more" card shows under `fields`: the overflow card that 25 Super Chats
// end in, and a sub burst's own "+N more" after its first three cards.
function summaries(fields: Record<string, unknown>) {
  const a = alertbox(fields);
  for (let i = 0; i < 25; i++) {
    a.fire("superchat");
  }
  const fill = (b: Burst) => {
    const name = { textContent: "" };
    const msg = { textContent: "a peek's old line" };
    const el = { classList: { add() {} }, querySelector: (sel: string) => (sel === ".alert-name" ? name : msg) };
    a.box.fillSummary(el, b);
    return { name: name.textContent, msg: msg.textContent };
  };
  const burst: Burst = {
    group: "subs",
    summaryFrom: 3,
    events: Array.from({ length: 8 }, (_, i) => ({ type: "sub", id: "s" + i })),
  };
  return { overflow: fill(a.box.queue().at(-1)!), burst: fill(burst) };
}

describe("alertbox queue", () => {
  test("a follow bot outside the burst window cannot build a backlog", () => {
    const a = alertbox();
    for (let i = 0; i < 300; i++) {
      a.fire("follow"); // 2 s apart: every one misses the 1.5 s burst window
    }
    expect(a.box.queue().length).toBeLessThanOrEqual(3);
    expect(a.held()).toBe(300); // merged, not dropped
  });

  test("a mixed storm waits at most one burst per group past the cap, and loses nothing", () => {
    const a = alertbox();
    const types = ["follow", "cheer", "sub", "superchat", "raid", "resub", "member"];
    for (let i = 0; i < 700; i++) {
      a.fire(types[i % types.length]);
    }
    const grouped = a.box.queue().filter((b) => b.group !== null);
    // follow, subs (sub + resub) and members: the three groups, plus what waited before the cap.
    expect(grouped.length).toBeLessThanOrEqual(3 + 3);
    // cheer, superchat and raid: ten whole cards each, plus one overflow burst each.
    expect(a.box.queue().length - grouped.length).toBeLessThanOrEqual(3 * 11);
    expect(a.held()).toBe(700);
    const waiting = new Set(a.box.queue().flatMap((b) => b.events.map((e) => e.type)));
    for (const t of types) {
      expect(waiting.has(t) || a.box.current()?.events.some((e) => e.type === t)).toBe(true);
    }
  });

  test("100 one-bit cheers end in bounded time: ten whole cards, then one '+N more'", () => {
    const a = alertbox();
    for (let i = 0; i < 100; i++) {
      a.fire("cheer");
    }
    const q = a.box.queue();
    expect(q.length).toBe(11);
    expect(q.slice(0, 10).every((b) => b.events.length === 1 && b.summaryFrom < 0)).toBe(true);
    expect(q[10].overflow).toBe(true);
    expect(q[10].summaryFrom).toBe(0); // the whole burst is its "+N more" card
    expect(q[10].events.length).toBe(89); // 1 on the deck + 10 whole + 89 folded
    expect(a.held()).toBe(100);
  });

  test("an overflow card names the type it counts; a burst's own '+N more' does not", () => {
    const { overflow, burst } = summaries(DEFAULT_FIELDS);
    expect(overflow).toEqual({ name: "and 14 more!", msg: "Super Chats" }); // 1 on the deck + 10 whole + 14 folded
    expect(burst).toEqual({ name: "and 5 more!", msg: "" });
  });

  test("a localized overflow line takes its type's name from the user's field", () => {
    const { overflow } = summaries({
      ...DEFAULT_FIELDS,
      msgBurstMore: "und {count} weitere!",
      nounSuperchat: "Superchats",
    });
    expect(overflow).toEqual({ name: "und 14 weitere!", msg: "Superchats" });
  });

  test("{type} places the name in the line, once; a burst's card drops it", () => {
    const { overflow, burst } = summaries({ ...DEFAULT_FIELDS, msgBurstMore: "und {count} weitere {type}!" });
    expect(overflow).toEqual({ name: "und 14 weitere Super Chats!", msg: "" });
    expect(burst).toEqual({ name: "und 5 weitere!", msg: "" });
  });

  test("a name cleared on purpose shows nothing; a field the widget lacks shows the type", () => {
    expect(summaries({ ...DEFAULT_FIELDS, nounSuperchat: "" }).overflow.msg).toBe("");
    const { nounSuperchat: _, ...forked } = DEFAULT_FIELDS;
    expect(summaries(forked).overflow.msg).toBe("superchat");
  });

  test("every type has its message field, and every play-alone type its overflow name field", () => {
    const keys = new Set(SCHEMA.map((f) => f.key));
    for (const [type, t] of Object.entries(alertbox().box.TYPES)) {
      expect([type, keys.has(t.msg ?? "")]).toEqual([type, true]);
      if (t.alone) {
        expect([type, keys.has(t.alone)]).toEqual([type, true]);
        expect(t.group).toBeUndefined();
      }
    }
  });

  test("5 Super Chats stay individual cards", () => {
    const a = alertbox();
    for (let i = 0; i < 5; i++) {
      a.fire("superchat");
    }
    const all = [a.box.current(), ...a.box.queue()].filter((b): b is Burst => b !== null);
    expect(all.length).toBe(5);
    expect(all.every((b) => b.events.length === 1 && !b.overflow && b.summaryFrom < 0)).toBe(true);
  });

  test("a type's overflow shows one folded alert whole, not as '+1 more'", () => {
    const a = alertbox();
    for (let i = 0; i < 12; i++) {
      a.fire("raid");
    }
    const last = a.box.queue().at(-1)!;
    expect(last.overflow).toBe(true);
    expect(last.events.length).toBe(1);
    expect(last.summaryFrom).toBe(-1);
  });

  test("under their cap, tips and raids never merge: each keeps its own card", () => {
    const a = alertbox();
    const types = ["cheer", "superchat", "raid", "follow"];
    for (let i = 0; i < 40; i++) {
      a.fire(types[i % types.length]);
    }
    const alone = a.box.queue().filter((b) => b.group === null);
    expect(alone.length).toBe(30 - (a.box.current()?.group === null ? 1 : 0));
    for (const b of alone) {
      expect(b.events.length).toBe(1);
    }
    expect(a.held()).toBe(40);
  });

  test("under the cap, nothing merges outside the burst window", () => {
    const a = alertbox();
    // 10 s apart: past both the burst window and the deck burst's own join budget.
    a.fire("follow", 10_000);
    a.fire("follow", 10_000);
    a.fire("follow", 10_000);
    expect(a.box.queue().map((b) => b.events.length)).toEqual([1, 1]);
  });
});
