<script lang="ts">
  import { onMount } from "svelte";
  import { obs } from "./bridge";

  // The native obs preview overlay (a sibling HWND above the CEF browser) is
  // positioned to cover exactly this element. We measure our client rect in CSS
  // pixels and report it plus devicePixelRatio; C++ converts to device pixels.
  // The overlay paints the canvas (with letterbox bars) over this region, so the
  // element itself stays empty/transparent -- just a thin frame + label so the
  // area reads as the preview when nothing is rendering yet.

  let el = $state<HTMLElement | undefined>();

  function reportRect() {
    if (!el) return;
    const r = el.getBoundingClientRect();
    obs
      .call("preview.setRect", {
        x: r.left,
        y: r.top,
        w: r.width,
        h: r.height,
        dpr: window.devicePixelRatio || 1,
      })
      .catch((e) => console.log("preview.setRect failed: " + (e as Error).message));
  }

  onMount(() => {
    reportRect();

    const ro = new ResizeObserver(reportRect);
    if (el) ro.observe(el);

    // Window resize and scroll both move our client rect relative to the host
    // client area; re-report on each. Capture phase so nested scroll containers
    // are caught too.
    window.addEventListener("resize", reportRect);
    window.addEventListener("scroll", reportRect, true);

    return () => {
      ro.disconnect();
      window.removeEventListener("resize", reportRect);
      window.removeEventListener("scroll", reportRect, true);
      obs.call("preview.hide").catch(() => {});
    };
  });
</script>

<section class="preview" bind:this={el}>
  <span class="label">Preview</span>
</section>

<style>
  .preview {
    flex: 1;
    min-height: 240px;
    position: relative;
    border: 1px solid var(--border);
    border-radius: 10px;
    /* Transparent: the native overlay HWND paints this region. */
    background: transparent;
    overflow: hidden;
  }
  .label {
    position: absolute;
    top: 8px;
    left: 10px;
    font-size: 11px;
    letter-spacing: 0.04em;
    text-transform: uppercase;
    color: var(--text-dim);
    pointer-events: none;
  }
</style>
