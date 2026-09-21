// Shared reactive list of saved stream-info presets (the reusable title/description/
// tags/category sheets, stored host-side). Two surfaces load from the same list -- the
// Go Live modal and the schedule entry editor -- so it is a singleton rather than a
// fetch per dialog; mirrors streamProfileStore's lifecycle.

import { obs } from "$lib/api/bridge";
import type { StreamInfoPreset } from "$lib/api/bridge";
import { MruListStore } from "$lib/stores/mruListStore.svelte";

class StreamInfoPresetStore extends MruListStore<StreamInfoPreset> {
  constructor() {
    super("streamInfoPresets", async () => (await obs.call("streamInfoPresets.list")).presets);
  }

  /** Stores the sheet, reporting whether it landed as a new preset or matched one that
   * already held these values. The id is empty when the store kept no row, so a caller
   * holding on to it must check before using it as one. */
  async remember(sheet: {
    shared: Record<string, unknown>;
    byProvider: Record<string, Record<string, unknown>>;
  }): Promise<{ id: string; created: boolean }> {
    return await obs.call("streamInfoPresets.remember", sheet);
  }
}

export const streamInfoPresetStore = new StreamInfoPresetStore();
