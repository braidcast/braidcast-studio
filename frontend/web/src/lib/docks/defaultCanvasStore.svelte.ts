// Shared state for the DEFAULT canvas, which hierarchy-model.html decomposes into
// the global Scenes + Sources + Preview docks. Those docks operate on the global
// channel-0 path: every bridge call OMITS the `canvas` param and every event is
// accepted when `canvas == null`. The Scenes dock and the (separate) Sources dock
// instance both read this singleton so they agree on the current scene.

import { obs } from "../bridge";
import { EV } from "../eventNames";
import type { SceneInfo } from "../bridge";

class DefaultCanvasStore {
  scenes = $state<SceneInfo[]>([]);
  current = $state<string | null>(null);
  loaded = $state(false);
  error = $state<string | null>(null);

  #started = false;

  // Idempotent: the first core dock to mount starts it; later mounts are no-ops.
  start(): void {
    if (this.#started) {
      return;
    }
    this.#started = true;
    obs.on(EV.scenesChanged, (p) => {
      // Default canvas == the global channel-0 path, emitted with canvas=null.
      if (p.canvas == null) {
        void this.refresh();
      }
    });
    void this.refresh();
  }

  async refresh(): Promise<void> {
    try {
      const list = await obs.call("scenes.list");
      this.scenes = list;
      this.current = list.find((s) => s.current)?.name ?? null;
      this.error = null;
    } catch (e) {
      this.error = (e as Error).message;
    } finally {
      this.loaded = true;
    }
  }

  async setCurrent(name: string): Promise<void> {
    if (name === this.current) {
      return;
    }
    await obs.call("scenes.setCurrent", { name });
  }

  async create(name: string): Promise<void> {
    await obs.call("scenes.create", { name });
    await obs.call("scenes.setCurrent", { name });
  }

  async rename(from: string, to: string): Promise<void> {
    await obs.call("scenes.rename", { from, to });
  }

  async duplicate(name: string): Promise<void> {
    await obs.call("scenes.duplicate", { name });
  }

  async remove(name: string): Promise<void> {
    await obs.call("scenes.remove", { name });
  }
}

export const defaultCanvas = new DefaultCanvasStore();
