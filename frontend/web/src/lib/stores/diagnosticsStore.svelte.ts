// Shared reactive diagnostics state: the DEBUG gate + the current session-log path.
// Mirrors outputBindingStore/canvasStore lifecycle (start/whenReady/refresh + a
// #seq guard). Seeded once from diagnostics.get at app boot; the debug.changed event
// keeps `debug` live when the Settings toggle (or any other caller) flips it.
//
// log.ts reads `diagnosticsStore.debug` for its gate, so this must be started early
// (App.svelte onMount) for early debug lines to be gated correctly.

import { obs } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";

class DiagnosticsStore {
  debug = $state(false);
  logPath = $state("");
  loaded = $state(false);
  error = $state<string | null>(null);

  #started = false;
  #ready: Promise<void>;
  #resolveReady: () => void = () => {};
  // Per-refresh token: a slow earlier seed can't overwrite a newer one.
  #seq = 0;

  constructor() {
    this.#ready = new Promise((r) => (this.#resolveReady = r));
  }

  start(): void {
    if (this.#started) {
      return;
    }
    this.#started = true;
    obs.on(EV.debugChanged, (p) => (this.debug = p.debug));
    void this.refresh();
  }

  whenReady(): Promise<void> {
    this.start();
    return this.#ready;
  }

  async refresh(): Promise<void> {
    const seq = ++this.#seq;
    try {
      const d = await obs.call("diagnostics.get");
      if (seq !== this.#seq) {
        return;
      }
      this.debug = d.debug;
      this.logPath = d.logPath;
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
}

export const diagnosticsStore = new DiagnosticsStore();
