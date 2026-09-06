import { nowTickStore } from "$lib/stores/nowTickStore.svelte";

// The relative timestamps only need to tick while a pane is actually on screen.
// Dockview hides an inactive tab by setting the panel's content div to display:none
// rather than unmounting it, so a dock keeps running behind a tab switch -- an
// IntersectionObserver on the dock's own root catches exactly that (it reports
// not-intersecting for an element with no layout box) without the dock needing to
// know anything about the docking library. The clock's own window-visibility gating
// lives in nowTickStore; this only decides whether the pane it is attached to holds a
// ref on it.
export function tickWhileVisible(node: HTMLElement): { destroy(): void } {
  let offTick: (() => void) | null = null;
  const observer = new IntersectionObserver((entries) => {
    // Chromium can batch several records for one target into a single callback when
    // changes accumulate; the most recent one is the one that reflects current state.
    const visible = entries[entries.length - 1]?.isIntersecting ?? false;
    if (visible && !offTick) {
      offTick = nowTickStore.subscribe();
    } else if (!visible && offTick) {
      offTick();
      offTick = null;
    }
  });
  observer.observe(node);
  return {
    destroy() {
      observer.disconnect();
      offTick?.();
    },
  };
}
