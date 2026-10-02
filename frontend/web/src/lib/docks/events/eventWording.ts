// How the app words a `follow` event per platform. A YouTube subscribe normalizes to
// `follow` (frontend/src/events/youtube_events.cpp) because it is the free channel follow,
// not the paid membership, but YouTube calls that act "subscribe". The event type stays
// `follow` so goals, labels, filters and replay are unaffected; only the wording differs.
// Overrides only: a new platform needs an entry here only where it disagrees.

import type { NormalizedEvent } from "$lib/api/bridge";
import { EVENT_TYPE_LABELS } from "$lib/theme/platformColors";

type Platform = NormalizedEvent["platform"];

const FOLLOW_VERB: Partial<Record<Platform, string>> = { youtube: "subscribed" };
const FOLLOW_LABEL: Partial<Record<Platform, string>> = { youtube: "Subscriber" };

/** The past-tense verb for a follow on `platform`. */
export function followVerb(platform: Platform): string {
  return FOLLOW_VERB[platform] ?? "followed";
}

/** The short label for an event of `type` on `platform` (the dock's row icon title). */
export function eventTypeLabel(type: string, platform: Platform): string {
  if (type === "follow") return FOLLOW_LABEL[platform] ?? EVENT_TYPE_LABELS.follow;
  return EVENT_TYPE_LABELS[type] ?? type;
}
