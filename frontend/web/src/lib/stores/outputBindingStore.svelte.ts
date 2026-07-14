// Shared reactive output-binding list (profile x canvas routing edges). Consumers
// (the Canvases page, the Multistream dock, the Go Live modal) used to fetch
// `outputBinding.list` and subscribe `outputBinding.changed` into private state.
// This singleton is the one source of truth; mirrors canvasStore's lifecycle.

import { obs } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
import type { OutputBindingInfo } from "$lib/api/bridge";

// Sentinel profileLabel values the host emits for a binding with no profile, or one
// whose profile was deleted out from under it. Centralized so the row-label logic
// can't drift from the strings the host actually sends.
const LABEL_UNSET = "(unset)";
const LABEL_DELETED = "(deleted)";

export function isBindingUnset(label: string): boolean {
  return label === LABEL_UNSET;
}
export function isBindingDangling(label: string): boolean {
  return label === LABEL_DELETED;
}
// Display name for a destination row: the unset placeholder reads "No destination".
export function bindingDisplayName(b: OutputBindingInfo): string {
  return isBindingUnset(b.profileLabel) ? "No destination" : b.profileLabel;
}

class OutputBindingStore {
  bindings = $state<OutputBindingInfo[]>([]);
  loaded = $state(false);
  error = $state<string | null>(null);

  #started = false;
  #ready: Promise<void>;
  #resolveReady: () => void = () => {};
  // Per-refresh token: drop a stale resolution so concurrent refreshes can't let a
  // slow earlier call overwrite a newer one (last-issued wins, not last-resolved).
  #seq = 0;

  constructor() {
    this.#ready = new Promise((r) => (this.#resolveReady = r));
  }

  start(): void {
    if (this.#started) {
      return;
    }
    this.#started = true;
    obs.on(EV.outputBindingChanged, () => void this.refresh());
    void this.refresh();
  }

  whenReady(): Promise<void> {
    this.start();
    return this.#ready;
  }

  async refresh(): Promise<void> {
    const seq = ++this.#seq;
    try {
      const bindings = await obs.call("outputBinding.list");
      if (seq !== this.#seq) {
        return;
      }
      this.bindings = bindings;
      this.error = null;
    } catch (e) {
      if (seq !== this.#seq) {
        return;
      }
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
