// Shared host-pushed stats. The host samples once per second and pushes
// `stats.changed` to EVERY browser, so the main window and each detached dock
// window render the same host truth. Each window used to poll `stats.get` on its own
// renderer timer, which meant a detached window's poll could die on its own (a
// throttled or dead renderer) and leave that panel showing a frozen snapshot with no
// cue -- pre-broadcast zeros are indistinguishable from a dead stream. Ref-counted
// the same way multistreamStatusStore is: the first subscriber seeds + wires the
// event, the last unsubscribe tears down and leaves the final snapshot in place.

import { obs, type Stats } from "$lib/api/bridge";
import { RefCountedSubscription } from "$lib/stores/refCountedSubscription";
import { EV } from "$lib/utils/eventNames";

// A sample older than this is no longer a reading. Three host ticks, so an ordinarily
// late tick can't flap the flag.
const STALE_AFTER_MS = 3000;

class StatsStore {
  stats = $state<Stats | null>(null);
  error = $state<string | null>(null);
  // Advanced by the watchdog below, not by the samples: staleness is a function of
  // elapsed time, and the case it detects is precisely "no new sample arrived".
  #nowMs = $state(Date.now());

  /** Age of the newest sample in ms; 0 before the first one arrives. */
  ageMs = $derived(this.stats === null ? 0 : Math.max(0, this.#nowMs - this.stats.sampledAtMs));

  /** True when the newest sample is too old to be a live reading -- the push stream
   * or the host sampler stopped. A consumer MUST surface this: the numbers stay on
   * screen either way, and unlabeled frozen values read as live ones. */
  stale = $derived(this.stats !== null && this.ageMs > STALE_AFTER_MS);

  #load(): void {
    obs
      .call("stats.get")
      .then((s) => {
        this.stats = s;
        this.error = null;
        this.#nowMs = Date.now();
      })
      .catch((e) => (this.error = (e as Error).message));
  }

  #feed = new RefCountedSubscription(() => {
    this.#load();
    // A push is an authoritative read: it supersedes a failed seed, and leaving
    // `error` set would keep the panel showing a failure over live numbers.
    const off = obs.on(EV.statsChanged, (s) => {
      this.stats = s;
      this.error = null;
      this.#nowMs = Date.now();
    });
    const timer = setInterval(() => (this.#nowMs = Date.now()), 1000);
    return () => {
      off();
      clearInterval(timer);
    };
  });

  /** Ref-counted subscription; returns an unsubscribe. The first subscriber seeds the
   * snapshot and wires the push, the last one drops both. */
  subscribe(): () => void {
    return this.#feed.subscribe();
  }
}

export const statsStore = new StatsStore();
