import type { SceneItem } from "$lib/api/bridge";

// The global SourcesDock (Default canvas, channel-0 path) publishes its current
// scene + selected item here so the app-level Ctrl+C / Ctrl+V handler can copy
// and paste without reaching into the dock. Keyboard copy/paste is intentionally
// scoped to this global selection; per-canvas docks use their context menus.
class SourceSelection {
  scene = $state<string | null>(null);
  item = $state<SceneItem | null>(null);
}

export const sourceSelection = new SourceSelection();
