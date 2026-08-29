// The Fields system's type table, and the one rule for turning an edited value into a
// stored override.
//
// A widget's fields are fixed by its schema; the panel edits their values only, and the
// values live in the widget's `settings` map rather than on the field. ONE entry per field
// type therefore carries just what a value control needs: which control to render, and the
// control's own type-specific extras. Adding a type is a row in FIELD_TYPES, not a new
// branch in FieldsPanel.

import type { OverlayField } from "$lib/api/bridge";
import { isPlainObject } from "$lib/utils/plainObject";

type FieldType = OverlayField["type"];

/** The value control a row renders. Several types share one (text and font, the two
 * uploads), which is why the row dispatches on this rather than on the type. */
type ValueControl = "text" | "number" | "slider" | "color" | "select" | "switch" | "upload" | "textstyle";

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
  textstyle: { control: "textstyle" },
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

/** Whether a spec needs the installed font families available to it at all. Distinct from
 * the predicate above, which answers only whether a text input carries the `list=`: the
 * text-style control holds a font input of its own, so a schema with no plain font field
 * can still need the list mounted. */
export function needsFontList(spec: FieldTypeSpec): boolean {
  return suggestsFonts(spec) || spec.control === "textstyle";
}

/** Why a `textstyle` field must declare `"default": {}`, and what a schema saying anything
 * else is told. The control stores the whole style object under ONE key, so its first edit
 * writes back every sub-property the default supplied — a full copy, which detaches that
 * widget from later changes to those defaults permanently. There is no way to express
 * "took the shipped size, overrode only the color" inside a single stored key, so a
 * non-empty default is refused outright rather than half-supported: the panel shows this
 * message in place of the control, so nothing is ever STORED in the broken shape.
 *
 * That is a claim about storage and not about the stream. The host merges schema defaults
 * on its way to the served page (MergeSettings), so a shipped `"default":{"fontSize":32}`
 * still reaches viewers as `font-size:32px` while the panel refuses to edit it. Deliberate
 * -- the fault is a schema bug to fix at the source, and blanking the widget's styling to
 * announce it would be a worse outcome than rendering what the schema asked for. */
export function textStyleDefaultFault(field: OverlayField): string | null {
  if (specFor(field).control !== "textstyle" || (isPlainObject(field.default) && Object.keys(field.default).length === 0)) {
    return null;
  }
  return `Schema error: "${field.key}" must declare "default": {}. A text-style field cannot ship values in its default — the first edit would copy them into this widget and detach it from later changes to them.`;
}

// --- overrides ---------------------------------------------------------------------
// `settings` holds overrides ONLY. A key that is absent resolves to the schema's default,
// and stays tied to it: change the default in a later build and every widget that never
// overrode the key follows. Storing a copy of the default instead would silently opt the
// widget out of that, which is the whole reason the split exists — so the write path below
// is the single place that decides, and no call site may set a key directly.

/** Structural equality over the shapes a setting can take: primitives, arrays, and plain
 * objects, all of which arrive as parsed JSON and so cannot be cyclic. Reference equality
 * would answer false for every edit of a nested value (the text-style control rebuilds its
 * object on each keystroke rather than mutating the schema's), which would store a full
 * copy of the default on the first change and quietly detach the widget from later
 * improvements to it -- the exact outcome the overrides-only split exists to prevent. */
function sameValue(a: unknown, b: unknown): boolean {
  if (a === b) {
    return true;
  }
  if (Array.isArray(a) || Array.isArray(b)) {
    return Array.isArray(a) && Array.isArray(b) && a.length === b.length && a.every((v, i) => sameValue(v, b[i]));
  }
  if (!isPlainObject(a) || !isPlainObject(b)) {
    return false;
  }
  const keys = Object.keys(a);
  return keys.length === Object.keys(b).length && keys.every((k) => Object.hasOwn(b, k) && sameValue(a[k], b[k]));
}

/** Whether a value is indistinguishable from the schema default, and therefore not worth
 * storing. */
function isDefaultValue(field: OverlayField, value: unknown): boolean {
  return sameValue(value, field.default);
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
