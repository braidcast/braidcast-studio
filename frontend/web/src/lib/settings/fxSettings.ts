// What Settings > General says about the Super Chat currency (GeneralSettings.fxHomeCurrency):
// the choices, the automatic one's resolved currency and source, and the line under the field.
// Pure, so the wording is tested rather than read off the page.

import type { FxSnapshot } from "$lib/api/bridge";

/** The currencies offered before any rates arrived: the ECB's reference set plus EUR, which
 * is what any later table holds too. */
const ECB_CURRENCIES = [
  "AUD", "BRL", "CAD", "CHF", "CNY", "CZK", "DKK", "EUR", "GBP", "HKD", "HUF", "IDR", "ILS", "INR", "ISK",
  "JPY", "KRW", "MXN", "MYR", "NOK", "NZD", "PHP", "PLN", "RON", "SEK", "SGD", "THB", "TRY", "USD", "ZAR",
];

const SOURCE_WORDS: Record<string, string> = {
  youtube: "from your YouTube channel's country",
  locale: "from your Windows region",
};

/** The currency choices, sorted: the rates' own when there are any, else the ECB set. The
 * stored value is kept as a choice even when the rates no longer list it. */
export function currencyChoices(fx: FxSnapshot | null, stored: string): string[] {
  const codes = new Set(fx && Object.keys(fx.rates).length > 0 ? Object.keys(fx.rates) : ECB_CURRENCIES);
  if (stored) {
    codes.add(stored);
  }
  return [...codes].sort();
}

/** The automatic choice's label: "Automatic (INR, from your YouTube channel's country)". */
export function automaticLabel(fx: FxSnapshot | null, stored: string): string {
  // With a currency chosen, the snapshot's home is that choice, not what automatic would be.
  if (!fx || stored || !fx.home) {
    return "Automatic";
  }
  const from = SOURCE_WORDS[fx.homeSource];
  return from ? `Automatic (${fx.home}, ${from})` : `Automatic (${fx.home})`;
}

/** The line under the field: where the rates come from and how old they are, or why amounts
 * show the viewer's currency alone. */
export function fxStatusText(fx: FxSnapshot | null): { text: string; problem: boolean } {
  if (!fx) {
    return { text: "Loading exchange rates…", problem: false };
  }
  if (!fx.home) {
    return {
      text: "No currency could be worked out for you, so Super Chats show the viewer's amount alone. Choose one above.",
      problem: true,
    };
  }
  if (Object.keys(fx.rates).length === 0) {
    return {
      text: "Exchange rates have not been downloaded yet, so Super Chats show the viewer's amount alone for now.",
      problem: true,
    };
  }
  if (fx.stale) {
    return {
      text: `The exchange rates are out of date (${fx.date}) and could not be refreshed, so Super Chats show the viewer's amount alone.`,
      problem: true,
    };
  }
  return {
    text: `Converted at the European Central Bank's reference rate for ${fx.date}.`,
    problem: false,
  };
}
