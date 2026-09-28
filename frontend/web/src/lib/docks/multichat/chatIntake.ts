// A chat message's identity and the question the Multichat dock asks of a set of them.
// The seam between chat.list pages and live chat.message frames -- a message that lands
// while a page is in flight can arrive both ways -- is closed by the feed itself, which
// dedupes on chatKey (utils/feedVirtualizer.svelte.ts).

import type { ChatMessage } from "$lib/api/bridge";
import { destinationKey } from "$lib/api/destinationKeys";

/** One message's identity: its destination plus its platform id. Two transports on one
 * platform share an id space, so the id alone would merge two chats' lines. Matches the
 * host's key (DestinationKey + ":" + id). */
export function chatKey(m: Pick<ChatMessage, "accountId" | "profileUuid" | "id">): string {
  return destinationKey(m.accountId, m.profileUuid) + ":" + m.id;
}

/** Whether these messages came from two or more destinations. */
export function spansDestinations(messages: Iterable<Pick<ChatMessage, "accountId" | "profileUuid">>): boolean {
  let first: string | undefined;
  for (const m of messages) {
    const key = destinationKey(m.accountId, m.profileUuid);
    if (first === undefined) {
      first = key;
    } else if (key !== first) {
      return true;
    }
  }
  return false;
}
