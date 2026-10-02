import { describe, expect, test } from "bun:test";
import { fillTemplate } from "../src/overlay/fillTemplate";

// The default alert box and event ticker are bundler-free overlay scripts, run here as source
// against stand-ins for the globals they touch (the alertboxQueue.test.ts pattern). The host
// sends an `eventredaction` frame naming the events a moderator's removal took the viewer's
// words from; after it, neither widget may show those words, on screen or still to come.

type Redacts = (e: { id: string }) => boolean;
type RedactionFn = (ids: string[], redacts: Redacts) => void;

const redaction = (ids: string[]): [string[], Redacts] => {
  const gone = new Set(ids);
  return [ids, (e) => gone.has(e.id)];
};

// An element that keeps what the scripts read back: its classes, its children (so the deck
// knows which cards stand) and one stable child per selector for the card's lines.
class El {
  dataset: Record<string, string> = {};
  classes = new Set<string>();
  style = { setProperty() {} } as Record<string, unknown> & { setProperty(): void };
  textContent = "";
  className = "";
  offsetWidth = 0;
  offsetHeight = 0;
  children: El[] = [];
  parent: El | null = null;
  private parts = new Map<string, El>();
  content = {
    firstElementChild: {
      cloneNode: () => {
        const card = new El();
        card.classes.add("alert");
        return card;
      },
    },
  };
  classList = {
    add: (...names: string[]) => names.forEach((n) => this.classes.add(n)),
    remove: (...names: string[]) => names.forEach((n) => this.classes.delete(n)),
    toggle: (name: string, on?: boolean) => {
      if (on ?? !this.classes.has(name)) this.classes.add(name);
      else this.classes.delete(name);
    },
  };
  querySelectorAll(sel: string): El[] {
    const alerts = this.children.filter((c) => c.classes.has("alert"));
    if (sel === ".alert") return alerts;
    if (sel === ".alert:not(.exiting)") return alerts.filter((c) => !c.classes.has("exiting"));
    if (sel === ".alert.exiting") return alerts.filter((c) => c.classes.has("exiting"));
    if (sel === ".tick") return this.children.filter((c) => c.className === "tick");
    return [];
  }
  querySelector(sel: string): El | null {
    if (sel.startsWith(".alert") && !sel.startsWith(".alert-")) return this.querySelectorAll(sel)[0] ?? null;
    if (sel === ".body") return this.find((c) => c.className === "body");
    let part = this.parts.get(sel);
    if (!part) {
      part = new El();
      this.parts.set(sel, part);
    }
    return part;
  }
  find(pick: (c: El) => boolean): El | null {
    for (const c of this.children) {
      if (pick(c)) return c;
      const deeper = c.find(pick);
      if (deeper) return deeper;
    }
    return null;
  }
  appendChild(c: El): El {
    c.parent = this;
    this.children.push(c);
    return c;
  }
  insertBefore(c: El): El {
    return this.appendChild(c);
  }
  remove(): void {
    if (this.parent) this.parent.children = this.parent.children.filter((c) => c !== this);
    this.parent = null;
  }
  addEventListener() {}
  /** Every text this element and its descendants show. */
  text(): string {
    return [this.textContent, ...[...this.parts.values()].map((p) => p.text()), ...this.children.map((c) => c.text())].join(" ");
  }
}

function alertbox() {
  const ids: Record<string, El> = {};
  const document = {
    getElementById: (id: string) => (ids[id] ??= new El()),
    documentElement: new El(),
    body: new El(),
  };
  let onEvent: (e: unknown) => void = () => {};
  let onRedaction: RedactionFn = () => {};
  const overlay = {
    // {message} is in no default template, so these put it where a user would.
    fields: { msgSuperchat: "{name} sent {amount}: {message}", msgSub: "{name} subscribed: {message}" },
    onLoad() {},
    onEvent: (fn: typeof onEvent) => (onEvent = fn),
    onEventRedaction: (fn: RedactionFn) => (onRedaction = fn),
    formatAmount: () => "$5",
    formatAmountText: () => "$5",
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
    SOURCES.alertbox + "\n;return { queue: () => queue, current: () => current };",
  );
  const box = run(document, overlay, { now: () => 0 }, () => 0, () => {}) as {
    queue: () => { events: { id: string; message?: string }[] }[];
    current: () => { events: { id: string; message?: string }[]; leaving?: boolean } | null;
  };
  return {
    box,
    deck: () => ids["deck"],
    live: () => ids["alert-live"],
    fire: (e: Record<string, unknown>) => onEvent(e),
    redact: (gone: string[]) => onRedaction(...redaction(gone)),
  };
}

function ticker() {
  const ids: Record<string, El> = {};
  const document = {
    getElementById: (id: string) => (ids[id] ??= new El()),
    documentElement: new El(),
    createElement: () => new El(),
  };
  let onEvent: (e: unknown) => void = () => {};
  let onRedaction: RedactionFn = () => {};
  const overlay = {
    onLoad() {},
    onEvent: (fn: typeof onEvent) => (onEvent = fn),
    onEventRedaction: (fn: RedactionFn) => (onRedaction = fn),
    formatAmountText: () => "$5",
    formatMoney: () => "$5",
  };
  const run = new Function(
    "document",
    "OBSOverlay",
    "window",
    "getComputedStyle",
    "requestAnimationFrame",
    SOURCES.ticker + "\n;return { buffer: () => buffer, makeTick };",
  );
  const belt = run(
    document,
    overlay,
    { addEventListener() {} },
    () => ({ getPropertyValue: () => "", fontSize: "16" }),
    () => 0,
  ) as { buffer: () => { id: string; body: string; summary: string }[]; makeTick: (item: unknown) => El };
  return {
    belt,
    track: () => ids["track"],
    fire: (e: Record<string, unknown>) => onEvent(e),
    redact: (gone: string[]) => onRedaction(...redaction(gone)),
  };
}

const SOURCES = {
  alertbox: await Bun.file(new URL("../public/overlay/default-alertbox/template.js", import.meta.url)).text(),
  ticker: await Bun.file(new URL("../public/overlay/default-ticker/template.js", import.meta.url)).text(),
};

const superchat = (id: string, message: string) => ({ type: "superchat", id, actorName: "viewer", message });

describe("event redaction in the default alert box", () => {
  test("the card on the deck is drawn again without the removed words", () => {
    const a = alertbox();
    a.fire(superchat("sc-1", "removed words"));
    expect(a.deck().text()).toContain("removed words");
    expect(a.live().textContent).toContain("removed words");
    a.redact(["sc-1"]);
    expect(a.deck().text()).not.toContain("removed words");
    expect(a.deck().text()).toContain("viewer");
    expect(a.live().textContent).not.toContain("removed words");
    expect(a.box.current()?.events[0].message).toBeUndefined();
  });

  test("a waiting alert loses its words; the others keep theirs", () => {
    const a = alertbox();
    a.fire(superchat("sc-1", "first words"));
    a.fire(superchat("sc-2", "removed words"));
    a.fire(superchat("sc-3", "kept words"));
    a.redact(["sc-2"]);
    const waiting = a.box.queue().flatMap((b) => b.events);
    expect(waiting.find((e) => e.id === "sc-2")?.message).toBeUndefined();
    expect(waiting.find((e) => e.id === "sc-3")?.message).toBe("kept words");
    expect(a.box.current()?.events[0].message).toBe("first words");
    expect(a.deck().text()).toContain("first words");
  });

  test("a card sliding off with the removed words goes at once; the others finish their exit", () => {
    const a = alertbox();
    a.fire({ type: "sub", id: "s-1", actorName: "viewer", message: "removed words" });
    a.fire({ type: "sub", id: "s-2", actorName: "other", message: "kept" });
    const exiting = (key: string, text: string) => {
      const el = new El();
      el.classes.add("alert");
      el.classes.add("exiting");
      el.dataset.key = key;
      el.textContent = text;
      return a.deck().appendChild(el);
    };
    const removed = exiting("e0", "removed words");
    const other = exiting("e1", "kept");
    a.redact(["s-1"]);
    expect(a.deck().children).not.toContain(removed);
    expect(a.deck().children).toContain(other);
    expect(a.deck().text()).not.toContain("removed words");
  });

  test("a deck already leaving is cleared, the live region with it", () => {
    const a = alertbox();
    a.fire(superchat("sc-1", "removed words"));
    a.box.current()!.leaving = true;
    a.redact(["sc-1"]);
    expect(a.deck().text()).not.toContain("removed words");
    expect(a.live().textContent).not.toContain("removed words");
  });

  test("ids it does not hold change nothing", () => {
    const a = alertbox();
    a.fire(superchat("sc-1", "kept words"));
    a.redact(["elsewhere"]);
    expect(a.deck().text()).toContain("kept words");
  });
});

describe("event redaction in the default ticker", () => {
  test("the belt's buffer and the ticks already drawn drop the removed words", () => {
    const t = ticker();
    t.fire(superchat("sc-1", "removed words"));
    t.fire(superchat("sc-2", "kept words"));
    for (const item of t.belt.buffer()) t.track().appendChild(t.belt.makeTick(item));
    expect(t.track().text()).toContain("removed words");
    t.redact(["sc-1"]);
    expect(t.belt.buffer().map((i) => i.body)).toEqual(["", "kept words"]);
    expect(t.track().text()).not.toContain("removed words");
    expect(t.track().text()).toContain("kept words");
    // Emptied in place at its width, so the ticks behind it keep their place on the belt.
    const body = t.track().children[0].find((c) => c.className === "body");
    expect(body?.style.width).toBe("0px");
    // The loop draws from the buffer again, still without them.
    expect(t.belt.makeTick(t.belt.buffer()[0]).text()).not.toContain("removed words");
  });
});

describe("follow wording in the default ticker", () => {
  test("a YouTube follow reads subscribed; other platforms read followed", () => {
    const t = ticker();
    for (const platform of ["youtube", "twitch", "kick"]) {
      t.fire({ id: platform, type: "follow", platform, actorName: "Ann" });
    }
    expect(t.belt.buffer().map((i) => i.summary)).toEqual(["subscribed", "followed", "followed"]);
  });
});
