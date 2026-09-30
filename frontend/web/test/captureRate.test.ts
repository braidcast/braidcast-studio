import { describe, expect, test } from "bun:test";
import type { CaptureRateRow } from "$lib/api/bridge";
import { describeCapture, fmtRate, fmtSinceReset } from "$lib/utils/captureRate";

function row(over: Partial<CaptureRateRow>): CaptureRateRow {
  return {
    uuid: "u",
    name: "Display",
    kind: "wgc",
    status: "ok",
    refFps: 60,
    rate: null,
    fraction: null,
    inputFps: null,
    renderedFps: null,
    below: false,
    lockedFraction: null,
    note: null,
    inGrace: false,
    sinceReset: { liveSec: 0, belowSec: 0, lockedSec: 0 },
    ...over,
  };
}

describe("describeCapture", () => {
  test("rates read to one decimal like the session log line", () => {
    expect(fmtRate(30)).toBe("30.0/s");
    expect(fmtRate(29.94)).toBe("29.9/s");
  });

  test("display capture at half the canvas is a neutral note, never a warning", () => {
    const v = describeCapture(
      row({ rate: 30, fraction: 0.5, lockedFraction: "1/2", note: "delivering 1/2 of canvas rate" }),
    );
    expect(v.value).toBe("30.0/s");
    expect(v.share).toBe("50% of canvas rate");
    expect(v.note).toBe("delivering 1/2 of canvas rate");
    expect(v.warn).toBeNull();
    expect(v.tone).toBe("ok");
  });

  test("display capture off air shows the rate only", () => {
    const v = describeCapture(row({ kind: "dxgi", refFps: null, rate: 12.34 }));
    expect(v.value).toBe("12.3/s");
    expect(v.share).toBeNull();
    expect(v.ref).toBeNull();
  });

  test("a row without its first delta reads as measuring", () => {
    expect(describeCapture(row({ inGrace: true })).value).toBe("Measuring…");
    expect(describeCapture(row({ kind: "async" })).value).toBe("Measuring…");
  });

  test("async below warns in words, and not otherwise", () => {
    const ok = describeCapture(row({ kind: "async", inputFps: 30, renderedFps: 29.9 }));
    expect(ok.value).toBe("in 30.0/s · out 29.9/s");
    expect(ok.warn).toBeNull();
    const below = describeCapture(row({ kind: "async", inputFps: 60, renderedFps: 30, below: true }));
    expect(below.tone).toBe("warn");
    expect(below.warn).not.toBeNull();
    expect(below.label).toContain("Warning");
  });

  test("idle and unmeasurable read differently", () => {
    const idle = describeCapture(row({ status: "idle" }));
    expect(idle.value).toBe("—");
    expect(idle.label).toContain("not capturing");
    const bitblt = describeCapture(row({ kind: "none", status: "unmeasurable" }));
    expect(bitblt.value).toBe("Unmeasurable");
    expect(bitblt.kind).toBe("");
    const game = describeCapture(row({ kind: "gameHook", status: "unmeasurable" }));
    expect(game.value).toBe("Unmeasurable");
    expect(game.note).toContain("game capture");
  });
});

describe("fmtSinceReset", () => {
  test("empty window reads as a dash", () => {
    expect(fmtSinceReset(row({}))).toBe("—");
  });

  test("names the locked and below shares of the measured time", () => {
    expect(fmtSinceReset(row({ sinceReset: { liveSec: 90, lockedSec: 45, belowSec: 0 } }))).toBe("1:30 · locked 50%");
    expect(fmtSinceReset(row({ sinceReset: { liveSec: 1000, lockedSec: 0, belowSec: 4 } }))).toBe("16:40 · below <1%");
  });
});
