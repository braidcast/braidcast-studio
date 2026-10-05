import { afterEach, beforeEach, describe, expect, mock, test } from "bun:test";

// The store reads its gate through the bridge; the test answers diagnostics.get by hand.
let answer: (v: { debug: boolean; logPath: string; devToolsPort: number }) => void = () => {};
let fail: (e: Error) => void = () => {};
const listeners: Record<string, (p: unknown) => void> = {};
mock.module("$lib/api/bridge", () => ({
  obs: {
    call: () =>
      new Promise((resolve, reject) => {
        answer = resolve;
        fail = reject;
      }),
    on: (ev: string, fn: (p: unknown) => void) => {
      listeners[ev] = fn;
      return () => {};
    },
  },
}));

const lines: unknown[][] = [];
const realDebug = console.debug;
beforeEach(() => {
  lines.length = 0;
  console.debug = (...a: unknown[]) => void lines.push(a);
});
afterEach(() => {
  console.debug = realDebug;
});

const settle = () => new Promise((r) => setTimeout(r, 0));

describe("log.dbg before the debug gate is read", () => {
  test("held lines are written in order once the gate reads on, and later lines go straight out", async () => {
    const { log } = await import("../src/lib/utils/log");
    const { diagnosticsStore } = await import("../src/lib/stores/diagnosticsStore.svelte");
    log.dbg("lifecycle", "boot", 1);
    log.dbg("preview", "gate");
    expect(lines).toEqual([]);
    diagnosticsStore.start();
    answer({ debug: true, logPath: "", devToolsPort: 0 });
    await settle();
    expect(lines).toEqual([
      ["[D][lifecycle]", "boot", 1],
      ["[D][preview]", "gate"],
    ]);
    log.dbg("canvas", "after");
    expect(lines.at(-1)).toEqual(["[D][canvas]", "after"]);
  });

  test("the gate turning off later stops the lines", async () => {
    const { log } = await import("../src/lib/utils/log");
    listeners["debug.changed"]?.({ debug: false });
    log.dbg("canvas", "quiet");
    expect(lines).toEqual([]);
  });
});
