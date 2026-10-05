import { describe, expect, test } from "bun:test";
import type { NormalizedEvent } from "../src/lib/api/bridge";
import { eventEmoji, eventSummary } from "../src/overlay/eventSummary";

// The default event list, loaded as source against stand-ins for the globals it touches.

const SOURCE = await Bun.file(new URL("../public/overlay/default-eventlist/template.js", import.meta.url)).text();

class El {
  #text = "";
  hidden = false;
  className = "";
  children: El[] = [];
  style = { animation: "", setProperty() {} };
  classList = { toggle() {} };
  // As in the DOM, setting the text replaces every child.
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

function eventList(fields: Record<string, unknown> = {}, preview = false) {
  const ids: Record<string, El> = {};
  const document = {
    getElementById: (id: string) => (ids[id] ??= new El()),
    documentElement: { style: { setProperty() {} } },
    createElement: () => new El(),
  };
  const handlers: Record<string, (x: unknown) => void> = {};
  const on = (name: string) => (fn: (x: unknown) => void) => (handlers[name] = fn);
  const fmt = { amount: () => "$5.00", amountText: () => "500 bits" };
  const overlay = {
    preview,
    onLoad: on("load"),
    onEvent: on("event"),
    onBackfill: on("backfill"),
    summarize: (e: NormalizedEvent) => eventSummary(e, fmt),
    eventEmoji,
  };
  new Function("document", "OBSOverlay", SOURCE)(document, overlay);
  handlers.load({ fields });
  /** Each row as "name what". */
  const rows = () => ids.rows.children.map((r) => r.children.filter((c) => c.className !== "emoji").map((c) => c.textContent).join(" "));
  return { rows, empty: ids.empty, fire: (e: unknown) => handlers.event(e), backfill: (l: unknown[]) => handlers.backfill(l) };
}

const ev = (id: string, type: string, actorName = "Ann", extra: Record<string, unknown> = {}) => ({
  id,
  type,
  platform: "twitch",
  ts: 0,
  actorName,
  ...extra,
});

describe("event list", () => {
  test("lists the newest events first, capped, and rebuilds from the backfill after a reload", () => {
    const l = eventList({ maxItems: 2 });
    l.backfill([ev("a", "follow", "Ann"), ev("b", "cheer", "Bo"), ev("c", "raid", "Cy")]);
    expect(l.rows()).toEqual(["Cy raided with 500 bits", "Bo cheered 500 bits"]);
    l.fire(ev("c", "raid", "Cy")); // already listed from the backfill
    l.fire(ev("d", "sub", "Di"));
    expect(l.rows()).toEqual(["Di subscribed", "Cy raided with 500 bits"]);
  });

  test("oldest first when asked, and only the event types switched on", () => {
    const l = eventList({ newestOnTop: false, showRaids: false });
    l.fire(ev("a", "follow", "Ann"));
    l.fire(ev("b", "raid", "Bo"));
    l.fire(ev("c", "subgift", "Cy", { count: 5 }));
    expect(l.rows()).toEqual(["Ann followed", "Cy gifted 5 subs"]);
  });

  test("replays never list twice; test frames list only in the editor preview", () => {
    const onStream = eventList();
    onStream.fire(ev("a", "follow"));
    onStream.fire(ev("a2", "follow", "Ann", { replay: true }));
    onStream.fire(ev("t", "follow", "Tester", { test: true }));
    expect(onStream.rows()).toEqual(["Ann followed"]);
    const preview = eventList({}, true);
    preview.fire(ev("t", "follow", "Tester", { test: true }));
    expect(preview.rows()).toEqual(["Tester followed"]);
  });

  test("the empty text shows until the first event", () => {
    const l = eventList({ emptyText: "No events yet" });
    expect([l.empty.textContent, l.empty.hidden]).toEqual(["No events yet", false]);
    l.fire(ev("a", "follow"));
    expect(l.empty.hidden).toBe(true);
  });
});
