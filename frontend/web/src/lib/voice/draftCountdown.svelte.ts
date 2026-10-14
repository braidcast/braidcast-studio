// The seconds a dictated draft's band shows. The host holds the authoritative timer (a
// deadline fixed when the draft was made) and acts on it; this only renders it, from what
// the host said was LEFT at its last emit. Every emit restarts the count from the host's
// newer figure, so the page's clock never drifts far from the host's, and a draft that
// arrived late shows a short window, never a fresh one.
import { draftSecondsLeft, type VoiceDraft } from "$lib/voice/voiceStatus";

/** The page's clock and timer, injectable so a test can drive the count. */
export interface CountdownClock {
  now(): number;
  /** Call `fn` every `ms` until the returned stop is called. */
  every(fn: () => void, ms: number): () => void;
}

const REAL_CLOCK: CountdownClock = {
  now: () => Date.now(),
  every: (fn, ms) => {
    const timer = setInterval(fn, ms);
    return () => clearInterval(timer);
  },
};

/** Ticks a quarter second, so the whole-second figure changes within 250 ms of when it
 * should. */
const TICK_MS = 250;

export class DraftCountdown {
  #seconds = $state(0);

  /** Construct inside a component (or an effect root): `draft` is read in an effect, and
   * each new draft object, which every voice.state emit makes, restarts the count. */
  constructor(draft: () => VoiceDraft | null, clock: CountdownClock = REAL_CLOCK) {
    $effect(() => {
      const d = draft();
      const remainingMs = d && d.phase !== "due" ? d.remainingMs : 0;
      const startedAt = clock.now();
      this.#seconds = draftSecondsLeft(remainingMs, 0);
      if (remainingMs <= 0) {
        return;
      }
      const stop = clock.every(() => {
        this.#seconds = draftSecondsLeft(remainingMs, clock.now() - startedAt);
        if (this.#seconds === 0) {
          stop();
        }
      }, TICK_MS);
      return stop;
    });
  }

  /** Whole seconds left of the draft's window; 0 with no draft, or one that is due. */
  get seconds(): number {
    return this.#seconds;
  }
}
