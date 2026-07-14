<script lang="ts">
  import ToggleSwitch from "$lib/ui/ToggleSwitch.svelte";

  interface Props {
    checked: boolean;
    label?: string;
    description?: string;
    disabled?: boolean;
    onchange: (v: boolean) => void;
  }
  let {
    checked,
    label = "Use Default canvas settings",
    description = "Follow the Default canvas for this group; controls below gray out.",
    disabled = false,
    onchange,
  }: Props = $props();
</script>

<div class="cv-inheritstrip">
  <ToggleSwitch size="sm" {checked} {disabled} {onchange} />
  <div class="cv-inheritstrip__l">
    <span class="cv-inheritstrip__t">{label}</span>
    {#if description}<span class="cv-inheritstrip__d">{description}</span>{/if}
  </div>
</div>
