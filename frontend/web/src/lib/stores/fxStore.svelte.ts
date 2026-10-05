// The exchange rates and the streamer's currency the app draws Super Chat amounts with
// (roadmap 9.6), from the host's rate store (fx.get). fx.changed carries the whole snapshot
// after a new day's rates or a currency setting change, so an event is a complete replacement.
// A channel connected or removed can change the automatic currency without either, so an
// oauth.status re-reads it.
//
// A popped-out dock runs its own copy of this singleton, correct from its own fx.get plus the
// events that follow.

import { obs } from "$lib/api/bridge";
import type { FxSnapshot } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";

class FxStore {
  /** Null until the first read lands: amounts then read as the payer's alone. */
  snapshot = $state<FxSnapshot | null>(null);

  #started = false;
  // A read that resolves after a newer event or read landed describes an older store.
  #seq = 0;

  start(): void {
    if (this.#started) {
      return;
    }
    this.#started = true;
    obs.on(EV.fxChanged, (s) => {
      this.#seq++;
      this.snapshot = s;
    });
    obs.on(EV.oauthStatus, () => void this.refresh());
    void this.refresh();
  }

  async refresh(): Promise<void> {
    const seq = ++this.#seq;
    try {
      const s = await obs.call("fx.get");
      if (seq === this.#seq) {
        this.snapshot = s;
      }
    } catch {
      // Payer-only amounts are the honest fallback; the next fx.changed replaces this.
    }
  }
}

export const fxStore = new FxStore();
