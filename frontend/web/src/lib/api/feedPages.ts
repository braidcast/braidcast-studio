// Walks a paged feed (events.list, chat.list) back to its oldest row, for a dock that
// still holds its whole history in the webview.

import type { FeedPage } from "$lib/api/bridge";

/** The largest page the host serves (Feed::kMaxLimit, frontend/src/chat/feed_query.hpp). */
export const FEED_PAGE_MAX = 200;

/** Every row the feed holds, oldest first: pages are requested newest-first, each one
 * before the oldest row of the last, until the host reports no more. A page read under a
 * newer clear epoch than the ones before it discards what they returned. */
export async function fetchAllPages<T>(page: (before: T | undefined) => Promise<FeedPage<T>>): Promise<T[]> {
  let items: T[] = [];
  let epoch: number | undefined;
  let before: T | undefined;
  for (;;) {
    const next = await page(before);
    if (epoch !== undefined && next.epoch !== epoch) {
      items = [];
    }
    epoch = next.epoch;
    items = next.items.concat(items);
    if (!next.more || next.items.length === 0) {
      return items;
    }
    before = next.items[0];
  }
}
