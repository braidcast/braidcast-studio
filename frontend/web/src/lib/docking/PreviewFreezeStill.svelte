<script lang="ts">
  import type { PreviewFreeze } from "$lib/stores/previewFreeze.svelte";
  import { overlayRectOf } from "$lib/utils/overlayRect";

  // Stands in for a hidden native preview surface while an overlay is up: the held
  // frame, drawn where the surface would draw the canvas, over the black the surface
  // clears to. Fills the element the surface was positioned from, which must be its
  // containing block and carry no border, so this box measures as the rect the surface
  // is sized from.
  interface Props {
    freeze: PreviewFreeze;
  }
  let { freeze }: Props = $props();

  let box = $state<HTMLDivElement | undefined>();

  // Nothing reports the surface's rect while it is hidden, so a resize under a held
  // still -- the window, a dock, or the display's scale -- is caught here and the still
  // re-placed for the new size. Device-pixel boxes, so a scale change with no change in
  // CSS size is caught as well.
  $effect(() => {
    const node = box;
    if (!node) {
      return;
    }
    const ro = new ResizeObserver(() => {
      const rect = overlayRectOf(node);
      if (rect) {
        void freeze.relayout(rect);
      }
    });
    ro.observe(node, { box: "device-pixel-content-box" });
    return () => ro.disconnect();
  });
</script>

<!-- aria-hidden: it is the same picture the surface was already showing, so announcing
     it adds nothing. -->
{#if freeze.frame}
  <div class="freeze" aria-hidden="true" bind:this={box}>
    <img
      src={freeze.frame.dataUri}
      alt=""
      elementtiming={freeze.paintId}
      decoding="sync"
      style:left={freeze.frame.placement.left}
      style:top={freeze.frame.placement.top}
      style:width={freeze.frame.placement.width}
      style:height={freeze.frame.placement.height}
      bind:this={freeze.img}
    />
  </div>
{/if}

<style>
  .freeze {
    position: absolute;
    inset: 0;
    overflow: hidden;
    /* The surface's own clear colour (obs_display_create in overlay_surface.cpp), so the
       margin around the canvas reads as it did live. */
    background: #000;
  }
  /* Stretched, not fitted: the surface draws the mix into exactly this rect. */
  img {
    position: absolute;
    display: block;
    max-width: none;
    object-fit: fill;
  }
</style>
