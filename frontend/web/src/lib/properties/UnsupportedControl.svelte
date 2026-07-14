<script lang="ts">
  import type { ControlProps } from "$lib/properties/controls";
  let { prop }: ControlProps = $props();
  // font / editable_list / frame_rate: serialized best-effort by the backend but
  // not yet editable here. Show a clear placeholder rather than a broken control.
</script>

<div class="unsupported" title={prop.long_description ?? ""}>
  <span class="badge">{prop.type}</span>
  <span class="msg">Editing not yet supported (TODO)</span>
</div>

<style>
  .unsupported {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 12px;
    color: var(--color-muted);
  }
  .badge {
    font-family: var(--mono, ui-monospace, monospace);
    background: var(--color-base);
    border: 1px solid var(--color-border);
    padding: 1px 6px;
  }
</style>
