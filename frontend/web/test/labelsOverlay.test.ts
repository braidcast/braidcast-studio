import { describe, expect, test } from "bun:test";
import { COUNTER_EVENT_SOURCES } from "../src/overlay/counter";
import { fillTemplate } from "../src/overlay/fillTemplate";

// The default label widget, loaded as source against stand-ins for the globals it touches.

const SOURCE = await Bun.file(new URL("../public/overlay/default-labels/template.js", import.meta.url)).text();

function label(fields: Record<string, unknown>) {
  const ids: Record<string, { textContent: string }> = {};
  const document = {
    getElementById: (id: string) => (ids[id] ??= { textContent: "" }),
    documentElement: { style: { setProperty() {} } },
  };
  let onLoad: (ctx: { fields: Record<string, unknown> }) => void = () => {};
  let onEvent: (e: unknown) => void = () => {};
  const overlay = {
    onLoad: (fn: typeof onLoad) => (onLoad = fn),
    onEvent: (fn: typeof onEvent) => (onEvent = fn),
    counter: { sources: COUNTER_EVENT_SOURCES },
    formatAmount: () => "",
    formatCount: (n: number) => String(n),
    fillTemplate,
    textField: (f: Record<string, unknown>, k: string, d: string) => (f[k] != null ? String(f[k]) : d),
  };
  new Function("document", "OBSOverlay", SOURCE)(document, overlay);
  onLoad({ fields });
  return { text: () => ids["label-text"].textContent, fire: (e: Record<string, unknown>) => onEvent(e) };
}

describe("label event types come from the Counter's sources", () => {
  test("a subscriber label shows subs and resubs, not gift subs", () => {
    const l = label({ eventType: "subscriber", mode: "latest", format: "{name}" });
    l.fire({ id: "1", type: "subgift", actorName: "Gifter" });
    l.fire({ id: "2", type: "resub", actorName: "Ann" });
    expect(l.text()).toBe("Ann");
  });

  test("an unknown or inherited event type falls back to followers", () => {
    const l = label({ eventType: "constructor", mode: "latest", format: "{name}" });
    l.fire({ id: "1", type: "follow", actorName: "Bo" });
    expect(l.text()).toBe("Bo");
  });
});
