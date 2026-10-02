// The creator goals the destinations' chats are showing (goals.*), kept host-side in the goal
// registry, which merges each goal's partial updates, holds it briefly across a chat restart and
// drops it when the stream stops.
// goals.changed carries the WHOLE list after every change, so an event is a complete
// replacement and nothing here merges.
//
// A popped-out Chat dock runs its own copy of this singleton in another browser, so it must be
// correct from its own initial goals.list plus the events that follow. That is also why a
// finished goal's linger is timed from the host's `endedAtMs`, and a held goal's exit from its
// `heldUntilMs`, rather than from when this copy heard about it: both windows drop it at the
// same moment.

import { obs } from "$lib/api/bridge";
import type { LiveGoal } from "$lib/api/bridge";
import { nextExpiryMs, shownGoals } from "$lib/docks/multichat/goalView";
import { EV } from "$lib/utils/eventNames";

class GoalStore {
  goals = $state<LiveGoal[]>([]);
  /** The clock `shown` is read at; moved on whenever a finished goal's linger or a held goal's
   * hold runs out. */
  now = $state(Date.now());
  /** What the dock draws: running goals, and finished or held ones for a short while. */
  shown = $derived(shownGoals(this.goals, this.now));

  #started = false;
  #timer: ReturnType<typeof setTimeout> | null = null;
  // Bumped by every list issued AND every event applied, as in pollStore: a goals.list that
  // resolves after a goals.changed landed describes an older registry, so it must not win.
  #seq = 0;

  start(): void {
    if (this.#started) {
      return;
    }
    this.#started = true;
    obs.on(EV.goalsChanged, ({ goals }) => {
      this.#seq++;
      this.#set(goals);
    });
    void this.refresh();
  }

  async refresh(): Promise<void> {
    const seq = ++this.#seq;
    try {
      const { goals } = await obs.call("goals.list");
      if (seq === this.#seq) {
        this.#set(goals);
      }
    } catch {
      // Nothing to show is the honest state for a list that could not be read; the next
      // goals.changed replaces it anyway.
    }
  }

  #set(goals: LiveGoal[]): void {
    this.goals = goals;
    this.#tick();
  }

  // Re-read the clock, then wake again when the next finished goal is due to go.
  #tick(): void {
    if (this.#timer !== null) {
      clearTimeout(this.#timer);
      this.#timer = null;
    }
    this.now = Date.now();
    const wait = nextExpiryMs(this.goals, this.now);
    if (wait !== null) {
      this.#timer = setTimeout(() => {
        this.#timer = null;
        this.#tick();
      }, wait);
    }
  }
}

export const goalStore = new GoalStore();
