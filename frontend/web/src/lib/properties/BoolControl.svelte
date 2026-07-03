<script lang="ts">
  import type { ControlProps } from "./controls";
  import ToggleSwitch from "../ToggleSwitch.svelte";
  let { prop, value, onChange }: ControlProps = $props();
  const checked = $derived(Boolean(value));
</script>

<!-- The field label + hint are rendered by PropertyRow's row layout, so the
     switch itself stays bare (mirrors the mock's tglField). -->
<ToggleSwitch {checked} disabled={!prop.enabled} onchange={(c) => onChange(prop.name, c)} />
