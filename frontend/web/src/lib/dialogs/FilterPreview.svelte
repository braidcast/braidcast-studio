<script lang="ts">
  import { onMount } from "svelte";
  import { obs } from "$lib/api/bridge";
  import { EV } from "$lib/utils/eventNames";
  import { overlayRectOf } from "$lib/utils/overlayRect";

  // The Filters dialog's live preview: a placeholder the host paints a native
  // overlay over, showing the filtered source with its whole chain applied.
  //
  // It is a SECOND native overlay, not the main preview. The main one is suspended
  // for every modal (previewGate.svelte.ts) because a child HWND composited above
  // CEF would swallow the dialog; this one is positioned inside the dialog's own
  // bounds, which is the arrangement upstream's OBSBasicFilters uses.
  //
  // Consequences of the surface being native rather than DOM, and how they are
  // handled here: it does not clip to the dialog, so the IntersectionObserver hides
  // it the moment the placeholder is anything less than fully visible; and it does
  // not follow a CSS transform, so the dialog calls resync() while it is dragged.
  //
  // Known limit: that observer reports CLIPPING, not OVERLAP. A DOM surface painted
  // on top of the placeholder still measures fully visible, so the overlay would
  // swallow it. Nothing the dialog itself opens can do that (its only popups are
  // native <select>/color pickers, which are OS windows above the overlay anyway);
  // an app-level fixed surface such as a toast could, and is accepted.
  interface Props {
    /** Source name whose filter chain is being edited. */
    source: string;
  }
  let { source }: Props = $props();

  let el = $state<HTMLElement | undefined>();

  // The host's verdict: true = previewable, false = nothing to preview (audio-only, or
  // no standalone composite) and the pane renders nothing rather than a black box,
  // null = no verdict -- either not answered yet or the call failed.
  //
  // A failure stays null rather than collapsing to false: "the call did not land" is a
  // different fact from "this source has no video", and recording it as the latter
  // would be a verdict nothing checked. The reachable failure is a source renamed or
  // removed between the open request and this effect running -- the dialog addresses
  // its source by name, so the host resolves it by name too and misses. No retry: on
  // that path filters.list has failed for the same reason and the dialog is already
  // showing its own error, so the preview has nothing to come back to.
  let hasVideo = $state<boolean | null>(null);

  // Not $state: read only inside reportRect, never rendered. Starts false so nothing
  // is positioned before the IntersectionObserver delivers its first record -- a
  // placeholder that mounts already clipped would otherwise flash one frame.
  let fullyVisible = false;

  let rectRaf = 0;
  function scheduleRect(): void {
    if (rectRaf) {
      return;
    }
    rectRaf = requestAnimationFrame(() => {
      rectRaf = 0;
      reportRect();
    });
  }

  function reportRect(): void {
    if (!el || hasVideo !== true) {
      return;
    }
    const rect = fullyVisible ? overlayRectOf(el) : null;
    if (!rect) {
      obs.call("filterPreview.hide").catch(() => {});
      return;
    }
    obs
      .call("filterPreview.setRect", rect)
      .catch((e) => console.log("filterPreview.setRect failed: " + (e as Error).message));
  }

  /** Re-measure now. Called while the dialog is dragged: a native child window does
   *  not follow the panel's CSS transform, so nothing else would move it. */
  export function resync(): void {
    scheduleRect();
  }

  // Bind the host preview to the current source, and rebind when the dialog is
  // pointed at a different one. Open replaces any previous binding host-side.
  $effect(() => {
    const target = source;
    let stale = false;
    hasVideo = null;
    obs
      .call("filterPreview.open", { source: target })
      .then((r) => {
        if (!stale) {
          hasVideo = r.video;
        }
      })
      .catch((e) => {
        if (!stale) {
          hasVideo = null;
          // The placeholder unmounts on a null verdict, so nothing is left to hide the
          // overlay. A call that failed in transport left the host's previous binding
          // standing; drop it here or it paints over the dialog until unmount. Guarded
          // by `stale` so a superseded open cannot close its successor's binding.
          obs.call("filterPreview.close").catch(() => {});
          console.log("filterPreview.open failed: " + (e as Error).message);
        }
      });
    return () => {
      stale = true;
    };
  });

  // Track the placeholder for as long as it exists. ResizeObserver covers layout
  // changes; IntersectionObserver covers everything that would clip a DOM element
  // but not a native child -- scrolled out of an ancestor, or off the viewport.
  $effect(() => {
    const node = el;
    if (!node) {
      return;
    }
    const ro = new ResizeObserver(scheduleRect);
    const io = new IntersectionObserver(
      (entries) => {
        fullyVisible = entries[entries.length - 1].intersectionRatio >= 0.999;
        reportRect();
      },
      { threshold: [0, 0.999, 1] },
    );
    ro.observe(node);
    io.observe(node);
    scheduleRect();
    return () => {
      ro.disconnect();
      io.disconnect();
    };
  });

  onMount(() => {
    window.addEventListener("resize", scheduleRect);
    window.addEventListener("scroll", scheduleRect, true);

    // The bound source was deleted while the dialog is open: the host has already
    // torn the overlay down, so collapse the pane instead of leaving a hole.
    const offClosed = obs.on(EV.filterPreviewClosed, () => {
      hasVideo = false;
    });

    return () => {
      window.removeEventListener("resize", scheduleRect);
      window.removeEventListener("scroll", scheduleRect, true);
      if (rectRaf) {
        cancelAnimationFrame(rectRaf);
      }
      offClosed();
      obs.call("filterPreview.close").catch(() => {});
    };
  });
</script>

{#if hasVideo}
  <!-- Fixed aspect box so the pane reserves its space before the native surface
       attaches and never reflows under it. The ground is black rather than a surface
       token because it has to match the obs_display clear color (0x000000, set in
       overlay_surface.cpp) -- the bars the surface paints and the DOM behind it must
       be indistinguishable on the first frame and while the overlay is hidden. -->
  <div
    class="filter-preview"
    bind:this={el}
    role="img"
    aria-label="Live preview of {source} with its filters applied"
  ></div>
{/if}

<style>
  .filter-preview {
    flex: 0 0 auto;
    width: 100%;
    aspect-ratio: 16 / 9;
    margin-bottom: 8px;
    background: #000;
    border: var(--border-weight) solid var(--color-border);
  }
</style>
