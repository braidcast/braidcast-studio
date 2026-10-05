import { describe, expect, test } from "bun:test";

// The default end credits, loaded as source against stand-ins for the globals it touches.
// The roll itself (requestAnimationFrame, layout) is inert here; what is tested is who is
// thanked, under which heading.

const SOURCE = await Bun.file(new URL("../public/overlay/default-endcredits/template.js", import.meta.url)).text();

class El {
  #text = "";
  hidden = false;
  className = "";
  clientHeight = 720;
  offsetHeight = 400;
  children: El[] = [];
  style = { transform: "" };
  get textContent() {
    return this.#text;
  }
  set textContent(v: string) {
    this.#text = v;
    this.children = [];
  }
  appendChild(c: El) {
    this.children.push(c);
    return c;
  }
}

function credits(fields: Record<string, unknown> = {}, preview = false) {
  const ids: Record<string, El> = {};
  const document = {
    getElementById: (id: string) => (ids[id] ??= new El()),
    documentElement: { style: { setProperty() {} } },
    createElement: () => new El(),
  };
  const handlers: Record<string, (x: unknown) => void> = {};
  const on = (name: string) => (fn: (x: unknown) => void) => (handlers[name] = fn);
  const overlay = { preview, onLoad: on("load"), onEvent: on("event"), onBackfill: on("backfill") };
  const window = { matchMedia: () => ({ matches: true }) }; // reduced motion: no frame loop
  new Function("document", "OBSOverlay", "window", "getComputedStyle", "requestAnimationFrame", SOURCE)(
    document,
    overlay,
    window,
    () => ({ fontSize: "16px" }),
    () => 0,
  );
  handlers.load({ fields });
  /** "Heading: a, b" per section. */
  const read = () =>
    ids.sections.children.map((s) => `${s.children[0].textContent}: ${s.children[1].children.map((li) => li.textContent).join(", ")}`);
  return { read, ids, fire: (e: unknown) => handlers.event(e), backfill: (l: unknown[]) => handlers.backfill(l) };
}

const ev = (id: string, type: string, actorName: string, extra: Record<string, unknown> = {}) => ({
  id,
  type,
  platform: "twitch",
  ts: 0,
  actorName,
  ...extra,
});

describe("end credits", () => {
  test("thanks each supporter once per section, in section order, from the backfill and live", () => {
    const c = credits();
    c.backfill([ev("a", "follow", "Ann"), ev("b", "sub", "Bo"), ev("c", "resub", "bo"), ev("d", "subgift", "Cy")]);
    c.fire(ev("e", "superchat", "Di", { amount: 500, currency: "USD" }));
    c.fire(ev("a", "follow", "Ann")); // already counted from the backfill
    c.fire(ev("f", "follow", "Ed"));
    expect(c.read()).toEqual(["Subscribers: Bo, Cy", "Super Chats: Di", "New followers: Ann, Ed"]);
    expect(c.ids.title.textContent).toBe("Thanks for watching!");
  });

  test("a switched-off section is left out; replays and on-stream tests thank no one", () => {
    const c = credits({ showFollows: false });
    c.fire(ev("a", "follow", "Ann"));
    c.fire(ev("b", "raid", "Bo"));
    c.fire(ev("b2", "raid", "Cy", { replay: true }));
    c.fire(ev("t", "cheer", "Tester", { test: true }));
    expect(c.read()).toEqual(["Raiders: Bo"]);
    const p = credits({}, true);
    p.fire(ev("t", "cheer", "Tester", { test: true }));
    expect(p.read()).toEqual(["Cheers: Tester"]);
  });

  test("the empty text shows when no one is there to thank", () => {
    const c = credits({ emptyText: "See you next time" });
    expect([c.ids.empty.textContent, c.ids.empty.hidden]).toEqual(["See you next time", false]);
    c.fire(ev("a", "follow", "Ann"));
    expect(c.ids.empty.hidden).toBe(true);
  });
});
