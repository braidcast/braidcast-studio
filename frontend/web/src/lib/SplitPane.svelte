<script lang="ts">
  import type { Snippet } from "svelte";
  import { untrack } from "svelte";

  // Horizontal master-detail split with a draggable vertical divider. The left pane
  // owns a controlled width (persisted per storageKey); the right pane keeps the
  // remaining space and stays dominant — the divider can never grow the list past
  // 34% of the live container width (nor the hard `max`). Divider drag mirrors
  // dock/Splitter's pointer-capture plumbing, one level up (this owns width + clamp
  // + persistence + keyboard + aria).
  interface Props {
    left: Snippet;
    right: Snippet;
    min?: number;
    max?: number;
    default?: number;
    storageKey: string;
  }
  let { left, right, min = 180, max = 420, default: def = 260, storageKey }: Props = $props();

  const STEP = 16;

  function clamp(v: number, lo: number, hi: number): number {
    return Math.min(Math.max(v, lo), Math.max(lo, hi));
  }

  function restore(): number {
    try {
      const raw = localStorage.getItem(storageKey);
      if (raw != null) {
        const n = parseFloat(raw);
        if (Number.isFinite(n)) {
          return n;
        }
      }
    } catch {
      // localStorage unavailable — fall through to the default.
    }
    return def;
  }

  // Pre-measurement initial: restored (or default) width clamped to the hard cap; the
  // container-based cap re-clamps once the ResizeObserver reports a width.
  function initialWidth(): number {
    return clamp(restore(), min, max);
  }

  let containerEl = $state<HTMLElement | undefined>();
  let containerW = $state(0);
  let w = $state(initialWidth());
  let dragging = $state(false);
  let last = 0;

  // Content-dominant cap: min(hard max, 34% of the live container). Falls back to the
  // hard max before the container is measured.
  const effMax = $derived(containerW > 0 ? Math.min(max, containerW * 0.34) : max);

  function persist(): void {
    try {
      localStorage.setItem(storageKey, String(Math.round(w)));
    } catch {
      // Non-fatal: width just won't survive a reload.
    }
  }

  $effect(() => {
    const el = containerEl;
    if (!el) {
      return;
    }
    const ro = new ResizeObserver((entries) => {
      containerW = entries[0]?.contentRect.width ?? el.clientWidth;
    });
    ro.observe(el);
    untrack(() => {
      containerW = el.clientWidth;
      w = clamp(restore(), min, effMax);
    });
    return () => ro.disconnect();
  });

  // Re-clamp only when the cap changes (window resize) so a saved-wide list can't
  // overflow a shrunk window.
  $effect(() => {
    const cap = effMax;
    untrack(() => {
      w = clamp(w, min, cap);
    });
  });

  function down(e: PointerEvent): void {
    e.preventDefault();
    dragging = true;
    last = e.clientX;
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
  }
  function move(e: PointerEvent): void {
    if (!dragging) {
      return;
    }
    const dx = e.clientX - last;
    last = e.clientX;
    w = clamp(w + dx, min, effMax);
  }
  function up(e: PointerEvent): void {
    if (!dragging) {
      return;
    }
    dragging = false;
    (e.currentTarget as HTMLElement).releasePointerCapture(e.pointerId);
    persist();
  }
  function key(e: KeyboardEvent): void {
    let next = w;
    if (e.key === "ArrowLeft") {
      next = w - STEP;
    } else if (e.key === "ArrowRight") {
      next = w + STEP;
    } else if (e.key === "Home") {
      next = min;
    } else if (e.key === "End") {
      next = effMax;
    } else {
      return;
    }
    e.preventDefault();
    w = clamp(next, min, effMax);
    persist();
  }
</script>

<div class="splitpane" bind:this={containerEl}>
  <div class="sp-left" style:flex="0 0 {w}px">
    {@render left()}
  </div>
  <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
  <!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
  <div
    class="sp-divider"
    class:dragging
    role="separator"
    aria-orientation="vertical"
    aria-valuenow={Math.round(w)}
    aria-valuemin={min}
    aria-valuemax={Math.round(effMax)}
    tabindex="0"
    onpointerdown={down}
    onpointermove={move}
    onpointerup={up}
    onpointercancel={up}
    onkeydown={key}
  ></div>
  <div class="sp-right">
    {@render right()}
  </div>
</div>

<style>
  .splitpane {
    flex: 1;
    min-height: 0;
    min-width: 0;
    display: flex;
  }
  .sp-left {
    min-width: 0;
    display: flex;
  }
  .sp-right {
    flex: 1 1 auto;
    min-width: 0;
    display: flex;
  }
  .sp-divider {
    flex: 0 0 6px;
    align-self: stretch;
    cursor: col-resize;
    background: transparent;
    border: 0;
    padding: 0;
    border-radius: 0;
    z-index: 2;
    transition: background 0.1s ease;
    touch-action: none;
  }
  .sp-divider:hover,
  .sp-divider:focus-visible,
  .sp-divider.dragging {
    background: var(--color-accent);
    outline: none;
  }
</style>
