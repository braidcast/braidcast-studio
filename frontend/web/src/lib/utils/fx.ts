// A Super Chat's amount in the streamer's currency first and the payer's second
// ("≈₹4,180 ($50.00)"), roadmap 9.6. One module for every surface that draws one -- the
// overlay runtime (alert box, ticker, labels), the Events dock and the chat dock -- so they
// cannot round, mark or refuse differently.
//
// The host supplies the rates (fx.get / fx.changed, and each overlay's bootstrap): the ECB's
// daily reference rates, units per 1 EUR. A converted figure is approximate and says so with
// "≈": YouTube's payout differs after fees and its own conversion. Anything that cannot be
// converted honestly -- no home currency, stale or missing rates, either currency outside
// them, or a payment already in the home currency -- reads as the payer's amount alone.

import type { ChatPaid, FxSnapshot } from "../api/bridge";
import { fmtMoney } from "./format";

export type { FxSnapshot };


const rateOf = (fx: FxSnapshot, code: string): number | null => {
  const r = Object.hasOwn(fx.rates, code) ? fx.rates[code] : undefined;
  return typeof r === "number" && Number.isFinite(r) && r > 0 ? r : null;
};

/** `hundredths` of `from` in the home currency's major units, or null when it cannot be
 * converted honestly (see the module comment). */
export function toHome(hundredths: number, from: string | undefined, fx: FxSnapshot | null | undefined): number | null {
  if (!fx || fx.stale || !fx.home || !from || !Number.isFinite(hundredths)) {
    return null;
  }
  const code = from.toUpperCase();
  if (code === fx.home) {
    return null;
  }
  const fromRate = rateOf(fx, code);
  const homeRate = rateOf(fx, fx.home);
  if (fromRate === null || homeRate === null) {
    return null;
  }
  return (hundredths / 100 / fromRate) * homeRate;
}

// currency|digits -> its formatter, or null for a code Intl refused.
const homeFormats = new Map<string, Intl.NumberFormat | null>();

/** An approximate figure: whole units from 100 up, where cents would claim a precision the
 * reference rate does not have; the currency's own cents below that. */
function fmtApprox(major: number, currency: string): string {
  const digits = Math.abs(major) >= 100 ? 0 : 2;
  const key = `${currency}|${digits}`;
  let format = homeFormats.get(key);
  if (format === undefined) {
    try {
      format = new Intl.NumberFormat(undefined, {
        style: "currency",
        currency,
        minimumFractionDigits: 0,
        maximumFractionDigits: digits,
      });
    } catch {
      format = null;
    }
    homeFormats.set(key, format);
  }
  return format ? format.format(major) : `${major.toFixed(digits)} ${currency}`;
}

/** The streamer's-currency figure for a payment, "≈₹4,180", or null when there is none to
 * show beside the payer's. */
export function homeAmountText(
  hundredths: number,
  currency: string | undefined,
  fx: FxSnapshot | null | undefined,
): string | null {
  const major = toHome(hundredths, currency, fx);
  return major === null || !fx ? null : `≈${fmtApprox(major, fx.home)}`;
}

/** A payment read for display: "≈₹4,180 ($50.00)" when it converts, else the payer's amount
 * alone. `payerText` is how the payer's amount is shown (a platform's own display string in
 * chat); it defaults to the app's money format. */
export function fmtMoneyDual(
  hundredths: number,
  currency: string | undefined,
  fx: FxSnapshot | null | undefined,
  payerText: string = fmtMoney(hundredths, currency),
): string {
  const home = homeAmountText(hundredths, currency, fx);
  return home === null ? payerText : `${home} (${payerText})`;
}

/** A paid chat line's amount: the platform's own display string, after the streamer's-currency
 * figure when the line carries a readable value (ChatPaid.value). Bits never convert. */
export function paidAmountText(paid: ChatPaid, fx: FxSnapshot | null | undefined): string {
  return typeof paid.value === "number" && paid.currency
    ? fmtMoneyDual(paid.value, paid.currency, fx, paid.amount)
    : paid.amount;
}
