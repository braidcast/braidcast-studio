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
