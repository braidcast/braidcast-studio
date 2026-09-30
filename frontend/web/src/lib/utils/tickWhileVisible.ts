import { nowTickStore } from "$lib/stores/nowTickStore.svelte";
import { whileVisible } from "$lib/utils/whileVisible";

// The relative timestamps only need to tick while a pane is actually on screen. The
// clock's own window-visibility gating lives in nowTickStore; this only decides
// whether the pane it is attached to holds a ref on it.
export function tickWhileVisible(node: HTMLElement): { destroy(): void } {
  return whileVisible(node, () => nowTickStore.subscribe());
}
