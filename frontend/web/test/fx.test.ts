import { describe, expect, test } from "bun:test";
import { fmtMoney } from "../src/lib/utils/format";
import { fmtMoneyDual, homeAmountText, paidAmountText, toHome, type FxSnapshot } from "../src/lib/utils/fx";

const fx = (over: Partial<FxSnapshot> = {}): FxSnapshot => ({
  home: "INR",
  homeSource: "youtube",
  date: "2026-10-02",
  fetchedAt: 1,
  stale: false,
  rates: { EUR: 1, USD: 1.08, INR: 90.4, JPY: 161.4 },
  ...over,
});
const money = (n: number, c: string) => new Intl.NumberFormat(undefined, { style: "currency", currency: c, minimumFractionDigits: 0, maximumFractionDigits: n >= 100 ? 0 : 2 }).format(n);

describe("Super Chat amounts in two currencies", () => {
  test("a $50 Super Chat reads in the streamer's rupees first, the payer's dollars second", () => {
    const rupees = (50 / 1.08) * 90.4; // 4,185.19
    expect(toHome(5000, "USD", fx())).toBeCloseTo(rupees, 6);
    expect(fmtMoneyDual(5000, "USD", fx())).toBe(`≈${money(rupees, "INR")} (${fmtMoney(5000, "USD")})`);
  });

  test("the payer's own display string is kept when one is given", () => {
    expect(fmtMoneyDual(5000, "usd", fx(), "$50.00")).toBe(`≈${money((50 / 1.08) * 90.4, "INR")} ($50.00)`);
  });

  test("small figures keep their cents; from 100 up they round to whole units", () => {
    expect(homeAmountText(100, "INR", fx({ home: "USD" }))).toBe(`≈${money((1 / 90.4) * 1.08, "USD")}`);
    expect(homeAmountText(500000, "JPY", fx({ home: "EUR" }))).toBe(`≈${money(5000 / 161.4, "EUR")}`);
  });

  test("anything that cannot convert honestly reads as the payer's amount alone", () => {
    const plain = fmtMoney(5000, "USD");
    expect(fmtMoneyDual(5000, "USD", fx({ stale: true }))).toBe(plain);
    expect(fmtMoneyDual(5000, "USD", fx({ home: "" }))).toBe(plain);
    expect(fmtMoneyDual(5000, "USD", fx({ rates: {} }))).toBe(plain);
    expect(fmtMoneyDual(5000, "ARS", fx())).toBe(fmtMoney(5000, "ARS"));
    expect(fmtMoneyDual(5000, "USD", fx({ home: "BRL" }))).toBe(plain);
    expect(fmtMoneyDual(5000, undefined, fx())).toBe(fmtMoney(5000));
    expect(fmtMoneyDual(5000, "USD", null)).toBe(plain);
    expect(fmtMoneyDual(5000, "toString", fx())).toBe(fmtMoney(5000, "toString"));
  });

  test("a payment already in the streamer's currency shows once", () => {
    expect(fmtMoneyDual(45000, "INR", fx())).toBe(fmtMoney(45000, "INR"));
  });

  test("a paid chat line keeps the platform's string and converts only a readable value", () => {
    const rupees = `≈${money((50 / 1.08) * 90.4, "INR")}`;
    expect(paidAmountText({ kind: "superchat", amount: "$50.00", value: 5000, currency: "USD" }, fx())).toBe(
      `${rupees} ($50.00)`,
    );
    expect(paidAmountText({ kind: "superchat", amount: "$50.00" }, fx())).toBe("$50.00");
    expect(paidAmountText({ kind: "cheer", amount: "100 bits" }, fx())).toBe("100 bits");
  });
});
