// Per-canvas CanvasDock sub-pane sizes (embed height + scenes-column width, both in
// px), keyed by canvas uuid. Module-level so a value survives the dock's remount
// (Dockview tears the panel down when it is tab-stacked away or floated). `null`
// means "never dragged" -> the CSS fallback (154px / 42%) applies until first drag.

export interface CanvasPaneSizes {
  embedH: number | null;
  scenesW: number | null;
}

const store = new Map<string, CanvasPaneSizes>();

export function getPaneSizes(uuid: string): CanvasPaneSizes {
  return store.get(uuid) ?? { embedH: null, scenesW: null };
}

export function setEmbedH(uuid: string, embedH: number): void {
  store.set(uuid, { ...getPaneSizes(uuid), embedH });
}

export function setScenesW(uuid: string, scenesW: number): void {
  store.set(uuid, { ...getPaneSizes(uuid), scenesW });
}
