// What an event's `amount` counts. Shared by the runtime's formatters and the alert scope
// resolver, so a variation's "amount" condition tests the same number the card prints.

import type { EventType, NormalizedEvent } from "../lib/api/bridge";
import { isTally } from "../lib/utils/format";

/** Types whose `amount` is money in hundredths of `currency`. Any other type reads as a plain
 * count, so nothing is dressed as currency unless it is listed here. */
export const MONEY_TYPES: ReadonlySet<EventType> = new Set<EventType>(["superchat", "supersticker"]);

/** The host omits a zero amount from the wire, so for a tally (a 0-viewer raid) a missing
 * amount is zero. Anything else without one carries no amount at all. */
export function amountOf(e: Pick<NormalizedEvent, "type" | "amount">): number | null {
  if (e.amount != null) {
    return e.amount;
  }
  return isTally(e.type) ? 0 : null;
}
