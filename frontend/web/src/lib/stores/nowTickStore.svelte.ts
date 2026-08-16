// Shared "now" clock for any UI that ages a stored timestamp against the wall clock: chat
// row relative timestamps, and the saved-stream-info picker's created/last-used dates.
// Ref-counted the same way statsStore counts its own 1s timer. The two consumers want it
// for opposite reasons and both are served by one clock: chat needs the count NOT to be
// per-row (a timer per row is one timer per message, and that dock caps its scrollback at
// 500), while the picker is a handful of rows that simply outlive a single clock read,
// since a dialog can sit open far longer than the age it is displaying.
//
// The interval pauses only when the window itself is not visible (minimized/occluded),
// not merely unfocused: chat is routinely watched on a second monitor, where the window
// stays fully visible while another one holds focus. Gating on focus made a message's
// timestamp keep reading "now" long after it was stale -- worse than the CPU the pause
// saved. It resumes with an immediate refresh so a value that went stale while hidden is
// correct the instant the window is looked at again, not up to one tick late.
//
// This store only owns the WINDOW-level half of "is anyone looking at this". A
// consumer inside a hidden dockview tab (the panel's content div goes display:none,
// the component itself stays mounted) is expected to drop its subscription while
// off-screen rather than ask this store to know about DOM layout.

import { RefCountedSubscription } from "$lib/stores/refCountedSubscription";

// Coarser than a 1s clock is fine: every consumer today buckets the age to whole
// minutes, so nothing finer than "the next minute boundary might have passed" needs
// to run, and a resumed-from-hidden refresh is immediate regardless of this period.
const TICK_MS = 15_000;

class NowTickStore {
  nowMs = $state(Date.now());

  #timer: ReturnType<typeof setInterval> | null = null;

  #active(): boolean {
    return document.visibilityState === "visible";
  }

  #refresh = (): void => {
    this.nowMs = Date.now();
  };

  // Re-evaluated on every visibilitychange: starts the interval (with an immediate
  // catch-up tick) when the window becomes visible, stops it outright otherwise.
  #applyActivity = (): void => {
    if (this.#active()) {
      if (this.#timer === null) {
        this.#refresh();
        this.#timer = setInterval(this.#refresh, TICK_MS);
      }
    } else if (this.#timer !== null) {
      clearInterval(this.#timer);
      this.#timer = null;
    }
  };

  #feed = new RefCountedSubscription(() => {
    this.#applyActivity();
    document.addEventListener("visibilitychange", this.#applyActivity);
    return () => {
      document.removeEventListener("visibilitychange", this.#applyActivity);
      if (this.#timer !== null) {
        clearInterval(this.#timer);
        this.#timer = null;
      }
    };
  });

  /** Ref-counted subscription; returns an unsubscribe. The first subscriber wires the
   * visibility listener and starts ticking (if already visible), the last one tears
   * both down. */
  subscribe(): () => void {
    return this.#feed.subscribe();
  }
}

export const nowTickStore = new NowTickStore();
