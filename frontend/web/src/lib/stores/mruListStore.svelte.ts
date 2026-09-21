// The shared shape of a host-side "saved things, most recently used first" list: stream-info
// presets and poll templates. Both are a list method, a changed event that says "re-list",
// and the same touch/remove/rename trio, so the lifecycle lives here once and each store
// adds only what is genuinely its own (the shape `remember` takes).

import { obs } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";

/** The method namespaces that follow the list/touch/remove/rename contract. */
export type MruNamespace = "streamInfoPresets" | "pollTemplates";

/** The event each namespace re-lists on, spelled through EV like every other subscription. */
const CHANGED_EVENT = {
  streamInfoPresets: EV.streamInfoPresetsChanged,
  pollTemplates: EV.pollTemplatesChanged,
} as const satisfies Record<MruNamespace, string>;

export abstract class MruListStore<T> {
  items = $state<T[]>([]);
  loaded = $state(false);
  error = $state<string | null>(null);

  readonly #ns: MruNamespace;
  readonly #list: () => Promise<T[]>;
  #started = false;
  // Per-refresh token: drop a stale resolution so concurrent refreshes can't let a
  // slow earlier call overwrite a newer one (last-issued wins, not last-resolved).
  #seq = 0;

  protected constructor(ns: MruNamespace, list: () => Promise<T[]>) {
    this.#ns = ns;
    this.#list = list;
  }

  start(): void {
    if (this.#started) {
      return;
    }
    this.#started = true;
    obs.on(CHANGED_EVENT[this.#ns], () => void this.refresh());
    void this.refresh();
  }

  async refresh(): Promise<void> {
    const seq = ++this.#seq;
    try {
      const items = await this.#list();
      if (seq !== this.#seq) {
        return;
      }
      this.items = items;
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

  // Mutations do not refresh: the host emits <ns>.changed after each one and the
  // subscription above re-lists off that, so a refresh here would be a second list
  // racing the first.

  /** Restamps lastUsedAtMs, which is what the list is ordered by. */
  async touch(id: string): Promise<void> {
    await obs.call(`${this.#ns}.touch`, { id });
  }

  async remove(id: string): Promise<void> {
    await obs.call(`${this.#ns}.remove`, { id });
  }

  /** An empty name resets the row to reading by its content (title, question). */
  async rename(id: string, name: string): Promise<void> {
    await obs.call(`${this.#ns}.rename`, { id, name });
  }
}
