// Shared reactive diagnostics state: the DEBUG gate, the current session-log path, and
// when this app session started.
// Mirrors outputBindingStore/canvasStore lifecycle (start/whenReady/refresh + a
// #seq guard). Seeded once from diagnostics.get at app boot; the debug.changed event
// keeps `debug` live when the Settings toggle (or any other caller) flips it.
//
// log.ts reads `diagnosticsStore.debug` for its gate. start() seeds asynchronously
// through diagnostics.get, so until that round-trip lands the gate is not known yet:
// `gateKnown` says so, and onGateKnown lets log.ts hold its early log.dbg lines until
// then rather than drop them against the unseeded `false`.

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

  /** Whether `debug` has been read (a seed landed, or debug.changed said it). */
  gateKnown = false;

  #started = false;
  #gateWaiters: (() => void)[] = [];
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
    obs.on(EV.debugChanged, (p) => {
      this.debug = p.debug;
      this.#markGateKnown();
    });
    void this.refresh();
  }

  /** Run `fn` once `debug` has been read: now when it has, else when it first is. */
  onGateKnown(fn: () => void): void {
    if (this.gateKnown) {
      fn();
    } else {
      this.#gateWaiters.push(fn);
    }
  }

  #markGateKnown(): void {
    if (this.gateKnown) {
      return;
    }
    this.gateKnown = true;
    for (const fn of this.#gateWaiters.splice(0)) {
      fn();
    }
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
        // A failed seed settles the gate too: `debug` stays off, which is the default.
        this.#markGateKnown();
      }
      this.#resolveReady();
    }
  }
}

export const diagnosticsStore = new DiagnosticsStore();
