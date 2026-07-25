// Single source for the platform / event-type accent palettes that were
// re-declared per consumer (MultichatDock, EventsDock, MonitorPage,
// overlays/PreviewPane). Keyed by plain string so this module stays decoupled
// from the bridge's ChatPlatform/EventType unions; consumers index with their
// typed keys directly.

/** Brand accent per chat platform (dot/tag/chip color). */
export const PLATFORM_COLORS: Record<string, string> = {
  twitch: "#a970ff",
  youtube: "#ff4e45",
  kick: "#53fc18",
};

/** Display label per chat platform. */
export const PLATFORM_LABELS: Record<string, string> = {
  twitch: "Twitch",
  youtube: "YouTube",
  kick: "Kick",
};

/** Stable platform order so chip rows / filters never reshuffle. */
export const PLATFORM_ORDER = ["twitch", "youtube", "kick"] as const;

/**
 * Normalize any platform-ish string to the keys the maps above use. Callers hold the
 * value under several shapes -- an OAuth `providerId` ("youtube"), a
 * `StreamProfileInfo.platform` display prefix ("YouTube"), a raw service word ("RTMP")
 * -- and each was lowercasing at the lookup site. Naming it keeps the keying
 * convention in the module that owns the keys.
 */
export function platformKey(raw: string): string {
  return raw.trim().toLowerCase();
}

/**
 * The cutout inside a brand mark (YouTube's play triangle). Part of the brand mark
 * rather than the theme palette, so it stays white in light mode -- `--color-text`
 * would invert to near-black and the mark would stop reading as the logo.
 */
export const PLATFORM_MARK_KNOCKOUT = "#ffffff";

/**
 * Accent color per normalized event type. follow=blue; sub/resub=purple;
 * subgift/member=gold; cheer=teal (bits); raid=orange; superchat/supersticker=
 * green (money).
 */
export const EVENT_TYPE_COLORS: Record<string, string> = {
  follow: "#3ea6ff",
  sub: "#a970ff",
  resub: "#a970ff",
  subgift: "#ffb62c",
  cheer: "#1fd1c3",
  raid: "#ff8a3d",
  superchat: "#2ecc71",
  supersticker: "#2ecc71",
  member: "#ffb62c",
};

/** Human label per normalized event type. */
export const EVENT_TYPE_LABELS: Record<string, string> = {
  follow: "Follow",
  sub: "Sub",
  resub: "Resub",
  subgift: "Gift Sub",
  cheer: "Cheer",
  raid: "Raid",
  superchat: "Super Chat",
  supersticker: "Super Sticker",
  member: "Member",
};
