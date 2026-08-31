<script lang="ts">
  import type { ControlProps } from "$lib/properties/controls";
  import type { ButtonProperty } from "$lib/api/bridge";
  import { openExternalUrl } from "$lib/utils/externalUrl";
  let { prop, onButton }: ControlProps = $props();

  const p = $derived(prop as ButtonProperty);

  function click() {
    // URL buttons open in the system browser; default buttons invoke the source's
    // click callback via properties.button.
    if (p.button_type === "url" && p.url) {
      openExternalUrl(p.url);
      return;
    }
    onButton(prop.name);
  }
</script>

<button
  type="button"
  class="prop-btn"
  disabled={!prop.enabled}
  title={prop.long_description ?? ""}
  onclick={click}
>
  {prop.label ?? prop.name}
</button>

<style>
  .prop-btn {
    grid-column: 1 / -1;
    justify-self: start;
  }
</style>
