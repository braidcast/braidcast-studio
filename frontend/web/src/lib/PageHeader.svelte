<script lang="ts">
  import type { Snippet } from "svelte";

  // Shared 58px page header (extraction of the Monitor/Overlays `.head`, pixel
  // identical): hairline bottom border on the surface color, 16px/600 title with a
  // baseline-gapped mono sub, optional right-aligned `actions` snippet.
  interface Props {
    title: string;
    sub?: string;
    actions?: Snippet;
  }
  let { title, sub, actions }: Props = $props();
</script>

<header class="head">
  <div class="titles">
    <span class="title">{title}</span>
    {#if sub}<span class="sub">{sub}</span>{/if}
  </div>
  {#if actions}
    <div class="actions">{@render actions()}</div>
  {/if}
</header>

<style>
  .head {
    flex: 0 0 auto;
    height: 58px;
    display: flex;
    align-items: center;
    padding: 0 24px;
    border-bottom: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .titles {
    display: flex;
    align-items: baseline;
    gap: 12px;
    min-width: 0;
  }
  .title {
    font-family: var(--font-ui);
    font-size: 16px;
    font-weight: 600;
    letter-spacing: -0.01em;
    white-space: nowrap;
  }
  .sub {
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-muted);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .actions {
    flex: 0 0 auto;
    margin-left: auto;
    padding-left: 16px;
    display: flex;
    align-items: center;
    gap: 8px;
  }
</style>
