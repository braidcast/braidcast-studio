<script lang="ts" module>
  export interface SegmentedOption {
    label: string;
    value: string;
  }
</script>

<script lang="ts">
  // The mock's segmented toggle (.seg / .seg span.on): a horizontal row of cells,
  // the one matching `value` highlighted with the accent. One reusable control for
  // every binary/short-enum axis (label case, density, meter/selection style,
  // border weight, letter spacing). Token-styled, 0 radius.
  let {
    options,
    value,
    onChange,
  }: {
    options: SegmentedOption[];
    value: string;
    onChange: (v: string) => void;
  } = $props();
</script>

<div class="seg" role="radiogroup">
  {#each options as opt (opt.value)}
    <button
      type="button"
      class="cell"
      class:on={value === opt.value}
      role="radio"
      aria-checked={value === opt.value}
      onclick={() => onChange(opt.value)}>{opt.label}</button
    >
  {/each}
</div>

<style>
  .seg {
    display: flex;
    border: var(--border-weight) solid var(--color-border);
  }
  .cell {
    flex: 1;
    height: auto;
    padding: 4px 10px;
    font-family: var(--font-ui);
    font-size: 10px;
    letter-spacing: var(--letter-spacing);
    background: transparent;
    border: none;
    border-right: var(--border-weight) solid var(--color-border);
    color: var(--color-muted);
    white-space: nowrap;
  }
  .cell:last-child {
    border-right: none;
  }
  .cell:hover {
    color: var(--color-text);
  }
  .cell.on {
    background: color-mix(in srgb, var(--color-accent) 18%, transparent);
    color: var(--color-accent);
  }
</style>
