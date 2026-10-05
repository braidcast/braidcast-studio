import { describe, expect, test } from "bun:test";
import type { FxSnapshot } from "../src/lib/api/bridge";
import { automaticLabel, currencyChoices, fxStatusText } from "../src/lib/settings/fxSettings";

const fx = (over: Partial<FxSnapshot> = {}): FxSnapshot => ({
  home: "INR",
  homeSource: "youtube",
  date: "2026-10-02",
  fetchedAt: 1,
  stale: false,
  rates: { EUR: 1, USD: 1.08, INR: 90.4 },
  ...over,
});

describe("Super Chat currency setting", () => {
  test("automatic names the currency it resolved to and where from", () => {
    expect(automaticLabel(fx(), "")).toBe("Automatic (INR, from your YouTube channel's country)");
    expect(automaticLabel(fx({ home: "GBP", homeSource: "locale" }), "")).toBe("Automatic (GBP, from your Windows region)");
    expect(automaticLabel(fx({ home: "", homeSource: "none" }), "")).toBe("Automatic");
    // With a currency chosen, the snapshot's home is the choice, not what automatic would pick.
    expect(automaticLabel(fx({ home: "JPY", homeSource: "setting" }), "JPY")).toBe("Automatic");
  });

  test("the choices are the rates' currencies, keeping a stored one they no longer list", () => {
    expect(currencyChoices(fx(), "")).toEqual(["EUR", "INR", "USD"]);
    expect(currencyChoices(fx(), "ZAR")).toEqual(["EUR", "INR", "USD", "ZAR"]);
    expect(currencyChoices(null, "")).toContain("JPY");
  });

  test("the status line says why amounts show the viewer's currency alone", () => {
    expect(fxStatusText(fx())).toEqual({ text: "Converted at the European Central Bank's reference rate for 2026-10-02.", problem: false });
    expect(fxStatusText(fx({ stale: true })).problem).toBe(true);
    expect(fxStatusText(fx({ rates: {} })).text).toContain("not been downloaded");
    expect(fxStatusText(fx({ home: "" })).text).toContain("Choose one");
  });
});
