<script lang="ts">
  import ToggleSwitch from "$lib/ui/ToggleSwitch.svelte";

  interface Props {
    checked: boolean;
    label?: string;
    description?: string;
    inheritNote?: string;
    disabled?: boolean;
    onchange: (v: boolean) => void;
  }
  let {
    checked,
    label = "Use Default canvas settings",
    // The grayed controls keep showing this canvas's own stored values, not the Default's,
    // so the copy says they are retained rather than implying they display what is inherited.
    description = "Follow the Default canvas for this group; the controls below gray out and keep this canvas's own values.",
    inheritNote,
    disabled = false,
    onchange,
  }: Props = $props();
</script>

<div class="cv-inheritstrip">
  <ToggleSwitch size="sm" {checked} {disabled} {onchange} />
  <div class="cv-inheritstrip__l">
    <span class="cv-inheritstrip__t">{label}</span>
    {#if description}<span class="cv-inheritstrip__d">{description}</span>{/if}
    {#if checked && inheritNote}<span class="cv-inheritstrip__n">{inheritNote}</span>{/if}
  </div>
</div>
