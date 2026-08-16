// Shared reactive list of saved stream-info presets (the reusable title/description/
// tags/category sheets, stored host-side). Two surfaces load from the same list -- the
// Go Live modal and the schedule entry editor -- so it is a singleton rather than a
// fetch per dialog; mirrors streamProfileStore's lifecycle.

import { obs } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
import type { StreamInfoPreset } from "$lib/api/bridge";

class StreamInfoPresetStore {
  presets = $state<StreamInfoPreset[]>([]);
  loaded = $state(false);
  error = $state<string | null>(null);

  #started = false;
  // Per-refresh token: drop a stale resolution so concurrent refreshes can't let a
  // slow earlier call overwrite a newer one (last-issued wins, not last-resolved).
  #seq = 0;

  start(): void {
    if (this.#started) {
      return;
    }
    this.#started = true;
    obs.on(EV.streamInfoPresetsChanged, () => void this.refresh());
    void this.refresh();
  }

  async refresh(): Promise<void> {
    const seq = ++this.#seq;
    try {
      const { presets } = await obs.call("streamInfoPresets.list");
      if (seq !== this.#seq) {
        return;
      }
      this.presets = presets;
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

  // Mutations do not refresh: the host emits streamInfoPresets.changed after each one
  // and the subscription above re-lists off that, so a refresh here would be a second
  // list racing the first.

  /** Stores the sheet, reporting whether it landed as a new preset or matched one that
   * already held these values. The id is empty when the store kept no row, so a caller
   * holding on to it must check before using it as one. */
  async remember(sheet: {
    shared: Record<string, unknown>;
    byProvider: Record<string, Record<string, unknown>>;
  }): Promise<{ id: string; created: boolean }> {
    return await obs.call("streamInfoPresets.remember", sheet);
  }

  /** Restamps lastUsedAtMs, which is what the list is ordered by. */
  async touch(id: string): Promise<void> {
    await obs.call("streamInfoPresets.touch", { id });
  }

  async remove(id: string): Promise<void> {
    await obs.call("streamInfoPresets.remove", { id });
  }

  /** An empty name resets the preset to reading by its title. */
  async rename(id: string, name: string): Promise<void> {
    await obs.call("streamInfoPresets.rename", { id, name });
  }
}

export const streamInfoPresetStore = new StreamInfoPresetStore();
