<script lang="ts" module>
  /** The top row's height. Fixed in CSS, so the feed's estimate for it is exact and it
   * never needs measuring. */
  export const FEED_TOP_HEIGHT = 26;
</script>

<script lang="ts">
  import type { TopState } from "$lib/utils/feedVirtualizer.svelte";

  // A feed's top row, positioned like the rows under it: a spinner while an older page
  // loads, the start of history once the window reaches the oldest row the host holds,
  // and nothing while older rows wait for the reader to scroll up to them.
  let { state, top }: { state: TopState; top: number } = $props();
</script>

<div class="marker" style:top={top + "px"} style:height={FEED_TOP_HEIGHT + "px"}>
  {#if state === "loading"}
    <span class="spinner" aria-hidden="true"></span><span>Loading earlier</span>
  {:else if state === "start"}
    <span>Start of history</span>
  {/if}
</div>

<style>
  .marker {
    position: absolute;
    left: 0;
    right: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
    font-family: var(--font-mono);
    font-size: 9.5px;
    letter-spacing: 0.09em;
    text-transform: var(--label-case);
    color: var(--color-dim);
    user-select: none;
  }
  .spinner {
    flex: none;
    width: 9px;
    height: 9px;
    border: calc(var(--border-weight) * 1.5) solid var(--color-border);
    border-top-color: var(--color-accent);
    animation: feed-marker-spin 0.8s linear infinite;
  }
  @keyframes feed-marker-spin {
    to {
      transform: rotate(360deg);
    }
  }
  @media (prefers-reduced-motion: reduce) {
    .spinner {
      animation-duration: 2.4s;
    }
  }
</style>
