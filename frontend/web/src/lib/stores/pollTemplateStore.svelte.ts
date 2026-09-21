// Saved poll templates (the polls a streamer has run, kept to run again), stored host-side
// and ordered most recently used first. polls.create remembers a template itself, so this
// store only lists, restamps, renames and deletes.
//
// A popped-out Chat dock runs its own copy of this singleton in another browser; it needs
// nothing but its own initial list and the changed event to be correct.

import { obs } from "$lib/api/bridge";
import type { PollTemplate } from "$lib/api/bridge";
import { MruListStore } from "$lib/stores/mruListStore.svelte";

class PollTemplateStore extends MruListStore<PollTemplate> {
  constructor() {
    super("pollTemplates", async () => (await obs.call("pollTemplates.list")).templates);
  }
}

export const pollTemplateStore = new PollTemplateStore();
