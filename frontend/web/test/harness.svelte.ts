// Rune hosts for the tests: a runes class ($state/$derived/$effect) only runs its effects
// inside an effect root, and a plain test file cannot write runes itself. This module is
// compiled by the preload (svelteRunes.ts) like any other *.svelte.ts.

/** Runs `fn` inside a fresh effect root; the returned function tears the root down. */
export function root(fn: () => void): () => void {
  return $effect.root(fn);
}

/** A pre effect, as a component's template runs one: before the post effects of a flush. */
export function pre(fn: () => void): void {
  $effect.pre(fn);
}
