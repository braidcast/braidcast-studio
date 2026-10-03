import { afterAll, afterEach, describe, expect, spyOn, test } from "bun:test";
import { amountOf, MONEY_TYPES } from "../src/overlay/eventAmount";
import { fillTemplate, fillTemplateParts } from "../src/overlay/fillTemplate";
import { logOnce } from "../src/overlay/logOnce";

describe("fillTemplateParts", () => {
  const values = { name: "Ana", amountText: "1,000 bits", amount: "", message: "{name}" };

  test("joining the parts gives exactly what fillTemplate gives", () => {
    const cases: [string, Record<string, unknown>][] = [
      ["{name} cheered {amountText}!", values],
      ["  {name} sent {amount}!  ", values],
      ["{unknown} stays, {name} fills", values],
      ["{message} is shown as typed", values],
      ["", values],
      ["{amount}", values],
      ["  {name}  ", { name: " a " }],
      ["{name} {name}", { name: "{name}" }],
      ["{a}{b}", { a: "", b: "" }],
    ];
    for (const [text, vals] of cases) {
      expect(fillTemplateParts(text, vals).map((p) => p.text).join("")).toBe(fillTemplate(text, vals));
    }
  });

  test("each filled-in value is its own run, marked with its variable", () => {
    expect(fillTemplateParts("{name} cheered {amountText}!", values)).toEqual([
      { text: "Ana", key: "name" },
      { text: " cheered ", key: null },
      { text: "1,000 bits", key: "amountText" },
      { text: "!", key: null },
    ]);
  });

  test("an unknown token stays literal text and a value is never re-expanded", () => {
    expect(fillTemplateParts("{nope} {message}", values)).toEqual([
      { text: "{nope}", key: null },
      { text: " ", key: null },
      { text: "{name}", key: "message" },
    ]);
  });

  test("an empty value leaves with its spaces and the ends are trimmed", () => {
    expect(fillTemplateParts("  {name} sent {amount}!  ", values)).toEqual([
      { text: "Ana", key: "name" },
      { text: " sent!", key: null },
    ]);
  });
});

describe("amountOf", () => {
  test("a tally with no amount is zero; anything else with none has none", () => {
    expect(amountOf({ type: "raid", amount: undefined })).toBe(0);
    expect(amountOf({ type: "follow", amount: undefined })).toBeNull();
    expect(amountOf({ type: "cheer", amount: 500 })).toBe(500);
  });

  test("only Super Chats and Super Stickers are money", () => {
    expect([...MONEY_TYPES].sort()).toEqual(["superchat", "supersticker"]);
  });
});

describe("logOnce", () => {
  const spy = spyOn(console, "error").mockImplementation(() => {});
  afterEach(() => spy.mockClear());
  afterAll(() => spy.mockRestore());

  test("one line per key, prefixed so the session log names the overlay", () => {
    logOnce("parts-test|a", "first notice");
    logOnce("parts-test|a", "first notice again");
    logOnce("parts-test|b", "second notice");
    expect(spy.mock.calls).toEqual([["OBSOverlay first notice"], ["OBSOverlay second notice"]]);
  });

  test("past its bound the oldest key is forgotten and may log again", () => {
    logOnce("parts-test|evict-0", "oldest");
    for (let i = 1; i <= 40; i++) {
      logOnce(`parts-test|evict-${i}`, `notice ${i}`);
    }
    spy.mockClear();
    logOnce("parts-test|evict-0", "oldest again");
    logOnce("parts-test|evict-40", "newest again");
    expect(spy.mock.calls).toEqual([["OBSOverlay oldest again"]]);
  });
});
