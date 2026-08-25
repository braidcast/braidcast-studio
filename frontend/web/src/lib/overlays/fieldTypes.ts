// The Fields system's type table, and the one rule for turning an edited value into a
// stored override.
//
// A widget's fields are fixed by its schema; the panel edits their values only, and the
// values live in the widget's `settings` map rather than on the field. ONE entry per field
// type therefore carries just what a value control needs: which control to render, and the
// control's own type-specific extras. Adding a type is a row in FIELD_TYPES, not a new
// branch in FieldsPanel.

import type { OverlayField } from "$lib/api/bridge";

type FieldType = OverlayField["type"];

/** The value control a row renders. Several types share one (text and font, the two
 * uploads), which is why the row dispatches on this rather than on the type. */
type ValueControl = "text" | "number" | "slider" | "color" | "select" | "switch" | "upload";

/** A union rather than one interface with optionals, so `spec.accept` is reachable
 * exactly where `spec.control === "upload"` and nowhere else.
 *
 * `suggest` names a list the text control offers as autocomplete while staying free
 * text -- which is the only shape the font field can take, since its value is a whole
 * CSS stack ("Inter, system-ui, sans-serif") rather than one installed family. */
export type FieldTypeSpec =
  | { control: "upload"; accept: string; uploadKind: "image" | "sound" }
  | { control: "text"; placeholder?: string; suggest?: "font-family" }
  | { control: Exclude<ValueControl, "upload" | "text"> };

export const FIELD_TYPES: Record<FieldType, FieldTypeSpec> = {
  text: { control: "text" },
  number: { control: "number" },
  color: { control: "color" },
  dropdown: { control: "select" },
  checkbox: { control: "switch" },
  slider: { control: "slider" },
  "image-upload": { control: "upload", accept: "image/*", uploadKind: "image" },
  "sound-upload": { control: "upload", accept: "audio/*", uploadKind: "sound" },
  font: { control: "text", placeholder: "CSS font-family", suggest: "font-family" },
};

/** The spec a field renders through. An unrecognized `type` reads as text rather than
 * taking the panel down with it -- the value arrives from a passthrough JSON document the
 * user can hand-edit, so the Record's key type does not bind it at runtime. */
export function specFor(field: OverlayField): FieldTypeSpec {
  return FIELD_TYPES[field.type] ?? FIELD_TYPES.text;
}

/** Whether a spec's text control offers the installed font families. One predicate, so
 * the row that renders `list=` and the panel that decides whether to mount the list at
 * all cannot drift apart. */
export function suggestsFonts(spec: FieldTypeSpec): boolean {
  return spec.control === "text" && spec.suggest === "font-family";
}

// --- overrides ---------------------------------------------------------------------
// `settings` holds overrides ONLY. A key that is absent resolves to the schema's default,
// and stays tied to it: change the default in a later build and every widget that never
// overrode the key follows. Storing a copy of the default instead would silently opt the
// widget out of that, which is the whole reason the split exists — so the write path below
// is the single place that decides, and no call site may set a key directly.

/** Whether a value is indistinguishable from the schema default, and therefore not worth
 * storing. Strict equality is enough: every control writes a primitive of the same shape
 * as the default it came from (numbers from the number/slider inputs, booleans from the
 * switch, strings everywhere else). */
function isDefaultValue(field: OverlayField, value: unknown): boolean {
  return value === field.default;
}

/** The next override set after editing one field. Writing the default back REMOVES the
 * key, which is also how a "reset to default" is expressed — pass `field.default`. */
export function withOverride(
  settings: Record<string, unknown>,
  field: OverlayField,
  value: unknown,
): Record<string, unknown> {
  const next = { ...settings };
  if (isDefaultValue(field, value)) {
    delete next[field.key];
  } else {
    next[field.key] = value;
  }
  return next;
}
