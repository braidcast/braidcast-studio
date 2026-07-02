<script lang="ts">
  import type { Snippet } from "svelte";

  // Shared empty-state card (promoted from the Streams-page "no profile selected"
  // pattern): hairline-boxed, centered in the host, SVG icon on top of a title +
  // mono muted sub. `compact` drops the box for dock-sized contexts — just the
  // centered stack.
  interface Props {
    title: string;
    sub?: string;
    compact?: boolean;
    icon?: Snippet;
  }
  let { title, sub, compact = false, icon }: Props = $props();
</script>

<div class="empty" class:compact>
  <div class="card">
    {#if icon}
      <div class="icon">{@render icon()}</div>
    {/if}
    <p class="title">{title}</p>
    {#if sub}<p class="sub">{sub}</p>{/if}
  </div>
</div>

<style>
  .empty {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 100%;
    height: 100%;
    min-height: 0;
  }
  .card {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 4px;
    text-align: center;
    color: var(--color-muted);
    border: var(--border-weight) solid var(--color-border);
    padding: 26px 34px;
  }
  .compact .card {
    border: 0;
    padding: 0;
  }
  .icon {
    display: flex;
    color: var(--color-border);
    margin-bottom: 8px;
  }
  .title {
    margin: 0;
    font-family: var(--font-ui);
    font-size: 12px;
    font-weight: 600;
    color: var(--color-dim);
  }
  .sub {
    margin: 0;
    font-family: var(--font-mono);
    font-size: 10.5px;
    color: var(--color-muted);
  }
</style>
