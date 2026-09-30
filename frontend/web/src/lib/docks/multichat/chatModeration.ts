// Platform moderation over chat rows: which loaded messages a `chat.moderation` op names,
// what a message becomes once it is removed, and what its label says. The host redacts
// its own scrollback the same way before it sends the op, so a page the host reads after
// the op already carries `deleted` and no fragments, with what the message said in
// `retracted` while the host still holds it. Rows the dock already holds, and a page read
// before the op that lands after it, need the op applied. So does a live row admitted
// before the op whose frame the host posted after it. The stream overlay's chat widgets
// match with moderationMatcher too (overlay/runtime.ts), and only ever drop the line.

import type { ChatAuthor, ChatMessage, ChatModeration, ChatModerationAction } from "$lib/api/bridge";
// Relative rather than $lib: the overlay runtime bundles this module with `bun build`,
// outside Vite.
import { destinationKey, isDestinationKey } from "../../api/destinationKeys";

/** A removed message's label when the platform gave none of its own. */
export const TOMBSTONE_TEXT: Record<ChatModerationAction, string> = {
  message: "Message deleted",
  user: "Messages removed by a moderator",
  all: "Chat cleared",
};

/** The fields of a chat line a moderation op is matched on: identity, admission order and
 * time, never text. Every `chat` frame carries them. */
export type ChatIdentity = Pick<ChatMessage, "accountId" | "profileUuid" | "id" | "seq" | "ts"> & {
  author: Pick<ChatAuthor, "id">;
};

/** The ChatIdentity of a chat frame, for a widget that keeps a line after drawing or counting
 * it: only what an op is matched on, so holding many costs no text. Every field is kept as
 * the frame carries it, the author id untrimmed: the host redacts by the raw id and the dock
 * matches the raw id, so an overlay line is named by exactly the ops that name its dock row. */
export function chatIdentity(m: ChatIdentity): ChatIdentity {
  return {
    accountId: m.accountId,
    profileUuid: m.profileUuid,
    id: m.id,
    seq: m.seq,
    ts: m.ts,
    author: { id: m.author?.id },
  };
}

/** Whether `op` removes a message: one the host admitted before the op (`seq` under
 * `before`) and, when the op carries `beforeTs`, said at or before it (a message with no
 * time, `ts` 0, is then never named), from the same destination, and then the whole
 * chat, the one message it names, or everything by the author it names. An op missing
 * the id its action needs names nothing. A message admitted or said after the op is never
 * removed by it: a line said after a clear, or by a user whose timeout has run out.
 *
 * Built once per op, so testing a message allocates nothing: the feed runs every
 * remembered op over every row that joins it. */
export function moderationMatcher(op: ChatModeration): (m: ChatIdentity) => boolean {
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

/** What a removed message's label reads: the platform's own stub text, else ours. */
export function removedLabel(m: Pick<ChatMessage, "deleted" | "deletedLabel">): string {
  return m.deletedLabel || (m.deleted ? TOMBSTONE_TEXT[m.deleted] : "");
}

/** `m` as removed by `op`: no body, marked deleted, what it said kept in `retracted` so the
 * dock can still show it (in memory only, as the host keeps it), and the op's label when it
 * has one. A later op never replaces what an earlier one kept with nothing. Returned
 * unchanged when it already is, so a repeat op re-renders nothing. */
export function redactChat(m: ChatMessage, op: Pick<ChatModeration, "action" | "label">): ChatMessage {
  const label = op.label || m.deletedLabel;
  if (m.deleted === op.action && m.fragments.length === 0 && m.deletedLabel === label) {
    return m;
  }
  const retracted = m.retracted ?? (m.fragments.length > 0 ? m.fragments : undefined);
  return {
    ...m,
    fragments: [],
    deleted: op.action,
    ...(retracted ? { retracted } : {}),
    ...(label ? { deletedLabel: label } : {}),
  };
}
