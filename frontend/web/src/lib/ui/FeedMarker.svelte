<script lang="ts" module>
  import { nowTickStore } from "$lib/stores/nowTickStore.svelte";
  import { sessionsStore } from "$lib/stores/sessionsStore.svelte";
  import { fmtStreamStarted } from "$lib/utils/format";
  import type { Boundary, FeedConfig, MarkerRow } from "$lib/utils/feedVirtualizer.svelte";

  /** The height of a feed's top row and of its dividers. Fixed in CSS, so the feed's
   * estimate for them is exact and they never need measuring. */
  export const FEED_MARKER_HEIGHT = 26;

  /** The feed config for the rows this draws: the top row, and a divider where each
   * broadcast began. The dock starts sessionsStore. */
  export const FEED_MARKERS = {
    topHeight: FEED_MARKER_HEIGHT,
    dividerHeight: FEED_MARKER_HEIGHT,
    boundaries: () => sessionsStore.boundaries,
    dividerLabel: (b: Boundary) => fmtStreamStarted(b.at, nowTickStore.today),
  } satisfies Partial<FeedConfig<unknown>>;
</script>

<script lang="ts">

  // A feed row that is not an item, positioned like the rows around it. The top row shows
  // a spinner while an older page loads, the start of history once the window reaches the
  // oldest row the host holds, a notice while a failed newest page is asked for again, and
  // nothing while older rows wait for the reader to scroll up to them. A divider marks
  // where a broadcast began.
  let { row }: { row: MarkerRow & { top: number } } = $props();
</script>

{#if row.kind === "divider"}
  <div
    class="marker divider"
    role="separator"
    aria-label={row.label}
    style:top={row.top + "px"}
    style:height={FEED_MARKER_HEIGHT + "px"}
  >
    <span class="rule"></span><span class="label">{row.label}</span><span class="rule"></span>
  </div>
{:else}
  <div class="marker" style:top={row.top + "px"} style:height={FEED_MARKER_HEIGHT + "px"}>
    {#if row.state === "loading"}
      <span class="spinner" aria-hidden="true"></span><span>Loading earlier</span>
    {:else if row.state === "start"}
      <span>Start of history</span>
    {:else if row.state === "retrying"}
      <span>Couldn't load, retrying</span>
    {/if}
  </div>
{/if}

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
    white-space: nowrap;
    user-select: none;
  }
  .divider {
    padding: 0 8px;
    color: var(--color-muted);
  }
  .label {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .rule {
    flex: 1 1 0;
    min-width: 12px;
    border-top: var(--border-weight) solid var(--color-border);
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
