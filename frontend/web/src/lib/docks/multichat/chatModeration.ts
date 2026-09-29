// Platform moderation over chat rows: which loaded messages a `chat.moderation` op names,
// what a message becomes once it is removed, and what its tombstone says. The host
// redacts its own scrollback the same way (RedactFrame) before it sends the op, so a page
// the host reads after the op already carries `deleted` and no fragments. Rows the dock
// already holds, and a page read before the op that lands after it, need the op applied.
// So does a live row admitted before the op whose frame the host posted after it.

import type { ChatMessage, ChatModeration, ChatModerationAction } from "$lib/api/bridge";
import { destinationKey, isDestinationKey } from "$lib/api/destinationKeys";

/** What a removed message reads as, in place of its body. */
export const TOMBSTONE_TEXT: Record<ChatModerationAction, string> = {
  message: "Message deleted",
  user: "Messages removed by a moderator",
  all: "Messages removed by a moderator",
};

/** Whether `op` removes a message: one the host admitted before the op (`seq` under
 * `before`) and, when the op carries `beforeTs`, said at or before it (a message with no
 * time, `ts` 0, is then never named), from the same destination, and then the whole
 * chat, the one message it names, or everything by the author it names. An op missing
 * the id its action needs names nothing. A message admitted or said after the op is never
 * removed by it: a line said after a clear, or by a user whose timeout has run out.
 *
 * Built once per op, so testing a message allocates nothing: the feed runs every
 * remembered op over every row that joins it. */
export function moderationMatcher(
  op: ChatModeration,
): (m: Pick<ChatMessage, "accountId" | "profileUuid" | "id" | "seq" | "ts" | "author">) => boolean {
  const dest = destinationKey(op.accountId, op.profileUuid);
  const beforeTs = op.beforeTs;
  return (m) => {
    if (!(m.seq < op.before) || !isDestinationKey(dest, m.accountId, m.profileUuid)) {
      return false;
    }
    if (beforeTs !== undefined && !(m.ts > 0 && m.ts <= beforeTs)) {
      return false;
    }
    switch (op.action) {
      case "all":
        return true;
      case "message":
        return !!op.msgId && m.id === op.msgId;
      case "user":
        return !!op.authorId && m.author?.id === op.authorId;
      default:
        return false;
    }
  };
}

/** `m` as removed by `action`: no body, marked deleted. Returned unchanged when it
 * already is, so a repeat op re-renders nothing. */
export function redactChat(m: ChatMessage, action: ChatModerationAction): ChatMessage {
  if (m.deleted === action && m.fragments.length === 0) {
    return m;
  }
  return { ...m, fragments: [], deleted: action };
}
