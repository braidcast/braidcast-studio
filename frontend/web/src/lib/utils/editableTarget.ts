// Whether an event's target is a text-editing surface. A global or overlay-wide key
// handler asks this before claiming a key, so native per-field behavior -- caret
// movement, typing, per-field undo -- keeps working while a field has focus.

export function isEditable(t: EventTarget | null): boolean {
  if (!(t instanceof HTMLElement)) {
    return false;
  }
  const tag = t.tagName;
  return tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT" || t.isContentEditable;
}
