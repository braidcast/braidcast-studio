// Shared reactive output-binding list (profile x canvas routing edges). Consumers
// (the Canvases page, the Multistream dock, the Go Live modal) used to fetch
// `outputBinding.list` and subscribe `outputBinding.changed` into private state.
// This singleton is the one source of truth; mirrors canvasStore's lifecycle.

import { obs } from "./bridge";
import type { OutputBindingInfo } from "./bridge";

class OutputBindingStore {
  bindings = $state<OutputBindingInfo[]>([]);
  loaded = $state(false);
  error = $state<string | null>(null);

  #started = false;
  #ready: Promise<void>;
  #resolveReady: () => void = () => {};

  constructor() {
    this.#ready = new Promise((r) => (this.#resolveReady = r));
  }

  start(): void {
    if (this.#started) {
      return;
    }
    this.#started = true;
    obs.on("outputBinding.changed", () => void this.refresh());
    void this.refresh();
  }

  whenReady(): Promise<void> {
    this.start();
    return this.#ready;
  }

  async refresh(): Promise<void> {
    try {
      this.bindings = await obs.call("outputBinding.list");
      this.error = null;
    } catch (e) {
      this.error = (e as Error).message;
    } finally {
      this.loaded = true;
      this.#resolveReady();
    }
  }

  forCanvas(canvasUuid: string): OutputBindingInfo[] {
    return this.bindings.filter((b) => b.canvasUuid === canvasUuid);
  }
}

export const outputBindingStore = new OutputBindingStore();
