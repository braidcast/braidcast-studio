import type { LabeledOption, OAuthProviderField } from "$lib/api/bridge";

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
// decides whether a control shows its inherit ghost. It answers "does this layer STATE
// anything", never "is there anything in it". A bool that has been set (even to false)
// counts as present; everything else is empty when blank/missing.
//
// A list value is the one place those two questions come apart, and the backend already
// keeps them apart — provider.cpp's readback comparison states the rule outright: an empty
// list is a real assertion ("no tags") and an absent key is the absence of one. Every
// provider applies it that way, acting on the key when it is present and assigning whatever
// it holds (Twitch's content labels are sent as the full set, each one on or off). So an
// array is a stated value whatever its length, and only a missing one is unset — otherwise
// clearing an inheriting channel's tags reads as "inherit" and every inherited tag springs
// straight back, and unticking a channel's last label is never sent at all.
export function isEmptyVal(type: string, v: unknown): boolean {
  if (isListType(type)) {
    return !Array.isArray(v);
  }
  switch (type) {
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

// "Nothing IN it", as opposed to isEmptyVal's "nothing stated here". The two differ for
// exactly the list types — a value of [] states an empty list while carrying nothing — and
// this one is built on the other so no further type can drift between them.
//
// For a value ARRIVING from outside: a platform reporting an empty tag list is reporting
// that it holds none, which is an observation, not an instruction to send one. Only a value
// the user put in this dialog is an instruction, and that path reads isEmptyVal.
export function isBlankVal(type: string, v: unknown): boolean {
  return isEmptyVal(type, v) || (isListType(type) && Array.isArray(v) && v.length === 0);
}

// The field types whose value is a list of strings: free tags, and a fixed label set.
export function isListType(type: string): boolean {
  return type === "tags" || type === "labelset";
}

// One key for a (scope, key) pair -- an inherit bucket and a field key, or an account and a
// field key -- wherever such pairs are kept in a Set or Map. The scope half may hold colons
// (an account id is "provider:user"), but the key half never does: it is a descriptor field
// key, a developer-owned identifier. So the last colon always marks where the key starts,
// and two different pairs can never join to the same string.
export function slotKey(scope: string, key: string): string {
  return scope + "::" + key;
}

// Type-aware value equality, used to tell a genuine per-channel divergence from a value
// that merely echoes the shared default. Plain === is wrong for category (two equal
// {id,name} objects are distinct references) and tags (array identity), which would
// reintroduce the spurious "overrides shared" chip.
export function valuesEqual(type: string, a: unknown, b: unknown): boolean {
  if (type === "category") {
    const ai = a && typeof a === "object" ? (a as { id?: string }).id : undefined;
    const bi = b && typeof b === "object" ? (b as { id?: string }).id : undefined;
    return ai === bi;
  }
  if (isListType(type)) {
    const aa = Array.isArray(a) ? [...(a as unknown[])].sort() : [];
    const bb = Array.isArray(b) ? [...(b as unknown[])].sort() : [];
    return aa.length === bb.length && aa.every((v, i) => v === bb[i]);
  }
  return a === b;
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
// the only reading that cannot push a value to a provider whose rules forbid it. The host
// reads channel scope the same way when it decides which fields an older preset could not
// hold (StreamInfoPresetStore::FieldsLegacyRowsNeverHeld).
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

/** One platform's own defaults block: its bucket, the fields that inherit from it, and the
 * connected account a field needing one (a category search) is looked up through. */
export interface ProviderDefaultsBlock<P> {
  provider: P;
  bucket: string;
  accountId: string;
  channelCount: number;
  fields: OAuthProviderField[];
}

/** The "<Platform> defaults" blocks: one per platform with MORE than one connected channel,
 * in the order its first channel appears, holding the simple fields whose nearest layer is
 * that platform's own bucket (inheritLayers), first channel's descriptor winning a key. With
 * one channel the bucket is still there, but a block over a single card says nothing that
 * card does not, so it is left out. Advanced and per-destination fields stay on the cards. */
export function providerDefaultsBlocks<P extends { id: string; fields: OAuthProviderField[] }>(
  connected: readonly { accountId: string; provider: P | null }[],
): ProviderDefaultsBlock<P>[] {
  const byProvider = new Map<string, { first: { accountId: string; provider: P | null }; count: number }>();
  for (const c of connected) {
    if (!c.provider) {
      continue;
    }
    const seen = byProvider.get(c.provider.id);
    if (seen) {
      seen.count++;
    } else {
      byProvider.set(c.provider.id, { first: c, count: 1 });
    }
  }
  const blocks: ProviderDefaultsBlock<P>[] = [];
  for (const [providerId, { first, count }] of byProvider) {
    const provider = first.provider;
    if (count < 2 || !provider) {
      continue;
    }
    const bucket = providerLayer(providerId);
    const keys = new Set<string>();
    const fields = provider.fields.filter((f) => {
      if (f.tier === "advanced" || inheritLayers(f, providerId)[0] !== bucket || keys.has(f.key)) {
        return false;
      }
      keys.add(f.key);
      return true;
    });
    if (fields.length > 0) {
      blocks.push({ provider, bucket, accountId: first.accountId, channelCount: count, fields });
    }
  }
  return blocks;
}
