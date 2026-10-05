<script lang="ts" module>
  export interface SegmentedOption {
    label: string;
    value: string;
    /** This one choice is unavailable; the rest stay usable. */
    disabled?: boolean;
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
    size = "sm",
    ariaLabel,
    disabled = false,
  }: {
    options: SegmentedOption[];
    value: string;
    onChange: (v: string) => void;
    size?: "sm" | "md";
    /** Names the group for assistive tech when no visible heading is tied to it. */
    ariaLabel?: string;
    /** The whole control is unavailable: shown dimmed, current choice still marked. */
    disabled?: boolean;
  } = $props();
</script>

<div
  class="seg"
  class:dis={disabled}
  data-size={size}
  role="radiogroup"
  aria-label={ariaLabel}
  aria-disabled={disabled || undefined}
>
  {#each options as opt (opt.value)}
    <button
      type="button"
      class="cell"
      class:on={value === opt.value}
      role="radio"
      aria-checked={value === opt.value}
      disabled={disabled || opt.disabled}
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
  .cell:hover:not(:disabled) {
    color: var(--color-text);
  }
  .cell:disabled {
    cursor: default;
  }
  .cell:disabled:not(.on) {
    opacity: 0.5;
  }
  .seg.dis {
    opacity: 0.4;
  }
  /* The control's own dimming already says it; a cell must not dim twice. */
  .seg.dis .cell:disabled {
    opacity: 1;
  }
  .cell.on {
    background: color-mix(in srgb, var(--color-accent) 18%, transparent);
    color: var(--color-accent);
  }
  /* md cells carry longer labels (e.g. "Custom Streaming Server", "WHIP Service").
     Let them shrink and wrap centered instead of overrunning the cell border. */
  .seg[data-size="md"] .cell {
    padding: 8px 10px;
    font-size: 11px;
    min-width: 0;
    white-space: normal;
    text-align: center;
    line-height: 1.25;
    overflow-wrap: anywhere;
  }
</style>
