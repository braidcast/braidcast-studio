<script lang="ts">
  import { fmtChatTime, isRealTimestamp } from "$lib/utils/format";
  import { nowTickStore } from "$lib/stores/nowTickStore.svelte";

  let { ts }: { ts: number } = $props();
</script>

<!-- Fixed-width leading gutter: the value's own width changes ("now" -> "45m" ->
     "14:32") as nowTickStore ticks, but the box does not, so later rows never reflow
     out from under it. Must sit in a flex container with `align-items: baseline` --
     `align-self: baseline` below has no baseline to join otherwise, and a lone
     baseline participant reduces to flex-start (Flexbox §8.3), which rides the
     timestamp visibly above the row's text. -->
<span class="time" title={isRealTimestamp(ts) ? new Date(ts).toLocaleString() : ""}
  >{fmtChatTime(ts, nowTickStore.nowMs)}</span
>

<style>
  /* min-width in ch (the font's own digit width) rather than a px guess -- the three
     shapes fmtChatTime returns ("now", "45m", "14:32") top out at 5 characters, so this
     is exactly wide enough for the longest and never resizes as a row's value grows. */
  .time {
    flex: 0 0 auto;
    align-self: baseline;
    min-width: 5ch;
    font-family: var(--font-mono);
    font-variant-numeric: tabular-nums;
    font-size: 10px;
    color: var(--color-muted);
  }
</style>
