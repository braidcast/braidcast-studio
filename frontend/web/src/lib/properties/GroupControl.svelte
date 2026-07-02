<script lang="ts">
  import type { ControlProps } from "./controls";
  import type { GroupProperty } from "../bridge";
  import PropertyRow from "./PropertyRow.svelte";

  let { prop, value, onChange, onButton, lookup }: ControlProps = $props();

  const p = $derived(prop as GroupProperty);
  const checkable = $derived(p.group_type === "checkable");
  const checked = $derived(Boolean(value));
  // Children are disabled when a checkable group is unchecked.
  const childrenEnabled = $derived(!checkable || checked);
</script>

<fieldset class="group" disabled={!childrenEnabled}>
  <legend>
    {#if checkable}
      <label class="legend-toggle">
        <input
          type="checkbox"
          {checked}
          disabled={!prop.enabled}
          onchange={(e) => onChange(prop.name, (e.currentTarget as HTMLInputElement).checked)}
        />
        <span>{prop.label ?? prop.name}</span>
      </label>
    {:else}
      <span>{prop.label ?? prop.name}</span>
    {/if}
  </legend>

  <div class="group-body">
    {#each p.props as child (child.name)}
      <PropertyRow prop={child} value={lookup(child.name)} {onChange} {onButton} {lookup} />
    {/each}
  </div>
</fieldset>

<style>
  .group {
    grid-column: 1 / -1;
    border: 1px solid var(--color-border);
    padding: 12px 14px 14px;
    margin: 0;
    min-width: 0;
  }
  .group[disabled] .group-body {
    opacity: 0.5;
  }
  legend {
    padding: 0 6px;
    font-size: 13px;
    font-weight: 600;
    color: var(--color-text);
  }
  .legend-toggle {
    display: flex;
    align-items: center;
    gap: 8px;
    cursor: pointer;
  }
  .group-body {
    display: flex;
    flex-direction: column;
    gap: 12px;
  }
</style>
