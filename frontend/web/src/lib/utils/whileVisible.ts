import type { Unsubscribe } from "$lib/api/bridge";

// Hold a ref-counted subscription only while the element it is attached to is on
// screen. Dockview hides an inactive tab by taking the panel's content out of layout
// rather than unmounting it, so a dock keeps running behind a tab switch -- an
// IntersectionObserver on the dock's own root catches exactly that (it reports
// not-intersecting for an element with no layout box) without the dock needing to
// know anything about the docking library. Window-level visibility (minimized,
// occluded) is the subscribed store's own business, since IntersectionObserver does
// not see it.
//
// `hold` takes the reference and returns its release; it runs each time the element
// becomes visible, and the release runs when it stops being visible or unmounts.
export function whileVisible(node: HTMLElement, hold: () => Unsubscribe): { destroy(): void } {
  let release: Unsubscribe | null = null;
  const observer = new IntersectionObserver((entries) => {
    // Chromium can batch several records for one target into a single callback when
    // changes accumulate; the most recent one is the one that reflects current state.
    const visible = entries[entries.length - 1]?.isIntersecting ?? false;
    if (visible && !release) {
      release = hold();
    } else if (!visible && release) {
      release();
      release = null;
    }
  });
  observer.observe(node);
  return {
    destroy() {
      observer.disconnect();
      release?.();
      release = null;
    },
  };
}
