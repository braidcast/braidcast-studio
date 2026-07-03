// A bump counter that ticks on every Dockview layout change (reorder, float, move,
// resize, add/remove). Native-preview docks watch it to re-assert their overlay
// rect: a position-only reorder (two equal-size panels swapping places) does NOT
// fire ResizeObserver, so without this the native child HWNDs would stay at their
// old screen positions while the DOM slots swapped — the previews appear switched.
export const dockLayout = $state<{ v: number }>({ v: 0 });

export function bumpDockLayout(): void {
  dockLayout.v++;
}
