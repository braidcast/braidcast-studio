// The ref-counted feed every push-fed store hangs its bridge subscription on: the
// first subscriber activates, the last release tears down, and a release called twice
// is a no-op. Stores differ only in what activation does (which events, whether it
// seeds a fetch, whether it runs a timer) -- that is the one callback this takes.
//
// Owning the accounting here is what makes the double-release guard unskippable. A
// component teardown legitimately runs an unsubscribe more than once, and an
// unguarded decrement drives the count negative: the next subscribe() then reaches 0
// instead of 1, so the "first subscriber" branch never runs, the events are never
// wired, and the store serves frozen values for the rest of the session with nothing
// thrown and nothing logged.

import type { Unsubscribe } from "$lib/api/bridge";

export class RefCountedSubscription {
  #subs = 0;
  #teardown: Unsubscribe | null = null;
  readonly #activate: () => Unsubscribe;

  /** `activate` runs on the first subscribe and returns the teardown the last
   * release runs. It is never called while a previous activation is still live. */
  constructor(activate: () => Unsubscribe) {
    this.#activate = activate;
  }

  /** Take a reference on the feed; returns a release that is safe to call more than
   * once. Releasing the last reference deactivates, and a later subscribe activates
   * again. */
  subscribe(): Unsubscribe {
    this.#subs++;
    if (this.#subs === 1) {
      this.#teardown = this.#activate();
    }
    let released = false;
    return () => {
      if (released) {
        return;
      }
      released = true;
      this.#subs--;
      if (this.#subs === 0) {
        // Cleared before running so a throwing teardown can't leave a dead one
        // installed for the next activation to inherit.
        const teardown = this.#teardown;
        this.#teardown = null;
        teardown?.();
      }
    };
  }
}
