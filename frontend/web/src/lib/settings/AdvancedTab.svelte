<script lang="ts">
  import { EV } from "$lib/utils/eventNames";
  import { obs } from "$lib/api/bridge";
  import PropertyForm from "$lib/properties/PropertyForm.svelte";

  // Every control here is declared once, in AdvancedSettings.hpp's field tables:
  // wire key, file key, struct member, label, hint, group, range and enable-condition
  // all travel together, and the form is generated from them. This tab used to restate
  // each of those by hand and had already drifted -- it seeded streamDelaySec 0 and
  // streamDelayPreserve false while the backend defaults were 20 and true.
  //
  // Re-key the form on settings.advancedChanged so an edit made elsewhere (another
  // window, a restore) is picked up; PropertyForm re-fetches on a target change and
  // otherwise owns its own reload after each push.
  let epoch = $state(0);
  $effect(() => obs.on(EV.settingsAdvancedChanged, () => (epoch += 1)));
</script>

<p class="dim note">Advanced output settings apply to streams started after the change.</p>

{#key epoch}
  <PropertyForm kind="settings" ref="advanced" />
{/key}

<style>
  .dim {
    color: var(--color-muted);
    margin: 0;
  }
  .note {
    font-size: 12px;
    margin-bottom: 10px;
  }
</style>
