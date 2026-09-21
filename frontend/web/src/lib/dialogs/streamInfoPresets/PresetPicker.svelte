<script lang="ts">
  import type { StreamInfoPreset } from "$lib/api/bridge";
  import { presetLabel } from "$lib/dialogs/streamInfoPresets/applyPreset";
  import SavedItemPicker, { type SavedItemCopy } from "$lib/dialogs/SavedItemPicker.svelte";
  import { streamInfoPresetStore } from "$lib/stores/streamInfoPresetStore.svelte";

  // Picks one saved stream-info sheet and hands it back. What APPLYING one means belongs
  // to the surface that opened it, since the Go Live modal writes inherit layers and the
  // schedule editor writes flat rows.
  interface Props {
    /** Worded by the caller: the same list is "load into this go-live" in one place and
     * "load into this entry" in the other. */
    title?: string;
    onPick: (preset: StreamInfoPreset) => void;
    onClose: () => void;
  }
  let { title = "Saved Stream Info", onPick, onClose }: Props = $props();

  streamInfoPresetStore.start();

  const COPY: SavedItemCopy = {
    noun: "preset",
    heading: "Presets",
    listLabel: "Saved stream info presets",
    renamePlaceholder: "Empty name reads by title",
    emptyTitle: "No saved stream info yet",
    emptySub:
      "Turn on Remember when you go live and this go-live's title, description and tags are saved here for next time.",
    note:
      "Loading one fills the fields in as a starting point — nothing is sent until you go live, " +
      "and everything stays editable.",
    deleteMessage: (label) =>
      `Delete "${label}"? This removes the saved sheet only — nothing already applied to a channel changes.`,
  };
</script>

<SavedItemPicker
  {title}
  items={streamInfoPresetStore.items}
  loaded={streamInfoPresetStore.loaded}
  error={streamInfoPresetStore.error}
  copy={COPY}
  label={presetLabel}
  touch={(id) => streamInfoPresetStore.touch(id)}
  remove={(id) => streamInfoPresetStore.remove(id)}
  rename={(id, name) => streamInfoPresetStore.rename(id, name)}
  {onPick}
  {onClose}
/>
