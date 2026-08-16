import type { OAuthProviderField, TagCharset } from "$lib/api/bridge";

// The rules a tag list obeys, apart from the control that renders it: what one tag may
// look like, which of an incoming batch may join, and how a list crosses the clipboard.
// Held here rather than in the component so the parse and the join that reverse each
// other sit in one file — a copy that a paste cannot read back is a round trip nobody
// checks until it is broken.

// The limits a caller may state, which is exactly the descriptor's own subset — the same
// key names, so nothing translates between the wire and the control and there is no
// mapping to fall out of step. A provider renaming one of these keys fails to compile
// here rather than silently stopping enforcement. Every one is optional: a limit the
// caller does not state is not enforced.
export type TagLimits = Pick<OAuthProviderField, "maxTags" | "maxTagChars" | "maxTotalChars" | "tagCharset">;

// One entry per character rule: the test and the words for it, together, because the
// sentence a user is shown has to be the sentence of the rule that turned them away.
// Adding a rule is an entry here plus the TagCharset union member it keys on.
const TAG_CHARSETS: Record<TagCharset, { fits: (tag: string) => boolean; text: string }> = {
  "lowercase-alnum": {
    fits: (tag) => /^[a-z0-9]+$/.test(tag),
    text: "lowercase letters and numbers only, no spaces",
  },
};

// The two characters that end one tag and start the next, wherever text arrives in bulk.
const TAG_SEPARATORS = /[,\n]/;

/** The list with repeats dropped, first occurrence kept.
 *
 * A second copy of a tag states nothing a first does not — every path that admits tags
 * already collapses one silently — so this hides nothing a user could act on. It is
 * applied to what arrives from OUTSIDE as well: a chip list is keyed by the tag itself,
 * and a repeat coming off a provider read, the remembered store, or the hand-editable
 * preset file would be a duplicate key rather than a visible repeat, which is a render
 * that throws rather than one that looks wrong. */
export function uniqueTags(tags: string[]): string[] {
  return tags.filter((t, i) => tags.indexOf(t) === i);
}

/** The tags in a run of text. Splits rather than taking the text whole: a tag list is
 * almost always pasted, and every source of one — a platform's own field, a keyword tool,
 * an old description — hands it over comma-separated. Drops blanks and repeats, so no
 * caller has to. */
export function parseTags(text: string): string[] {
  return uniqueTags(
    text
      .split(TAG_SEPARATORS)
      .map((raw) => raw.trim())
      .filter((t) => t !== ""),
  );
}

/** One run of text carrying every tag, in the shape parseTags reads back.
 *
 * The reverse of parseTags for anything parseTags produced, since it splits on exactly the
 * separators this joins with. NOT lossless in general: a list can also arrive from outside
 * — a provider's own channel read, the remembered store, the hand-editable preset file —
 * and nothing there forbids a tag containing a comma. Such a tag joins out and parses back
 * as two, and no comma-separated shape can carry it. */
export function joinTags(tags: string[]): string {
  return tags.join(", ");
}

/** Does this text carry more than one tag? Text that does not is left to the browser's
 * own paste, so pasting a single tag still lands in the box the caret is in and can be
 * edited before it is committed. */
export function hasTagSeparator(text: string): boolean {
  return TAG_SEPARATORS.test(text);
}

// What one tag must look like, in words. Shown while the box has focus and repeated in the
// message when a tag is turned away, so the rule taught is the rule enforced. "" when the
// caller constrains nothing about a single tag.
export function tagRuleText(limits: TagLimits): string {
  const parts: string[] = [];
  const charset = limits.tagCharset ? TAG_CHARSETS[limits.tagCharset] : undefined;
  if (charset) {
    parts.push(charset.text);
  }
  if (limits.maxTagChars !== undefined) {
    parts.push(`up to ${limits.maxTagChars} characters`);
  }
  return parts.join(", ");
}

/** How many characters a list of tags spends against a maxTotalChars budget. Exported
 * because the running readout and the budget admitTags actually enforces have to be the
 * same number — two reducers is a control counting down to a limit it does not apply. */
export function tagCharsUsed(tags: string[]): number {
  return tags.reduce((n, t) => n + t.length, 0);
}

export interface TagAdmission {
  /** The incoming tags that may join, in the order given. */
  accepted: string[];
  /** The ones that may not, in the order given, for the caller to hand back to the user
   * rather than drop — a tag that vanishes without trace is worse than one refused. */
  rejected: string[];
  /** Why, in one sentence naming the rule. "" when nothing was turned away. */
  note: string;
}

// Which of `incoming` may join `kept`, and why the rest may not. The single gate every
// path into a tag list goes through — typing, pasting, and editing a tag in place all
// admit through here, so a rule cannot hold on one route and not another.
//
// A duplicate of something already kept is absorbed silently rather than refused: it is
// the existing collapse behavior, it consumes no slot, and it is not a limit anyone broke.
//
// `platform` is the display name of the one platform these limits belong to, and it is
// named in the refusal because the rules differ per platform: a list Twitch caps at ten
// lowercase words is a list YouTube takes whole. A sentence that says only "tags must be
// lowercase" leaves the streamer to guess who is refusing. "" when the caller has no
// descriptor and therefore no platform either.
export function admitTags(
  kept: string[],
  incoming: string[],
  limits: TagLimits,
  platform = "",
): TagAdmission {
  const charset = limits.tagCharset ? TAG_CHARSETS[limits.tagCharset] : undefined;
  const accepted: string[] = [];
  const rejected: string[] = [];
  let brokeRule = false;
  let brokeCount = false;
  let brokeTotal = false;
  let used = tagCharsUsed(kept);

  for (const tag of incoming) {
    if (kept.includes(tag) || accepted.includes(tag)) {
      continue;
    }
    if ((charset && !charset.fits(tag)) || (limits.maxTagChars !== undefined && tag.length > limits.maxTagChars)) {
      rejected.push(tag);
      brokeRule = true;
      continue;
    }
    if (limits.maxTags !== undefined && kept.length + accepted.length >= limits.maxTags) {
      rejected.push(tag);
      brokeCount = true;
      continue;
    }
    if (limits.maxTotalChars !== undefined && used + tag.length > limits.maxTotalChars) {
      rejected.push(tag);
      brokeTotal = true;
      continue;
    }
    accepted.push(tag);
    used += tag.length;
  }

  // Every reason is worded to read with or without the platform in front of it, so an
  // unnamed platform costs a subject rather than a broken sentence.
  const who = platform === "" ? "" : platform + " ";
  const reasons: string[] = [];
  if (brokeRule) {
    reasons.push(`${who}tags must be ${tagRuleText(limits)}`);
  }
  if (brokeCount) {
    reasons.push(`${who}tags are capped at ${limits.maxTags}`);
  }
  if (brokeTotal) {
    reasons.push(`${who}tags may total ${limits.maxTotalChars} characters`);
  }
  // Quotes the single refused tag so the sentence names the text still sitting in the box;
  // a longer list would make the sentence unreadable, so that one counts instead.
  const lead = rejected.length === 1 ? `"${rejected[0]}" doesn't fit` : `${rejected.length} tags don't fit`;
  return { accepted, rejected, note: reasons.length === 0 ? "" : `${lead} — ${reasons.join("; ")}.` };
}

/** How many tags, in words, for a sentence that counts them. */
export function tagCountLabel(n: number): string {
  return `${n} tag${n === 1 ? "" : "s"}`;
}
