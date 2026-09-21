<script lang="ts">
  import Avatar from "$lib/ui/Avatar.svelte";
  import CanvasMark from "$lib/ui/CanvasMark.svelte";
  import PlatformMark from "$lib/ui/PlatformMark.svelte";
  import type { Attribution } from "$lib/ui/destinationSelection";

  // Which destination a chat-side row belongs to, as the Chat dock draws it: platform
  // mark, channel avatar, and the canvas where it disambiguates. One component because a
  // chat line and a poll opened in that chat must name their destination identically.
  interface Props {
    platform: string;
    origin: Attribution;
    title: string;
  }
  let { platform, origin: o, title }: Props = $props();
</script>

<span class="origin" {title}>
  <PlatformMark {platform} size={11} />
  <!-- An unattributable row knows its platform and nothing else. Both the avatar and the
       canvas label would resolve to ABSENT_LABEL, which prints an absence as if it were
       content; the mark above is real. -->
  {#if o.fidelity !== "none"}
    <!-- The avatar is what tells two channels of one platform apart: they share one brand
         mark and one stripe color, so neither can. -->
    <Avatar url={o.avatarUrl} name={o.channel} size={15} />
    {#if !o.named || o.siblings >= 2}
      {#if o.named}
        <CanvasMark
          number={o.canvasNumber}
          name={o.canvasLabel}
          width={o.canvasWidth}
          height={o.canvasHeight}
          size={13}
        />
      {:else}
        <span class="ocanvas">{o.canvasLabel}</span>
      {/if}
    {/if}
  {/if}
</span>

<style>
  .origin {
    align-self: center;
    flex: 0 0 auto;
    display: inline-flex;
    align-items: center;
    gap: 3px;
    min-width: 0;
  }
  /* Only ever a state word -- a named canvas renders as a CanvasMark. Mono and dimmer so
     "channel-wide" never reads as something the user named. */
  .ocanvas {
    min-width: 0;
    max-width: 10ch;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.06em;
    color: var(--color-muted);
  }
</style>
