import { obs, type ScheduleEntryInfo, type ScheduleEntryInput } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";

// Planned entries are queried state, not a live stream: the set changes when the
// user edits one or the runner settles one, which is what schedule.changed
// reports. Same start-and-refresh shape as sessionsStore rather than
// statsStore's ref-counted push model.
//
// The whole set is held rather than a per-view range. A month of entries is
// small, and range-scoped caching would mean every calendar navigation is a
// round trip plus a cache-coherence question at exactly the moment the user is
// dragging something.
class ScheduleStore {
  entries = $state<ScheduleEntryInfo[]>([]);
  loaded = $state(false);
  error = $state<string | null>(null);
  #started = false;
  #seq = 0;

  start(): void {
    if (this.#started) {
      return;
    }
    this.#started = true;
    obs.on(EV.scheduleChanged, () => void this.refresh());
    void this.refresh();
  }

  async refresh(): Promise<void> {
    // Drops a stale response that lands after a newer one: schedule.changed can
    // fire twice in quick succession (a drag commits, then the runner arms), and
    // the first reply must not overwrite the second.
    const seq = ++this.#seq;
    try {
      const entries = await obs.call("schedule.list", { from: 0 });
      if (seq !== this.#seq) {
        return;
      }
      this.entries = entries;
      this.error = null;
    } catch (e) {
      if (seq !== this.#seq) {
        return;
      }
      this.error = (e as Error).message;
    } finally {
      this.loaded = true;
    }
  }

  async create(input: ScheduleEntryInput): Promise<ScheduleEntryInfo> {
    return obs.call("schedule.create", input);
  }

  async update(id: string, input: ScheduleEntryInput): Promise<ScheduleEntryInfo> {
    return obs.call("schedule.update", { id, ...input });
  }

  async remove(id: string): Promise<void> {
    await obs.call("schedule.delete", { id });
  }

  /** Disarm this occurrence. The runner pushes schedule.changed itself, so there
   * is nothing to refresh here -- the subscription above does it. */
  async cancelCountdown(id: string): Promise<void> {
    await obs.call("schedule.cancelCountdown", { id });
  }

  /** Start this occurrence immediately. Same no-refresh reasoning as
   * cancelCountdown: the runner's own schedule.changed carries the new state. */
  async startNow(id: string): Promise<void> {
    await obs.call("schedule.startNow", { id });
  }
}

export const scheduleStore = new ScheduleStore();
