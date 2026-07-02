<script lang="ts">
  // Hard-edged 0-radius switch (replaces per-dock checkbox/toggle one-offs).
  // A native <button role="switch"> so Space/Enter toggle for free; `checked` is
  // bindable and `onchange` fires with the new value. Without a `label`, pass an
  // accessible name via the surrounding row (or reuse `label` — it doubles as
  // aria-label).
  interface Props {
    checked: boolean;
    onchange?: (checked: boolean) => void;
    disabled?: boolean;
    size?: "sm" | "md";
    label?: string;
  }
  let { checked = $bindable(false), onchange, disabled = false, size = "md", label }: Props = $props();

  function toggle() {
    checked = !checked;
    onchange?.(checked);
  }
</script>

<button
  class="switch {size}"
  class:on={checked}
  role="switch"
  aria-checked={checked}
  aria-label={label}
  {disabled}
  onclick={toggle}
>
  <span class="track"><span class="thumb"></span></span>
  {#if label}<span class="label">{label}</span>{/if}
</button>

<style>
  .switch {
    display: inline-flex;
    align-items: center;
    gap: 7px;
    background: none;
    border: 0;
    padding: 0;
    height: auto;
    cursor: pointer;
  }
  .switch:hover {
    border: 0;
  }
  .switch:disabled {
    opacity: 0.5;
    cursor: default;
  }
  .switch:focus-visible {
    outline: var(--border-weight) solid var(--color-accent);
    outline-offset: 2px;
  }

  .track {
    position: relative;
    flex: 0 0 auto;
    display: inline-block;
    background: var(--color-border);
    transition: background 120ms linear;
  }
  .md .track {
    width: 34px;
    height: 18px;
  }
  .sm .track {
    width: 26px;
    height: 14px;
  }
  .thumb {
    position: absolute;
    top: 3px;
    left: 3px;
    background: var(--color-dim);
    transition: transform 120ms linear;
  }
  .md .thumb {
    width: 12px;
    height: 12px;
  }
  .sm .thumb {
    width: 8px;
    height: 8px;
  }
  .on .track {
    background: var(--color-accent);
  }
  .on .thumb {
    background: var(--color-accent-ink);
    transform: translateX(16px);
  }
  .sm.on .thumb {
    transform: translateX(12px);
  }

  .label {
    font-family: var(--font-ui);
    font-size: 11px;
    color: var(--color-dim);
    white-space: nowrap;
  }
</style>
