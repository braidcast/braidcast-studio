<script lang="ts">
  import ToggleSwitch from "../ToggleSwitch.svelte";

  interface Props {
    checked: boolean;
    label?: string;
    sub?: string;
    disabled?: boolean;
    onchange: (v: boolean) => void;
  }
  let { checked, label = "Use Default", sub, disabled = false, onchange }: Props = $props();
</script>

<div class="strip">
  <ToggleSwitch size="sm" {checked} {disabled} {onchange} />
  <div class="txt">
    <span class="l">{label}</span>
    {#if sub}<span class="s">{sub}</span>{/if}
  </div>
</div>

<style>
  .strip {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 10px 12px;
    margin-bottom: 16px;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .txt {
    display: flex;
    flex-direction: column;
    gap: 2px;
    min-width: 0;
  }
  .l {
    font-size: 12px;
    color: var(--color-text);
  }
  .s {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
  }
</style>
