// Per-canvas scene state. One store instance per CanvasPanel, keyed by canvas
// uuid, kept fresh by the `scenes.changed` push event filtered to that canvas.
//
// All bridge calls pass `{ canvas: uuid }`; the backend maps the Default canvas
// uuid to the global channel-0 path, so panels treat every canvas uniformly.
//
// A small registry mirrors each canvas's current-scene name so the shared
// SourcesPanel can resolve the focused canvas's scene without owning a store.

import { obs } from "./bridge";
import type { SceneInfo } from "./bridge";

export interface CanvasSceneStore {
  readonly state: { scenes: SceneInfo[]; current: string | null; loaded: boolean; error: string | null };
  refresh(): Promise<void>;
  dispose(): void;
}

// uuid -> current scene name, for the shared SourcesPanel to follow focus.
const currentByCanvas = $state<Record<string, string | null>>({});

/** Reactive: the current scene name of the given canvas, or null. */
export function currentSceneFor(uuid: string | null): string | null {
  if (!uuid) return null;
  return currentByCanvas[uuid] ?? null;
}

/** Create a scene store bound to one canvas. Caller must dispose() on unmount.
 * `isDefault` lets the Default canvas's store also react to global-path events,
 * which the backend emits with `canvas: null`. */
export function createCanvasSceneStore(uuid: string, isDefault: boolean): CanvasSceneStore {
  const state = $state<{
    scenes: SceneInfo[];
    current: string | null;
    loaded: boolean;
    error: string | null;
  }>({ scenes: [], current: null, loaded: false, error: null });

  async function refresh(): Promise<void> {
    try {
      const list = await obs.call("scenes.list", { canvas: uuid });
      state.scenes = list;
      state.current = list.find((s) => s.current)?.name ?? null;
      state.error = null;
      currentByCanvas[uuid] = state.current;
    } catch (e) {
      state.error = (e as Error).message;
    } finally {
      state.loaded = true;
    }
  }

  // Refresh on scene mutations addressed to this canvas. The Default canvas also
  // reacts to the global channel-0 path, which the backend emits as canvas=null.
  const off = obs.on("scenes.changed", (p) => {
    if (p.canvas === uuid || (isDefault && p.canvas == null)) {
      void refresh();
    }
  });

  void refresh();

  function dispose(): void {
    off();
    delete currentByCanvas[uuid];
  }

  return {
    get state() {
      return state;
    },
    refresh,
    dispose,
  };
}
