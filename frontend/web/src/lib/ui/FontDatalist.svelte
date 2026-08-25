<script lang="ts">
  // The installed-family suggestions behind a font input, as a <datalist> the caller
  // points an `<input list=...>` at.
  //
  // A datalist rather than a picker because the field must stay free text: the overlay
  // font field takes a whole CSS stack ("Inter, system-ui, sans-serif"), which no list of
  // installed families can enumerate. It also filters as you type, needs no key handling,
  // and cannot trap focus.
  //
  // The id is the caller's rather than this component's because an input addresses a
  // datalist by id, and the caller is the one that has to name it. Both font surfaces
  // render repeatedly on a page, so callers pass $props.id() -- a constant id would have
  // every mounted copy resolving to whichever list rendered first.
  import { fontStore } from "$lib/stores/fontStore.svelte";

  let { id }: { id: string } = $props();

  // Reading the system font collection is the host's most expensive enumeration, and most
  // sessions never open a font field -- so it is asked for here, on the first font control
  // to mount, rather than at boot.
  $effect(() => {
    void fontStore.load();
  });
</script>

<datalist {id}>
  {#each fontStore.families as family (family)}
    <option value={family}></option>
  {/each}
</datalist>
