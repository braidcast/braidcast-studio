// The Fields system's type table and row identity — the two things the overlay field
// editor dispatches on.
//
// ONE entry per field type carries everything the editor needs: the label the Type
// picker shows, the inline value control the row renders, the schema editor the row's
// expansion adds, and the value/extras seed applied when a field switches to it. Adding
// a type is a row in FIELD_TYPES, not a new branch in FieldsDesigner.

import type { OverlayField } from "$lib/api/bridge";

export type FieldType = OverlayField["type"];

/** The inline value control a row renders. Several types share one (text and font, the
 * two uploads), which is why the row dispatches on this rather than on the type. */
export type ValueControl = "text" | "number" | "slider" | "color" | "select" | "switch" | "upload";

/** The schema editor the expansion adds under Key/Label/Type; null = none. */
export type SchemaExtras = "range" | "options" | null;

interface FieldTypeBase {
  label: string;
  extras: SchemaExtras;
  /** The value plus type-specific extras seeded when a field becomes this type. */
  defaults: () => Partial<OverlayField>;
}

/** A union rather than one interface with optionals, so `spec.accept` is reachable
 * exactly where `spec.control === "upload"` and nowhere else. */
export type FieldTypeSpec =
  | (FieldTypeBase & { control: "upload"; accept: string; uploadKind: "image" | "sound" })
  | (FieldTypeBase & { control: "text"; placeholder?: string })
  | (FieldTypeBase & { control: Exclude<ValueControl, "upload" | "text"> });

export const FIELD_TYPES: Record<FieldType, FieldTypeSpec> = {
  text: {
    label: "Text",
    control: "text",
    extras: null,
    defaults: () => ({ default: "", value: "" }),
  },
  number: {
    label: "Number",
    control: "number",
    extras: null,
    defaults: () => ({ default: 0, value: 0 }),
  },
  color: {
    label: "Color",
    control: "color",
    extras: null,
    defaults: () => ({ default: "#ffffff", value: "#ffffff" }),
  },
  dropdown: {
    label: "Dropdown",
    control: "select",
    extras: "options",
    defaults: () => ({ options: [{ value: "a", label: "Option A" }], default: "a", value: "a" }),
  },
  checkbox: {
    label: "Checkbox",
    control: "switch",
    extras: null,
    defaults: () => ({ default: false, value: false }),
  },
  slider: {
    label: "Slider",
    control: "slider",
    extras: "range",
    defaults: () => ({ default: 0, value: 0, min: 0, max: 100, step: 1 }),
  },
  "image-upload": {
    label: "Image",
    control: "upload",
    extras: null,
    accept: "image/*",
    uploadKind: "image",
    defaults: () => ({ default: "", value: "" }),
  },
  "sound-upload": {
    label: "Sound",
    control: "upload",
    extras: null,
    accept: "audio/*",
    uploadKind: "sound",
    defaults: () => ({ default: "", value: "" }),
  },
  font: {
    label: "Font",
    control: "text",
    extras: null,
    placeholder: "CSS font-family",
    defaults: () => ({ default: "", value: "" }),
  },
};

/** Picker order = declaration order. */
export const FIELD_TYPE_ORDER = Object.keys(FIELD_TYPES) as FieldType[];

/** The patch that switches a field to `type`: strip every type-specific extra, then seed
 * the incoming type's own. Both halves have to happen together, or a leftover `options`
 * or `min` outlives the type that meant something by it and the widget's fieldData and
 * the value control disagree. */
export function typeChangePatch(type: FieldType): Partial<OverlayField> {
  return {
    type,
    options: undefined,
    min: undefined,
    max: undefined,
    step: undefined,
    ...FIELD_TYPES[type].defaults(),
  };
}

/** A fresh text field, seeded from the same table everything else reads. The literal
 * `default`/`value` are not redundant with the spread: `defaults()` is typed
 * `Partial<OverlayField>`, so they are what satisfies the return type. */
export function newField(key: string): OverlayField {
  return { key, label: "Field", type: "text", default: "", value: "", ...FIELD_TYPES.text.defaults() };
}

// --- row identity ----------------------------------------------------------------
// `key` is user-editable and duplicable, so it cannot identify a row. Rows therefore
// carry a client-side `id`, minted here and never persisted: OverlaysPage hydrates on
// the way in from overlays.get and projects it back out in fieldsForWire() before
// overlays.update, so the stored document is byte-identical to one written without ids.

/** An OverlayField with its editor-session identity attached. */
export interface DesignerField extends OverlayField {
  id: string;
}

let seq = 0;

export function newFieldId(): string {
  seq += 1;
  return `fld${seq}`;
}

function hasId(f: OverlayField): f is DesignerField {
  return typeof (f as Partial<DesignerField>).id === "string" && (f as DesignerField).id !== "";
}

/** Identity-preserving: an already-hydrated array comes back as itself, so hydrating on
 * every render costs nothing and never re-mints an id out from under an open row. */
export function withFieldIds(fields: OverlayField[]): DesignerField[] {
  return fields.every(hasId) ? fields : fields.map((f) => (hasId(f) ? f : { ...f, id: newFieldId() }));
}

/** The wire form of the field list. Written as an explicit projection of the persisted
 * properties rather than as a `delete id`, so a client-only property added later cannot
 * reach overlays.update by being forgotten here. */
export function fieldsForWire(fields: OverlayField[]): OverlayField[] {
  return fields.map((f) => ({
    key: f.key,
    type: f.type,
    label: f.label,
    default: f.default,
    value: f.value,
    options: f.options,
    min: f.min,
    max: f.max,
    step: f.step,
  }));
}
