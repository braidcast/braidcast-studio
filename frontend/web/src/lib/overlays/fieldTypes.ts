// The Fields system's type table and row identity — the two things the overlay field
// panel dispatches on.
//
// A widget's fields are fixed by its type; the panel edits their values only. ONE entry
// per field type therefore carries just what a value control needs: which control to
// render, and the control's own type-specific extras. Adding a type is a row in
// FIELD_TYPES, not a new branch in FieldsPanel.

import type { OverlayField } from "$lib/api/bridge";

type FieldType = OverlayField["type"];

/** The value control a row renders. Several types share one (text and font, the two
 * uploads), which is why the row dispatches on this rather than on the type. */
type ValueControl = "text" | "number" | "slider" | "color" | "select" | "switch" | "upload";

/** A union rather than one interface with optionals, so `spec.accept` is reachable
 * exactly where `spec.control === "upload"` and nowhere else. */
export type FieldTypeSpec =
  | { control: "upload"; accept: string; uploadKind: "image" | "sound" }
  | { control: "text"; placeholder?: string }
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
  font: { control: "text", placeholder: "CSS font-family" },
};

// --- row identity ----------------------------------------------------------------
// `key` cannot identify a row: a legacy or hand-edited document may carry the same key
// twice, which a keyed block reads as a collision. Rows therefore carry a client-side
// `id`, minted here and never persisted: OverlaysPage hydrates on the way in from
// overlays.get and projects it back out in fieldsForWire() before overlays.update, so the
// stored document is byte-identical to one written without ids.

/** An OverlayField with its editor-session identity attached. */
export interface EditorField extends OverlayField {
  id: string;
}

let seq = 0;

function newFieldId(): string {
  seq += 1;
  return `fld${seq}`;
}

function hasId(f: OverlayField): f is EditorField {
  return typeof (f as Partial<EditorField>).id === "string" && (f as EditorField).id !== "";
}

/** Identity-preserving: an already-hydrated array comes back as itself, so hydrating on
 * every render costs nothing and never re-mints an id out from under a row. */
export function withFieldIds(fields: OverlayField[]): EditorField[] {
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
