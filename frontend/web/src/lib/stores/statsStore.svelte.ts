import { obs, type Stats } from "$lib/api/bridge";

// Shared 1 Hz stats poller. `stats.get` has no push, so each consumer used to own
// its own interval — two ran concurrently on the Studio page (bottom bar + Stats
// dock). This ref-counts subscribers and runs a SINGLE interval while >=1 is
// active; the last unsubscribe stops it and leaves the final snapshot in place.
// Per-consumer page gating is preserved by where each subscribes (e.g. StudioPage
// only subscribes while on the studio page).
class StatsStore {
  stats = $state<Stats | null>(null);
  error = $state<string | null>(null);
  #subs = 0;
  #timer: ReturnType<typeof setInterval> | undefined;

  #load(): void {
    obs
      .call("stats.get")
      .then((s) => {
        this.stats = s;
        this.error = null;
      })
      .catch((e) => (this.error = (e as Error).message));
  }

  /** Ref-counted subscription; returns an unsubscribe. Start the poll on the first
   * subscriber, stop it on the last. */
  subscribe(): () => void {
    this.#subs++;
    if (this.#subs === 1) {
      this.#load();
      this.#timer = setInterval(() => this.#load(), 1000);
    }
    return () => {
      this.#subs--;
      if (this.#subs === 0 && this.#timer) {
        clearInterval(this.#timer);
        this.#timer = undefined;
      }
    };
  }
}

export const statsStore = new StatsStore();
