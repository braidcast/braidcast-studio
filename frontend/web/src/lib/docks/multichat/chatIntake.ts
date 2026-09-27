// How messages reach the Multichat dock's feed: the host's scrollback first (chat.list),
// then live chat.message frames, each message exactly once.
//
// The host dedupes: its ring (chat_history.hpp) admits each message once and only an
// admitted one is emitted, so the snapshot holds no repeats and neither does the live
// stream. What the dock must not double is the seam between the two. It subscribes before
// it asks for the scrollback, so nothing can fall between them -- which means a message
// landing while chat.list is in flight can arrive both in the snapshot and live. Live
// frames are held until the snapshot lands, the snapshot goes in first (it is older), and
// only the held frames are checked against it; after that live frames pass straight on.

import type { ChatMessage } from "$lib/api/bridge";
import { destinationKey } from "$lib/api/destinationKeys";

/** The host's scrollback cap (Chat::ChatHistory::kCap, frontend/src/chat/chat_history.hpp).
 * The feed holds as many rows, so a hydrate never trims what the host just sent. */
export const CHAT_HISTORY_MAX = 1000;

/** One message's identity: its destination plus its platform id. Two transports on one
 * platform share an id space, so the id alone would merge two chats' lines. Matches the
 * host's key (DestinationKey + ":" + id). */
export function chatKey(m: Pick<ChatMessage, "accountId" | "profileUuid" | "id">): string {
  return destinationKey(m.accountId, m.profileUuid) + ":" + m.id;
}

/** Where admitted messages go -- the dock's FeedVirtualizer. */
export interface ChatSink {
  enqueue(m: ChatMessage): void;
  setFeed(list: ChatMessage[]): void;
}

export class ChatIntake {
  readonly #sink: ChatSink;
  // Live frames held until the scrollback lands; null once it has.
  #held: ChatMessage[] | null = [];

  constructor(sink: ChatSink) {
    this.#sink = sink;
  }

  /** A live chat.message frame. */
  live(m: ChatMessage): void {
    if (this.#held) {
      this.#held.push(m);
      return;
    }
    this.#sink.enqueue(m);
  }

  /** The scrollback, oldest first -- or [] when chat.list failed, which still releases
   * the held live frames. Only the first call counts. */
  hydrate(snapshot: readonly ChatMessage[]): void {
    const held = this.#held;
    if (!held) {
      return;
    }
    this.#held = null;
    this.#sink.setFeed([...snapshot]);
    // A message with no id cannot be matched, so it is let through.
    const inSnapshot = new Set(snapshot.filter((m) => m.id).map(chatKey));
    for (const m of held) {
      if (!m.id || !inSnapshot.has(chatKey(m))) {
        this.#sink.enqueue(m);
      }
    }
  }
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
