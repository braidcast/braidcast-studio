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
// A tags value is the one place those two questions come apart, and the backend already
// keeps them apart — provider.cpp's readback comparison states the rule outright: an empty
// list is a real assertion ("no tags") and an absent key is the absence of one. Every
// provider applies it that way, acting on the `tags` key when it is present and assigning
// whatever it holds. So an array is a stated value whatever its length, and only a missing
// one is unset — otherwise clearing an inheriting channel's tags reads as "inherit" and
// every inherited tag springs straight back.
export function isEmptyVal(type: string, v: unknown): boolean {
  switch (type) {
    case "tags":
      return !Array.isArray(v);
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

// "Nothing IN it", as opposed to isEmptyVal's "nothing stated here". The two differ for
// exactly one type — a tags value of [] states an empty list while carrying nothing — and
// this one is built on the other so no further type can drift between them.
//
// For a value ARRIVING from outside: a platform reporting an empty tag list is reporting
// that it holds none, which is an observation, not an instruction to send one. Only a value
// the user put in this dialog is an instruction, and that path reads isEmptyVal.
export function isBlankVal(type: string, v: unknown): boolean {
  return isEmptyVal(type, v) || (type === "tags" && Array.isArray(v) && v.length === 0);
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
  if (type === "tags" || type === "labelset") {
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
