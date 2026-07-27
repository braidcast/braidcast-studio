// Svelte actions for a field that is created by the interaction that needs it: an
// inline rename input, a reveal-on-click filter box, a prompt dialog. Mount is the
// only moment the element exists, so the focus rides on the action rather than an
// effect. `selectOnMount` pre-selects so typing replaces the seeded value, which is
// what every rename/prompt field wants; `focusOnMount` leaves the caret alone for
// fields whose existing text is meant to be extended.

export function focusOnMount(node: HTMLElement): void {
  node.focus();
}

export function selectOnMount(node: HTMLInputElement | HTMLSelectElement): void {
  node.focus();
  if (node instanceof HTMLInputElement) {
    node.select();
  }
}
