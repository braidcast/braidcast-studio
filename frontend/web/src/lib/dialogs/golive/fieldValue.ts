import type { LabeledOption, OAuthProviderField, TagCharset } from "$lib/api/bridge";

// Descriptor-value rules shared by GoLiveFieldInput (which renders a field) and
// GoLiveModal (which owns the model behind it and pushes it to the host). Both have to
// read a value the same way: a second copy of these rules is a control showing one
// thing while the host receives another.

// Coerce a bare string to a LabeledOption so a mixed/legacy provider never renders
// "[object Object]".
export function normOpt(o: unknown): LabeledOption {
  return typeof o === "string" ? { value: o, label: o } : (o as LabeledOption);
}

// Does this field address WHERE one stream posts, rather than describe what it says?
// Such a field belongs to the individual stream: two streams on one account deliberately
// targeting two Facebook Pages is the case it exists for, so its value is edited, pushed
// and remembered per stream and has no channel layer to fall back to.
export function isPerDestination(field: OAuthProviderField): boolean {
  return field.perDestination === true;
}

// "Empty" per descriptor type — the inheritance/omission predicate, and the same test that
// decides whether a control shows its inherit ghost. A bool that has been set (even to
// false) counts as present; everything else is empty when blank/missing.
export function isEmptyVal(type: string, v: unknown): boolean {
  switch (type) {
    case "tags":
    case "labelset":
      return !Array.isArray(v) || v.length === 0;
    // A provider that always reports a category has no null to report an unset one with, so
    // it sends a blank id instead (Kick's id is a wire integer). A blank id is that unset
    // state: counting it as held would block the layer below, suppress the inherit cue, and
    // push an id naming no category.
    case "category": {
      const id = (v as { id?: unknown } | null | undefined)?.id;
      return typeof id !== "string" || id.trim() === "";
    }
    case "bool":
      return v === undefined || v === null;
    default:
      return typeof v !== "string" || v.trim() === "";
  }
}

// How far one field's value may legitimately travel. Reach is a property of the field's
// VALUE SPACE, not of the dialog: a category id means something different on every
// platform, and a tag Twitch's applyMetadata hard-rejects (lowercase alphanumeric, no
// spaces) is a perfectly ordinary tag on Kick and YouTube — so a value held once for
// every provider is a value at least one provider will refuse.
export type FieldScope = "all" | "provider" | "channel";

// The cross-provider layer.
export const ALL_LAYER = "all";

// Prefixed so no provider id can collide with the cross-provider key. Not exported: a
// caller that needs a bucket is given one by inheritLayers, never builds it.
function providerLayer(providerId: string): string {
  return `provider:${providerId}`;
}

// A descriptor that names no scope keeps its value to the channel: of the three, that is
// the only reading that cannot push a value to a provider whose rules forbid it.
export function fieldScope(field: OAuthProviderField): FieldScope {
  return field.scope === "all" || field.scope === "provider" ? field.scope : "channel";
}

// The layers under a field's channel control, nearest first — the buckets a channel
// holding nothing of its own inherits from, in the order to consult them. A
// provider-scoped field deliberately does NOT fall through to the cross-provider layer:
// reaching it is the very thing its scope rules out.
//
// One ordering, held here, because a control that ghosts one layer while the push reads
// another shows the user a value the provider never receives.
export function inheritLayers(field: OAuthProviderField, providerId: string): string[] {
  // A per-destination field addresses this one stream, so it has no layer below it at all.
  if (isPerDestination(field)) {
    return [];
  }
  switch (fieldScope(field)) {
    case "all":
      return [ALL_LAYER];
    case "provider":
      return [providerLayer(providerId)];
    default:
      return [];
  }
}

// The tag limits a provider declares, which is exactly the descriptor's own subset — the
// same key names, so nothing translates between the wire and the control and there is no
// mapping to fall out of step. Every one is optional: a limit the provider does not state
// is not enforced, which is also what a call site with no descriptor at all gets.
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

// What one tag must look like, in words. Shown while the box has focus and repeated in the
// message when a tag is turned away, so the rule taught is the rule enforced. "" when the
// provider constrains nothing about a single tag.
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

// How many characters a list of tags spends against a maxTotalChars budget.
function tagCharsUsed(tags: string[]): number {
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
export function admitTags(kept: string[], incoming: string[], limits: TagLimits): TagAdmission {
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

  const reasons: string[] = [];
  if (brokeRule) {
    reasons.push(`tags must be ${tagRuleText(limits)}`);
  }
  if (brokeCount) {
    reasons.push(`only ${limits.maxTags} tags allowed`);
  }
  if (brokeTotal) {
    reasons.push(`tags may total ${limits.maxTotalChars} characters`);
  }
  // Quotes the single refused tag so the sentence names the text still sitting in the box;
  // a longer list would make the sentence unreadable, so that one counts instead.
  const lead = rejected.length === 1 ? `"${rejected[0]}" doesn't fit` : `${rejected.length} tags don't fit`;
  return { accepted, rejected, note: reasons.length === 0 ? "" : `${lead} — ${reasons.join("; ")}.` };
}

// A `required` enum has no valid empty state. `inheritable` exempts an inherit layer,
// where empty means "take the layer below" rather than "unset".
export function isRequiredEnum(field: OAuthProviderField, inheritable: boolean): boolean {
  return field.type === "enum" && field.required === true && !inheritable;
}

// What a required enum actually stands for: the held value when it names a real option,
// else the descriptor default, else the first option. A <select> whose value matches no
// option lands on the FIRST one, which for privacy would read "Public" while the host
// received "private" — so every site that shows, hints at, or pushes the value resolves
// it through here.
//
// Resolved on READ, never written back into the model: a synthesized write is
// indistinguishable from the user's own choice, and the prefill that restores remembered
// values skips any field that already holds one.
export function resolveRequiredEnum(
  field: OAuthProviderField,
  value: unknown,
  inheritable = false,
): string {
  const str = typeof value === "string" ? value : "";
  if (!isRequiredEnum(field, inheritable)) {
    return str;
  }
  const values = (field.options ?? []).map((o) => normOpt(o).value);
  if (values.includes(str)) {
    return str;
  }
  const dflt = typeof field.default === "string" ? field.default : "";
  return values.includes(dflt) ? dflt : (values[0] ?? "");
}
