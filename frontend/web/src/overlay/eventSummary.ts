// How an overlay words one event in a line: a glyph and an action phrase ("gifted 5 subs ·
// Tier 1"), ported from EventsDock.svelte's SUMMARY map so overlays read like the app. One
// copy for every stock widget that lists events (the ticker, the event list, the end
// credits), shared through OBSOverlay.summarize / eventEmoji, so they cannot drift apart.
// Data, not branches: a new type is one row in each map.

import type { NormalizedEvent } from "../lib/api/bridge";

/** How amounts are read: OBSOverlay.formatAmount / formatAmountText, which carry the money
 * rule (the streamer's currency first) and each tally's unit. */
export interface AmountFormat {
  amount(e: NormalizedEvent): string;
  amountText(e: NormalizedEvent): string;
}

/** Leading glyph per event type. */
export const EVENT_EMOJI: Readonly<Record<string, string>> = {
  follow: "🎉",
  sub: "⭐",
  resub: "🔄",
  subgift: "🎁",
  cheer: "💎",
  raid: "🚀",
  superchat: "💰",
  supersticker: "🎨",
  member: "🛡️",
  kicks: "🪙",
};

const SUMMARY: Readonly<Record<string, (e: NormalizedEvent, f: AmountFormat) => string>> = {
  // YouTube's follow is a subscribe (eventWording.ts's followVerb).
  follow: (e) => (e.platform === "youtube" ? "subscribed" : "followed"),
  sub: (e) => "subscribed" + (e.tier ? " · " + e.tier : ""),
  resub: (e) => "resubscribed" + (e.months ? " · " + e.months + " months" : ""),
  subgift: (e) => {
    const n = e.count != null ? e.count : 1;
    return "gifted " + n + " sub" + (n === 1 ? "" : "s") + (e.tier ? " · " + e.tier : "");
  },
  cheer: (e, f) => "cheered " + f.amountText(e),
  raid: (e, f) => "raided with " + f.amountText(e),
  superchat: (e, f) => "Super Chat" + (e.amount != null ? " " + f.amount(e) : ""),
  supersticker: (e, f) => "Super Sticker" + (e.amount != null ? " " + f.amount(e) : ""),
  member: (e) =>
    e.months ? "member · " + e.months + " months" : e.tier ? "became a member · " + e.tier : "became a member",
  kicks: (e, f) => "sent " + f.amountText(e) + (e.tier ? " · " + e.tier : ""),
};

/** `e`'s action phrase; an unknown type reads as its raw type string. */
export function eventSummary(e: NormalizedEvent, f: AmountFormat): string {
  return Object.hasOwn(SUMMARY, e.type) ? SUMMARY[e.type](e, f) : String(e.type);
}

/** `type`'s glyph, "" for an unknown type. */
export function eventEmoji(type: string): string {
  return Object.hasOwn(EVENT_EMOJI, type) ? EVENT_EMOJI[type] : "";
}
