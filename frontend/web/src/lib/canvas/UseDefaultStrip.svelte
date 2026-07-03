<script lang="ts">
  import ToggleSwitch from "../ToggleSwitch.svelte";

  interface Props {
    checked: boolean;
    label?: string;
    sub?: string;
    onchange: (v: boolean) => void;
  }
  let { checked, label = "Use Default", sub, onchange }: Props = $props();
</script>

<div class="strip">
  <ToggleSwitch size="sm" {checked} {onchange} />
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
