// What the Events dock shows by default, and in what order.
//
// The host's store keeps the last 500 events however old they are, and every account
// connect re-seeds the most recent followers regardless of age -- so the ring is mostly
// history, and in arrival order a follower from 2020 re-seeded today reads as the newest
// row. The dock therefore orders by the event's own time and opens scoped to the current
// broadcast; everything older stays one toggle away rather than being dropped (the ring
// is also what the host dedupes against, so nothing here may shrink it).

import type { SessionInfo } from "$lib/api/bridge";
import type { FeedRow } from "$lib/utils/feedVirtualizer.svelte";

/** Which boundary the default view starts at: the broadcast on the air, the last one this
 * app session ran, or -- with no broadcast yet this session -- the app's own start. */
export type ScopeKind = "live" | "last" | "launch";

export interface EventScope {
  /** Epoch ms; events at or after it are in scope. */
  since: number;
  kind: ScopeKind;
}

/**
 * The scope boundary. The newest broadcast that started this app session: running
 * (`endedAt` null) makes it the live one, finished makes it the last one. Sessions from
 * earlier launches never count, and with none this launch the boundary is the launch.
 * History unavailable (no session rows at all) degrades to the launch boundary too.
 */
export function eventScope(
  sessions: readonly Pick<SessionInfo, "startedAt" | "endedAt">[],
  appStartedAt: number,
): EventScope {
  let latest: Pick<SessionInfo, "startedAt" | "endedAt"> | null = null;
  for (const s of sessions) {
    if (s.startedAt >= appStartedAt && (latest === null || s.startedAt > latest.startedAt)) {
      latest = s;
    }
  }
  if (latest === null) {
    return { since: appStartedAt, kind: "launch" };
  }
  return { since: latest.startedAt, kind: latest.endedAt === null ? "live" : "last" };
}

/** Oldest first by the event's own time; arrival order breaks a tie, so rows sharing a
 * timestamp never swap places between renders. */
export function byEventTime<T extends { ts: number }>(rows: readonly FeedRow<T>[]): FeedRow<T>[] {
  return [...rows].sort((a, b) => a.item.ts - b.item.ts || a.clientKey - b.clientKey);
}

/** The rows to show under a scope, and how many it holds back. With `showEarlier` every
 * row shows; `earlier` still counts the pre-scope ones, so the toggle keeps its number. */
export function scopeRows<T extends { ts: number }>(
  rows: readonly FeedRow<T>[],
  since: number,
  showEarlier: boolean,
): { shown: FeedRow<T>[]; earlier: number } {
  const inScope = rows.filter((r) => r.item.ts >= since);
  return { shown: showEarlier ? [...rows] : inScope, earlier: rows.length - inScope.length };
}
