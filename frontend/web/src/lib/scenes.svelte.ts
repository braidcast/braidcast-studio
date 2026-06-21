// Shared scenes/current-scene state, driven by the bridge and kept fresh by the
// scenes.changed push event. One subscription for the whole app; panels read the
// reactive `sceneState` and call the action helpers.

import { obs } from "./bridge";

export interface Scene {
  name: string;
  current: boolean;
}

export const sceneState = $state<{
  scenes: Scene[];
  current: string | null;
  loaded: boolean;
  error: string | null;
}>({
  scenes: [],
  current: null,
  loaded: false,
  error: null,
});

export async function refreshScenes(): Promise<void> {
  try {
    const list = await obs.call("scenes.list");
    sceneState.scenes = list;
    sceneState.current = list.find((s) => s.current)?.name ?? null;
    sceneState.error = null;
  } catch (e) {
    sceneState.error = (e as Error).message;
  } finally {
    sceneState.loaded = true;
  }
}

// Install the live subscription + initial load exactly once.
let started = false;
export function startSceneSync(): void {
  if (started) {
    return;
  }
  started = true;
  obs.on("scenes.changed", () => void refreshScenes());
  void refreshScenes();
}
