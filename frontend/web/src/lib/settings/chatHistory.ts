// The Chat history section of General settings: the retention choices, which changes
// delete stored chat and so ask first, the session-only platform hint, and the status line.
// Pure, so the rules are testable without the tab.
import type { ChatHistoryRetention, ChatHistoryStatus } from "$lib/api/bridge";
import { PLATFORM_LABELS, platformKey } from "$lib/theme/platformColors";

export const RETENTION_OPTIONS: readonly { value: ChatHistoryRetention; label: string }[] = [
  { value: "off", label: "Off" },
  { value: "session", label: "This session" },
  { value: "7d", label: "7 days" },
];

/** Whether a stored GeneralSettings.chatHistoryRetention is one this build knows. */
export function isRetention(value: string): value is ChatHistoryRetention {
  return RETENTION_OPTIONS.some((o) => o.value === value);
}

/**
 * Whether moving from `from` to `to` deletes stored chat, and so needs a confirm. Off
 * deletes everything; This session deletes what earlier sessions stored, which only
 * 7 days keeps. Moving to a longer retention deletes nothing. A stored value this build
 * does not know keeps what is stored as 7 days does, so it counts as the longest.
 */
export function retentionChangeDeletes(from: string, to: ChatHistoryRetention): boolean {
  if (from === to) return false;
  const kept: ChatHistoryRetention = isRetention(from) ? from : "7d";
  return to === "off" || (to === "session" && kept === "7d");
}

/** The confirm prompt for a change retentionChangeDeletes says deletes. */
export function retentionConfirmMessage(to: ChatHistoryRetention): string {
  return to === "off"
    ? "Turning chat history off deletes all chat stored on this computer."
    : "Keeping only this session deletes the chat stored by earlier sessions.";
}

const LIST = new Intl.ListFormat("en", { style: "long", type: "conjunction" });

/**
 * The chat platforms whose history is kept for the current session only, in words: every
 * platform with chat (the keys of PLATFORM_LABELS) less the ones whose deletes and bans the
 * app applies, which the host reports. Empty when every platform is moderated.
 */
export function sessionOnlyPlatforms(moderated: readonly string[]): string[] {
  const kept = new Set(moderated.map(platformKey));
  return Object.keys(PLATFORM_LABELS)
    .filter((p) => !kept.has(p))
    .map((p) => PLATFORM_LABELS[p]);
}

/** What 7 days does not keep past the session, or "" when it keeps every platform. Only
 *  7 days needs saying: This session keeps every platform for the session alike. */
export function sessionOnlyHint(moderated: readonly string[]): string {
  const names = sessionOnlyPlatforms(moderated);
  if (names.length === 0) return "";
  return `With 7 days, ${LIST.format(names)} chat is still kept for this session only, because moderator deletes and bans there are not applied to stored chat yet.`;
}

function messages(n: number): string {
  return `${n.toLocaleString("en")} message${n === 1 ? "" : "s"}`;
}

/** The status line, and whether it reports a problem. */
export function historyStatusText(s: ChatHistoryStatus): { text: string; problem: boolean } {
  switch (s.status) {
    case "off":
      return s.onDisk
        ? {
            text: "Nothing new is stored, but a chat history file this version did not remove is still on disk.",
            problem: true,
          }
        : { text: "Nothing is stored.", problem: false };
    case "ok":
      return { text: `${messages(s.rows)} stored.`, problem: false };
    case "recovered":
      return {
        text: s.detail
          ? `The chat history file was damaged and set aside as ${s.detail}; a new one was started. ${messages(s.rows)} stored.`
          : `The chat history file was damaged and deleted; a new one was started. ${messages(s.rows)} stored.`,
        problem: true,
      };
    case "newer-schema":
      return {
        text: `The chat history file belongs to a newer version of Braidcast and is left as it is (${s.detail}). Chat stays in memory only while that file is there.`,
        problem: true,
      };
    case "disabled":
      return { text: `Chat history is unavailable this session: ${s.detail}.`, problem: true };
    case "degraded":
      return {
        text: `Chat history stopped saving this session (${s.detail}). Recent chat stays in memory until you restart.`,
        problem: true,
      };
    case "unknown-setting":
      return {
        text: `The stored setting ("${s.detail}") is not one this version knows. Chat already saved is kept for up to 7 days and removed as usual, but new chat is not saved. Choose a setting above.`,
        problem: true,
      };
    case "unreadable":
      return {
        text: `The chat history file ${s.detail} could not be read, so it was left in place. Removing it is tried again when chat history is next cleared or its setting changes.`,
        problem: true,
      };
    default:
      return { text: "The chat history status is not one this version knows.", problem: true };
  }
}
