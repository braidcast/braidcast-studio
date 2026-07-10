// Shared reactive canvas list. Every canvas-aware surface (the Canvases page, the
// Studio bar, the Multistream/Preview/Canvas docks, the reconciler, the Schedule
// modal) used to fetch `canvas.list` and subscribe `canvas.changed` into its own
// private state. This singleton is the one source of truth: the first consumer to
// call start() does the initial fetch + opens the subscription; every consumer
// reads the reactive `canvases` array. Mirrors defaultCanvasStore's lifecycle.

import { obs } from "./bridge";
import { EV } from "./eventNames";
import type { CanvasInfo } from "./bridge";

class CanvasStore {
  canvases = $state<CanvasInfo[]>([]);
  loaded = $state(false);
  error = $state<string | null>(null);

  #started = false;
  #ready: Promise<void>;
  #resolveReady: () => void = () => {};

  constructor() {
    this.#ready = new Promise((r) => (this.#resolveReady = r));
  }

  // Idempotent: the first consumer to mount starts it; later starts are no-ops.
  start(): void {
    if (this.#started) {
      return;
    }
    this.#started = true;
    obs.on(EV.canvasChanged, () => void this.refresh());
    // Each CanvasInfo.enabled is AnyEnabledForCanvas(canvas) server-side, so a pure
    // binding toggle (outputBinding.changed, no canvas.changed) still flips a canvas's
    // enabled flag -- refresh on it too so `enabled` stays authoritative.
    obs.on(EV.outputBindingChanged, () => void this.refresh());
    void this.refresh();
  }

  // Resolves after the first refresh settles, for one-shot consumers that need a
  // populated list at a specific moment (a detached window's name lookup, a modal's
  // prefill). Starts the store if it hasn't been.
  whenReady(): Promise<void> {
    this.start();
    return this.#ready;
  }

  async refresh(): Promise<void> {
    try {
      this.canvases = await obs.call("canvas.list");
      this.error = null;
    } catch (e) {
      this.error = (e as Error).message;
    } finally {
      this.loaded = true;
      this.#resolveReady();
    }
  }

  byUuid(uuid: string): CanvasInfo | undefined {
    return this.canvases.find((c) => c.uuid === uuid);
  }
}

export const canvasStore = new CanvasStore();
