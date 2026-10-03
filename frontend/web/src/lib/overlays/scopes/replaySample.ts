// The test event the stage's Replay button fires for a scope: one that resolves to THAT
// scope, so what plays is what the inspector is editing. Every candidate is checked with the
// overlay's own resolver rather than reasoned about here, so a sample can never disagree with
// what the stream would do with the same event.

import type { ChatPlatform, EventType, NormalizedEvent } from "$lib/api/bridge";
import { PLATFORM_ORDER } from "$lib/theme/platformColors";
import {
  conditionCurrency,
  DEFAULT_SCOPE,
  resolveAlertSettings,
  tierNumber,
  type AlertEventScope,
  type AlertScopes,
  type AlertVariation,
} from "../../../overlay/alertScopes";

/** What overlays.test takes: the event type, and the fields that pin its resolution. Every
 * condition-relevant field is sent explicitly, because the host fills a missing one with a
 * stand-in of its own (500 bits, USD, Twitch) that a variation could match. */
export interface ReplaySample {
  type: string;
  overrides: { amount: number; tier: string; months: number; count: number; platform: string; currency: string };
}

/** The smallest values each field takes: below every sensible threshold, so an event-scope
 * sample does not trip one of its own variations. Platform is chosen per sample (eventSample). */
const MINIMAL: ReplaySample["overrides"] = {
  amount: 1,
  tier: "Tier 1",
  months: 1,
  count: 1,
  platform: PLATFORM_ORDER[0],
  currency: "USD",
};

function asEvent(s: ReplaySample): NormalizedEvent {
  return {
    id: "replay-sample",
    type: s.type as EventType,
    ts: 0,
    actorName: "",
    ...s.overrides,
    platform: s.overrides.platform as ChatPlatform,
  };
}

function resolvesTo(scopes: AlertScopes, s: ReplaySample): string {
  return resolveAlertSettings({}, scopes, asEvent(s)).scope;
}

/** The event type a sample of `event` uses: its first type, or a gift sub when the
 * variation tests for one. */
function typeFor(event: AlertEventScope, variation: AlertVariation | null): string {
  if (variation?.when.field === "gift" && variation.when.value === true && event.types.includes("subgift")) {
    return "subgift";
  }
  return event.types[0] ?? event.key;
}

/** The sample for an event scope: its minimal values on the first platform where no
 * variation of its own catches it (a YouTube follow variation must not hijack the Follows
 * replay); failing that, on the first platform. */
function eventSample(scopes: AlertScopes, event: AlertEventScope): ReplaySample {
  const type = typeFor(event, null);
  const variationIds = new Set(scopes.variations.map((v) => v.id));
  for (const platform of PLATFORM_ORDER) {
    const s = { type, overrides: { ...MINIMAL, platform } };
    if (!variationIds.has(resolvesTo(scopes, s))) {
      return s;
    }
  }
  return { type, overrides: { ...MINIMAL } };
}

/** The sample for a variation: its event at exactly its threshold, in its currency, on its
 * platform. */
function variationSample(event: AlertEventScope, v: AlertVariation): ReplaySample {
  const overrides = { ...MINIMAL };
  const value = v.when.value;
  switch (v.when.field) {
    case "amount":
      overrides.amount = typeof value === "number" ? value : MINIMAL.amount;
      overrides.currency = conditionCurrency(v.when) ?? MINIMAL.currency;
      break;
    case "months":
    case "count":
      overrides[v.when.field] = typeof value === "number" ? value : MINIMAL[v.when.field];
      break;
    case "platform":
      overrides.platform = typeof value === "string" && value !== "" ? value.toLowerCase() : MINIMAL.platform;
      break;
    case "tier": {
      const n = tierNumber(value);
      overrides.tier = n !== null ? `Tier ${n}` : String(value ?? "");
      break;
    }
  }
  return { type: typeFor(event, v), overrides };
}

/** The sample Replay fires for `scope`, or null when the scope is unknown. A variation whose
 * condition is broken still gets its event's sample, which is the honest replay: it shows
 * that the variation never plays. */
export function sampleFor(scopes: AlertScopes, scope: string): ReplaySample | null {
  if (scope === DEFAULT_SCOPE) {
    // The first event no scope of its own touches; failing that, the first event at all.
    for (const event of scopes.events) {
      const s = eventSample(scopes, event);
      if (resolvesTo(scopes, s) === DEFAULT_SCOPE) {
        return s;
      }
    }
    const first = scopes.events[0];
    return first ? eventSample(scopes, first) : null;
  }
  const variation = scopes.variations.find((v) => v.id === scope);
  const event = scopes.events.find((e) => e.key === (variation ? variation.event : scope));
  if (!event) {
    return null;
  }
  return variation ? variationSample(event, variation) : eventSample(scopes, event);
}
