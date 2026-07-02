<script lang="ts">
  import type { PropertyDescriptor } from "../bridge";
  import { controlFor } from "./controls";

  interface Props {
    prop: PropertyDescriptor;
    value: unknown;
    onChange: (name: string, value: unknown) => void;
    onButton: (name: string) => void;
    lookup: (name: string) => unknown;
  }
  let { prop, value, onChange, onButton, lookup }: Props = $props();

  const Control = $derived(controlFor(prop.type));
  // bool/button/info/group own their full-width row and render their own label,
  // so the form grid's label cell is suppressed for them.
  const ownsLabel = $derived(
    prop.type === "bool" ||
      prop.type === "button" ||
      prop.type === "group" ||
      (prop.type === "text" && (prop as { text_type?: string }).text_type === "info"),
  );
</script>

{#if prop.visible}
  <div class="row" class:full={ownsLabel}>
    {#if !ownsLabel}
      <label class="label" for={`p-${prop.name}`} title={prop.long_description ?? ""}>
        {prop.label ?? prop.name}
      </label>
    {/if}
    <div class="control">
      <Control {prop} {value} {onChange} {onButton} {lookup} />
    </div>
  </div>
{/if}

<style>
  .row {
    display: grid;
    grid-template-columns: minmax(120px, 0.4fr) 1fr;
    gap: 10px 14px;
    align-items: center;
  }
  .row.full {
    grid-template-columns: 1fr;
  }
  .label {
    text-align: right;
    font-size: 13px;
    color: var(--color-text);
    overflow-wrap: anywhere;
  }
  .control {
    min-width: 0;
  }
</style>
