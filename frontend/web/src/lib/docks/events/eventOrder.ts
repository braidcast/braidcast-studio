// The order the Events dock shows events in, which must be the host's events.list page
// order (EventStore::Page, frontend/src/events/event_store.cpp): by the event's own time,
// then by id. Arrival order is no use -- every account connect re-seeds the most recent
// followers regardless of age, so a follower from 2020 re-seeded today would read as the
// newest row.

import type { NormalizedEvent } from "$lib/api/bridge";

/** Code-point order, which is the host's: it compares ids as UTF-8 bytes. `<` on strings
 * compares UTF-16 code units and disagrees for astral characters (U+1F600 sorts before
 * U+FF21 that way), so this walks code points instead. */
export function compareCodePoints(a: string, b: string): number {
  const ia = a[Symbol.iterator]();
  const ib = b[Symbol.iterator]();
  for (;;) {
    const x = ia.next();
    const y = ib.next();
    if (x.done || y.done) {
      return x.done && y.done ? 0 : x.done ? -1 : 1;
    }
    const d = x.value.codePointAt(0)! - y.value.codePointAt(0)!;
    if (d !== 0) {
      return d;
    }
  }
}

/** Oldest first by the event's own time, then by id. */
export function compareEvents(a: Pick<NormalizedEvent, "ts" | "id">, b: Pick<NormalizedEvent, "ts" | "id">): number {
  return a.ts - b.ts || compareCodePoints(a.id, b.id);
}
