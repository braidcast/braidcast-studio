import { describe, expect, test } from "bun:test";
import { fillTemplateParts } from "../src/overlay/fillTemplate";
import { splitGraphemes, textUnits } from "../src/overlay/animation/textFx";

describe("grapheme splitting", () => {
  test("emoji, ZWJ sequences, flags and combining marks stay whole", () => {
    const family = "\u{1F468}‍\u{1F469}‍\u{1F467}";
    const flag = "\u{1F1EF}\u{1F1F5}";
    const thumbs = "\u{1F44D}\u{1F3FD}";
    const eAcute = "é";
    expect(splitGraphemes(`a${family}${flag}${thumbs}${eAcute}`)).toEqual(["a", family, flag, thumbs, eAcute]);
  });

  test("text units join back to the line exactly", () => {
    const parts = fillTemplateParts("{name} cheered {amountText}! \u{1F389}", { name: "Zoë", amountText: "1,000 bits" });
    for (const target of ["all", "vars"] as const) {
      expect(textUnits(parts, target).map((u) => u.text).join("")).toBe("Zoë cheered 1,000 bits! \u{1F389}");
    }
  });

  test("'vars' animates only the filled-in variables, and spaces never animate", () => {
    const parts = fillTemplateParts("{name} sent {amount}", { name: "ab", amount: "$5" });
    const units = textUnits(parts, "vars");
    expect(units.filter((u) => u.animate).map((u) => u.text)).toEqual(["a", "b", "$", "5"]);
    expect(textUnits(fillTemplateParts("a b", {}), "all")).toEqual([
      { text: "a", animate: true },
      { text: " ", animate: false },
      { text: "b", animate: true },
    ]);
  });
});
