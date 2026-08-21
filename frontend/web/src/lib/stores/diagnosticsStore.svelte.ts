// Shared reactive diagnostics state: the DEBUG gate + the current session-log path.
// Mirrors outputBindingStore/canvasStore lifecycle (start/whenReady/refresh + a
// #seq guard). Seeded once from diagnostics.get at app boot; the debug.changed event
// keeps `debug` live when the Settings toggle (or any other caller) flips it.
//
// log.ts reads `diagnosticsStore.debug` for its gate. start() seeds asynchronously
// through diagnostics.get, so any reader running before that round-trip resolves sees
// the unseeded `false` -- early web log.dbg lines are therefore NOT reliably gated.
// Starting early (App.svelte onMount) narrows that window; it does not close it.

import { obs } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";

class DiagnosticsStore {
  debug = $state(false);
  logPath = $state("");
  // Listening CEF remote-debugging port, 0 when closed. Fixed for the session
  // (CefSettings is read once at CefInitialize), so the boot seed is the only read
  // and no event updates it -- debug.changed below carries the gate alone.
  devToolsPort = $state(0);
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
      this.devToolsPort = d.devToolsPort;
      this.error = null;
    } catch (e) {
      if (seq !== this.#seq) {
        return;
      }
      this.error = (e as Error).message;
    } finally {
      // Guarded: a stale response returning after a newer refresh took over must not
      // mark the store loaded, because `loaded && !error` is what licenses the
      // Diagnostics tab to say "No debugging port is open" -- and on the stale path
      // devToolsPort is still 0 and error still null, so an unsettled read would render
      // as an affirmative all-clear. #resolveReady stays unguarded: it is one-shot, and
      // whenReady() callers must not hang because a superseded call lost the race.
      if (seq === this.#seq) {
        this.loaded = true;
      }
      this.#resolveReady();
    }
  }
}

export const diagnosticsStore = new DiagnosticsStore();
