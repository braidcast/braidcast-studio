import { describe, expect, test } from "bun:test";

// Widgets that keep a channel's last frame as their state, loaded as source against
// stand-ins for the globals they touch. A frame the editor's Test button sent reaches the
// browser source on stream too; only the preview may keep what it carries.

const source = (type: string) =>
  Bun.file(new URL(`../public/overlay/default-${type}/template.js`, import.meta.url)).text();
const SOURCES = { viewercount: await source("viewercount"), uptime: await source("uptime") };

class El {
  textContent = "";
  hidden = false;
  className = "";
  children: El[] = [];
  style = { background: "", setProperty() {} };
  get childElementCount() {
    return this.children.length;
  }
  appendChild(c: El) {
    this.children.push(c);
    return c;
  }
  /** Every text this element and its children carry, in order. */
  text(): string {
    return [this.textContent, ...this.children.map((c) => c.text())].join("");
  }
}

function load(type: keyof typeof SOURCES, preview: boolean) {
  const ids: Record<string, El> = {};
  const document = {
    getElementById: (id: string) => (ids[id] ??= new El()),
    documentElement: new El(),
    createElement: () => new El(),
  };
  const handlers: Record<string, (x: unknown) => void> = {};
  const on = (name: string) => (fn: (x: unknown) => void) => (handlers[name] = fn);
  const overlay = {
    preview,
    onLoad: on("load"),
    onViewers: on("viewers"),
    onStream: on("stream"),
  };
  new Function("document", "OBSOverlay", "setInterval", "clearInterval", SOURCES[type])(
    document,
    overlay,
    () => 1,
    () => {},
  );
  handlers.load({ fields: {} });
  return { ids, fire: (name: string, frame: unknown) => handlers[name](frame) };
}

const viewers = (n: number, test = false) => ({
  total: n,
  perAccount: { "twitch:1": n },
  perPlatform: { twitch: n },
  ...(test ? { test: true } : {}),
});

describe("test frames stay off stream", () => {
  test("a viewer count keeps the real figure when a test frame arrives on stream", () => {
    const w = load("viewercount", false);
    w.fire("viewers", viewers(12));
    w.fire("viewers", viewers(9999, true));
    expect(w.ids.chips.text()).toContain("12");
    expect(w.ids.chips.text()).not.toContain("9,999");
    // Nor does a test end-of-stream frame clear it.
    w.fire("stream", { active: false, startedAt: null, destinations: [], test: true });
    expect(w.ids.chips.text()).toContain("12");
  });

  test("the editor preview shows the test figure", () => {
    const w = load("viewercount", true);
    w.fire("viewers", viewers(12));
    w.fire("viewers", viewers(9999, true));
    expect(w.ids.chips.text()).toContain("9,999");
  });

  test("uptime on stream ignores a test broadcast state", () => {
    const w = load("uptime", false);
    w.fire("stream", { active: true, startedAt: Date.now() - 3_600_000, destinations: [], test: true });
    expect(w.ids.value.textContent).not.toMatch(/1:00:0\d/);
    const p = load("uptime", true);
    p.fire("stream", { active: true, startedAt: Date.now() - 3_600_000, destinations: [], test: true });
    expect(p.ids.value.textContent).toMatch(/1:00:0\d/);
  });
});
