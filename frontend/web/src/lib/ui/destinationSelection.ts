// The destination-scoping vocabulary and logic shared by the two destination-scoped
// feeds: Chat (MultichatDock) and Events (EventsDock). Both ask the same five
// questions of the same `{ platform, accountId, profileUuid? }` row shape -- does this
// row belong to the current selection, which connected platform has nothing to show,
// which destination can this row honestly be attributed to, what is the selection
// called, and is the selection still valid -- so the answers live once, here.
//
// Not named `destinationFilter`: in Chat the same selection is also the send target,
// and calling it a filter is how a composer came to disagree with the feed it sits
// under. It scopes; what a scope means is the caller's.
//
// Its own module rather than exports on DestinationChips.svelte: the component renders
// a selection, it does not define what one means. Wording stays with the caller --
// Chat and Events describe the same fidelity tier in different words on purpose, and a
// shared string with a surface flag would be one abstraction pretending to be two.

import { destinationIdentityStore, type DestinationIdentity } from "$lib/stores/destinationIdentityStore.svelte";
import { PLATFORM_LABELS, platformKey } from "$lib/theme/platformColors";

/** What is selected. `platform` is a platformColors.ts key; `profileUuid` names one
 * stream profile. A caller that can only act on a single destination gates on
 * `kind === "destination"` rather than parsing a string. */
export type DestinationSelection =
  | { kind: "all" }
  | { kind: "platform"; platform: string }
  | { kind: "destination"; profileUuid: string };

export const ALL_DESTINATIONS: DestinationSelection = { kind: "all" };

/** What every destination surface prints for an absence it must not guess at. */
export const ABSENT_LABEL = "—";

/** What a surface prints where a canvas cannot be named because the row belongs to the
 * channel rather than to one of its broadcasts. */
export const CHANNEL_WIDE = "channel-wide";

/** The part of a feed row that decides which destination it came from. `accountId` is
 * optional because a host-synthesized event carries none; `profileUuid` is present
 * only on a row a per-broadcast source stamped. */
export interface DestinationSource {
  platform: string;
  accountId?: string;
  profileUuid?: string;
}

/** accountId -> its destinations. Needed to attribute a channel-wide row to the
 * streams it could belong to, and to know whether one chat is shared. */
export function destinationsByAccount(
  destinations: readonly DestinationIdentity[],
): Map<string, DestinationIdentity[]> {
  const m = new Map<string, DestinationIdentity[]>();
  for (const d of destinations) {
    const siblings = m.get(d.accountId);
    if (siblings) {
      siblings.push(d);
    } else {
      m.set(d.accountId, [d]);
    }
  }
  return m;
}

/** Connected platforms with no destination configured. Their transports still run, so
 * they earn a disabled chip that says why rather than no chip at all. */
export function unarmedPlatforms(
  connectedPlatforms: readonly string[],
  destinations: readonly DestinationIdentity[],
): string[] {
  return connectedPlatforms.filter((p) => !destinations.some((d) => d.platform === p));
}

/** `consequence` completes the sentence with what this particular surface loses --
 * "it has no chat here." / "its events cannot be filtered." */
export function unarmedHint(platform: string, consequence: string): string {
  return (
    (PLATFORM_LABELS[platformKey(platform)] ?? platform) +
    " is connected but has no destination configured, so " +
    consequence
  );
}

/**
 * Does this row belong to the current selection?
 *
 * A channel-wide row has no canvas, so it belongs to every stream of its channel
 * equally; showing it under each keeps narrowing to one stream from silently dropping
 * real rows. The row still reads "channel-wide", so it cannot be mistaken for an exact
 * attribution.
 */
export function matchesSelection(
  item: DestinationSource,
  sel: DestinationSelection,
  destByUuid: ReadonlyMap<string, DestinationIdentity>,
): boolean {
  if (sel.kind === "all") {
    return true;
  }
  if (sel.kind === "platform") {
    return platformKey(item.platform) === sel.platform;
  }
  if (item.profileUuid) {
    return item.profileUuid === sel.profileUuid;
  }
  return item.accountId === destByUuid.get(sel.profileUuid)?.accountId;
}

/**
 * Provenance of a row's canvas label, in descending confidence. Which tier a row lands
 * in is a fact about its source, not a presentation choice:
 *
 * - `exact` -- the row named the broadcast it arrived on. Only a per-broadcast source
 *   stamps `profileUuid` (YouTube's live-chat sink, `frontend/src/chat/youtube_chat.cpp`);
 *   Twitch EventSub, Kick's Pusher feed and YouTube's REST reads carry `accountId` only.
 * - `single` -- INFERRED, never stated: a channel-wide row whose channel has exactly one
 *   armed destination, so the engine (one enabled binding per profile, `bridge.cpp`
 *   outputBinding.setEnabled) leaves no other stream it could belong to.
 * - `wide` -- channel-wide with more than one candidate. Naming a canvas would be a guess.
 * - `pending` -- the destination's canvas has not loaded yet, or was deleted.
 * - `none` -- no stream profile is configured for the account the row came from.
 */
export type Fidelity = "exact" | "single" | "wide" | "pending" | "none";

export interface Attribution {
  fidelity: Fidelity;
  channel: string;
  avatarUrl: string;
  canvasLabel: string;
  /** canvasLabel is a real canvas name rather than a state word. */
  named: boolean;
  /** The canvas's number and encode size, for the canvas mark. All 0 unless `named`:
   * a row that will not name a canvas has no canvas to draw either. */
  canvasNumber: number;
  canvasWidth: number;
  canvasHeight: number;
  /** How many destinations share this channel; the canvas earns row width only where
   * it disambiguates. */
  siblings: number;
}

function fromDestination(
  d: DestinationIdentity,
  fidelity: Fidelity,
  destByAccount: ReadonlyMap<string, DestinationIdentity[]>,
): Attribution {
  const named = fidelity === "exact" || fidelity === "single";
  return {
    fidelity,
    channel: d.displayName,
    avatarUrl: d.channelAvatarUrl,
    canvasLabel: named ? (d.canvasName ?? ABSENT_LABEL) : fidelity === "wide" ? CHANNEL_WIDE : ABSENT_LABEL,
    named,
    canvasNumber: named ? d.canvasNumber : 0,
    canvasWidth: named ? d.canvasWidth : 0,
    canvasHeight: named ? d.canvasHeight : 0,
    siblings: destByAccount.get(d.accountId)?.length ?? 1,
  };
}

/** Which live stream produced this row, and how confidently. */
export function attribute(
  item: DestinationSource,
  destByAccount: ReadonlyMap<string, DestinationIdentity[]>,
): Attribution {
  if (item.profileUuid) {
    const d = destinationIdentityStore.forProfile(item.profileUuid);
    // A profile deleted since the row arrived is not fatal: the account below still
    // names the channel, so it degrades to channel-wide rather than to nothing.
    if (d) {
      const fidelity: Fidelity = d.canvasUuid === null ? "wide" : d.canvasName === null ? "pending" : "exact";
      return fromDestination(d, fidelity, destByAccount);
    }
  }
  const siblings = item.accountId ? (destByAccount.get(item.accountId) ?? []) : [];
  const armed = siblings.filter((d) => d.canvasUuid !== null);
  if (armed.length === 1) {
    return fromDestination(armed[0], armed[0].canvasName === null ? "pending" : "single", destByAccount);
  }
  if (siblings.length > 0) {
    // Any sibling names the channel identically once OAuth identity has loaded; prefer
    // one that has it so a row never prints a profile label as a channel.
    return fromDestination(siblings.find((d) => d.channelName !== "") ?? siblings[0], "wide", destByAccount);
  }
  return {
    fidelity: "none",
    channel: ABSENT_LABEL,
    avatarUrl: "",
    canvasLabel: ABSENT_LABEL,
    named: false,
    canvasNumber: 0,
    canvasWidth: 0,
    canvasHeight: 0,
    siblings: 0,
  };
}

/**
 * The current selection in words. `separator` joins channel and canvas; `all` is what
 * this surface calls its unscoped state (and is also the fallback for a destination
 * selection whose profile has gone).
 */
export function selectionLabel(
  sel: DestinationSelection,
  destByUuid: ReadonlyMap<string, DestinationIdentity>,
  { separator, all }: { separator: string; all: string },
): string {
  if (sel.kind === "platform") {
    return PLATFORM_LABELS[sel.platform] ?? sel.platform;
  }
  if (sel.kind === "destination") {
    const d = destByUuid.get(sel.profileUuid);
    if (d) {
      return d.canvasName ? d.displayName + separator + d.canvasName : d.displayName;
    }
  }
  return all;
}

/**
 * The selection this surface should hold now, or null when the current one still
 * stands. With exactly one chip, pin to it: it is the only one, so it must read active
 * and it is an honest single target. Otherwise drop a selection whose destination or
 * platform is gone back to all.
 */
export function reconcileSelection(
  sel: DestinationSelection,
  destinations: readonly DestinationIdentity[],
  destByUuid: ReadonlyMap<string, DestinationIdentity>,
  unarmedCount: number,
): DestinationSelection | null {
  if (destinations.length === 1 && unarmedCount === 0) {
    const only = destinations[0].profileUuid;
    if (sel.kind === "destination" && sel.profileUuid === only) {
      return null;
    }
    return { kind: "destination", profileUuid: only };
  }
  const stale =
    (sel.kind === "destination" && !destByUuid.has(sel.profileUuid)) ||
    (sel.kind === "platform" && !destinations.some((d) => platformKey(d.platform) === sel.platform));
  return stale ? ALL_DESTINATIONS : null;
}
