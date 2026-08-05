// How a stream profile names and pictures itself. Every surface that lists
// destinations -- the Streams list, the bind picker, a canvas's destination cards
// -- renders the same three things, and each had grown its own copy: StreamsTab's
// displayName and ProfileSelect's profileName were already the same function under
// two names, free to drift apart.

import { PLATFORM_LABELS, platformKey } from "$lib/theme/platformColors";
import { oauthStore } from "$lib/stores/oauthStore.svelte";
import type { StreamProfileInfo } from "$lib/api/bridge";

/** A fresh profile can have an empty label AND an empty platform (platform is
 *  derived from the service and may lag), which collapsed a row to a blank line.
 *  Fall back label -> platform -> a literal so a row always names itself. */
export function profileName(p: StreamProfileInfo): string {
  return p.label?.trim() || p.platform?.trim() || "Untitled profile";
}

/** The destination detail line: the full service string (e.g. "YouTube - RTMPS"),
 *  falling back to the brand label or the raw platform. */
export function platformLabel(p: StreamProfileInfo): string {
  const key = platformKey(p.platform);
  return p.serviceLabel?.trim() || PLATFORM_LABELS[key] || p.platform || "Unknown";
}

/** The picture for a profile, or "" to fall back to the monogram. The claimed
 *  target's own picture wins: several profiles share one account, and on a
 *  platform with targets (Facebook Pages) the account avatar is the same image
 *  for all of them, so the account's would make two destinations look identical.
 *  It is empty for providers without targets and for claims predating avatars,
 *  hence the account fallback. Reads the oauth store, so a consumer still has to
 *  subscribe (`$effect(() => oauthStore.subscribe())`) for it to resolve. */
export function profileAvatarUrl(p: StreamProfileInfo): string {
  return p.targetAvatarUrl || (oauthStore.connectedStatusForAccount(p.accountId)?.avatarUrl ?? "");
}
