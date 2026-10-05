import { describe, expect, mock, test } from "bun:test";

mock.module("$lib/api/bridge", () => ({ obs: { call: async () => null, on: () => () => {} } }));
mock.module("$lib/dialogs/logViewerOpener.svelte", () => ({ openLogViewer: () => {} }));
mock.module("$lib/stores/pageStore.svelte", () => ({ setPage: () => {} }));
const { evaluate } = await import("../src/lib/monitor/advisorRules");
type Snapshot = Parameters<typeof evaluate>[0];

const RULE = "browser.fpsAboveCanvas";

function snapshot(over: Partial<Snapshot> = {}): Snapshot {
  return {
    scenes: [],
    sources: new Map([
      ["Alerts", "browser_source"],
      ["Chat", "browser_source"],
    ]),
    settings: new Map([
      ["Alerts", { fps_custom: true, fps: 60, shutdown: true }],
      ["Chat", { fps_custom: false, fps: 120, shutdown: true }],
    ]),
    pageErrors: new Map(),
    onAir: new Set(["Alerts", "Chat"]),
    placed: new Set(["Alerts", "Chat"]),
    consumers: new Map([
      ["Alerts", new Set([""])],
      ["Chat", new Set([""])],
    ]),
    canvasFps: new Map([
      ["", 30],
      ["vert", 60],
    ]),
    onAirGaps: [],
    graphGaps: [],
    settingsGaps: [],
    logGaps: [],
    ...over,
  };
}

const rows = (s: Snapshot) => evaluate(s).rows.filter((r) => r.rule.id === RULE);

describe("Advisor: a browser source painting faster than its canvas", () => {
  test("a custom 60 fps page shown only on a 30 fps canvas is waste, even with a 60 fps canvas elsewhere", () => {
    const found = rows(snapshot());
    expect(found.map((r) => r.finding.title)).toEqual(["Alerts"]);
    expect(found[0].finding.cost).toContain("the canvas showing it runs at 30");
  });

  test("the canvas rate is fine when the page is also shown on a canvas that fast", () => {
    expect(rows(snapshot({ consumers: new Map([["Alerts", new Set(["", "vert"])]]) }))).toEqual([]);
  });

  test("with the graph incomplete it compares against every canvas, so it never accuses on a guess", () => {
    expect(rows(snapshot({ graphGaps: ["x"] }))).toEqual([]);
    const fast = snapshot({
      graphGaps: ["x"],
      settings: new Map([["Alerts", { fps_custom: true, fps: 144 }]]),
    });
    const found = rows(fast);
    expect(found.map((r) => r.finding.title)).toEqual(["Alerts"]);
    expect(found[0].finding.cost).toContain("no canvas runs faster than 60");
  });

  test("60 against a 59.94 canvas is not a finding", () => {
    expect(rows(snapshot({ canvasFps: new Map([["", 60000 / 1001]]) }))).toEqual([]);
  });

  test("a page in no scene is left to the unplaced rule", () => {
    expect(rows(snapshot({ consumers: new Map() }))).toEqual([]);
  });

  test("no canvas list means the rule says it could not run", () => {
    const result = evaluate(snapshot({ canvasFps: new Map() }));
    expect(result.skipped.find((k) => k.rule.id === RULE)?.reason).toBe("the canvas list could not be read");
  });

  test("a browser source whose settings were not read is reported as a shortfall", () => {
    const result = evaluate(
      snapshot({ settings: new Map([["Chat", { fps_custom: false }]]), settingsGaps: ["a source's settings could not be read"] }),
    );
    expect(result.skipped.find((k) => k.rule.id === RULE)?.partial).toEqual({ unread: 1 });
  });
});
