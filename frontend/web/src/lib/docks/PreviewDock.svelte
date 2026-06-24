<script lang="ts">
  import { onMount } from "svelte";
  import { obs } from "../bridge";
  import { previewSuspended, suspendPreview } from "../previewGate.svelte";
  import { WINDOW_ID } from "../windowContext";
  import ContextMenu, { type ContextMenuItem } from "../ContextMenu.svelte";
  import PropertyForm from "../properties/PropertyForm.svelte";
  import { openFilters } from "../filterDialogOpener.svelte";
  import { openTransform } from "../transformOpener.svelte";
  import { prefetchMonitors, projectorItems } from "../projectorMenu";

  let {}: Record<string, unknown> = $props();

  let menu = $state<{ x: number; y: number; items: (ContextMenuItem | null)[] } | null>(null);
  let propsForSource = $state<string | null>(null);

  // Source context menu for the right-clicked scene item. Default surface, so every
  // call OMITS the canvas param (global channel-0 path).
  function buildItems(p: {
    scene: string | null;
    id: number | null;
    source: string | null;
    visible: boolean;
    locked: boolean;
  }): (ContextMenuItem | null)[] {
    const call = (method: string, params: Record<string, unknown>) =>
      obs.call(method, params).catch((e) => console.log(method + " failed: " + (e as Error).message));
    return [
      {
        label: "Properties",
        action: () => {
          if (p.source) {
            propsForSource = p.source;
          }
        },
      },
      { label: "Filters", disabled: !p.source, action: () => p.source && openFilters(p.source) },
      {
        label: "Edit Transform",
        action: () =>
          p.id != null && openTransform({ scene: p.scene ?? undefined, id: p.id }, p.source ?? "(unnamed)"),
      },
      null,
      { label: p.visible ? "Hide" : "Show", action: () => void call("sceneItems.setVisible", { scene: p.scene, id: p.id, visible: !p.visible }) },
      { label: p.locked ? "Unlock" : "Lock", action: () => void call("sceneItems.setLocked", { scene: p.scene, id: p.id, locked: !p.locked }) },
      null,
      { label: "Move Up", action: () => void call("sceneItems.reorder", { scene: p.scene, id: p.id, direction: "up" }) },
      { label: "Move Down", action: () => void call("sceneItems.reorder", { scene: p.scene, id: p.id, direction: "down" }) },
      { label: "Move to Top", action: () => void call("sceneItems.reorder", { scene: p.scene, id: p.id, direction: "top" }) },
      { label: "Move to Bottom", action: () => void call("sceneItems.reorder", { scene: p.scene, id: p.id, direction: "bottom" }) },
      null,
      // Project the program / default mix (the Default surface == channel 0).
      ...projectorItems({ kind: "program" }),
      null,
      { label: "Remove", danger: true, action: () => void call("sceneItems.remove", { scene: p.scene, id: p.id }) },
    ];
  }

  // The native overlay (a sibling HWND above CEF) covers this exact element; it
  // stays transparent so the overlay paints through. Global channel-0 path: the
  // Default surface is addressed by OMITTING the canvas param (P3 side docks pass
  // their own uuid, keeping this surface distinct).
  let previewEl = $state<HTMLElement | undefined>();

  function reportRect() {
    if (!previewEl) {
      return;
    }
    const r = previewEl.getBoundingClientRect();
    obs
      .call("preview.setRect", {
        window: WINDOW_ID,
        x: r.left,
        y: r.top,
        w: r.width,
        h: r.height,
        dpr: window.devicePixelRatio || 1,
      })
      .catch((e) => console.log("preview.setRect failed: " + (e as Error).message));
  }

  function hidePreview() {
    obs.call("preview.hide", { window: WINDOW_ID }).catch(() => {});
  }

  function destroyPreview() {
    obs.call("preview.destroy", { window: WINDOW_ID }).catch(() => {});
  }

  onMount(() => {
    prefetchMonitors();
    reportRect();
    const ro = new ResizeObserver(reportRect);
    if (previewEl) {
      ro.observe(previewEl);
    }
    window.addEventListener("resize", reportRect);
    window.addEventListener("scroll", reportRect, true);

    // Right-click in the overlay: filter to the Default surface in this window with
    // a real hit, then map the device-px cursor to viewport coords via the rect.
    const offMenu = obs.on("preview.contextMenu", (p) => {
      if (p.canvas != null || p.window !== WINDOW_ID || p.id == null || !previewEl) {
        return;
      }
      const r = previewEl.getBoundingClientRect();
      const dpr = window.devicePixelRatio || 1;
      menu = { x: r.left + p.x / dpr, y: r.top + p.y / dpr, items: buildItems(p) };
    });

    return () => {
      ro.disconnect();
      window.removeEventListener("resize", reportRect);
      window.removeEventListener("scroll", reportRect, true);
      offMenu();
      destroyPreview();
    };
  });

  // The global previewGate suspends every native overlay while a modal is open;
  // hide our surface and re-assert its rect when cleared.
  $effect(() => {
    if (previewSuspended()) {
      hidePreview();
    } else {
      reportRect();
    }
  });

  // The context menu opens at the cursor inside the preview; the native overlay
  // sits above CEF and would occlude it, so suspend the overlay while it's open.
  $effect(() => {
    if (menu) {
      return suspendPreview();
    }
  });

  // The properties modal overlaps the preview; suspend the native overlay while open.
  $effect(() => {
    if (propsForSource) {
      return suspendPreview();
    }
  });
</script>

<section class="preview" bind:this={previewEl}>
  <span class="label">Default</span>
</section>

{#if menu}
  <ContextMenu x={menu.x} y={menu.y} items={menu.items} onClose={() => (menu = null)} />
{/if}

{#if propsForSource}
  <div
    class="modal-backdrop"
    role="presentation"
    onclick={(e) => {
      if (e.target === e.currentTarget) propsForSource = null;
    }}
  >
    <div class="modal" role="dialog" aria-modal="true" aria-label="Source properties">
      <header class="modal-head">
        <h3>Properties — {propsForSource}</h3>
        <button class="dock-icon close" title="Close" onclick={() => (propsForSource = null)}>✕</button>
      </header>
      <div class="modal-body">
        <PropertyForm kind="source" ref={propsForSource} />
      </div>
    </div>
  </div>
{/if}

<style>
  .preview {
    height: 100%;
    width: 100%;
    position: relative;
    /* Transparent: the native overlay HWND paints this region. */
    background: transparent;
    overflow: hidden;
  }
  .label {
    position: absolute;
    top: 8px;
    left: 10px;
    font-family: var(--font-ui);
    font-size: 9px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    color: var(--color-muted);
    pointer-events: none;
  }
  .modal-backdrop {
    position: fixed;
    inset: 0;
    background: rgba(0, 0, 0, 0.55);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 100;
    padding: 24px;
  }
  .modal {
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    width: min(560px, 100%);
    max-height: 80vh;
    display: flex;
    flex-direction: column;
  }
  .modal-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 12px 16px;
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .modal-head h3 {
    margin: 0;
    font-size: 12px;
    font-family: var(--font-ui);
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .modal-body {
    padding: 16px;
    overflow: auto;
  }
  .close {
    font-size: 13px;
  }
</style>
