<script lang="ts">
  import { onMount } from "svelte";
  import { obs } from "../bridge";
import { EV } from "../eventNames";
  import { canvasStore } from "../canvasStore.svelte";
  import { previewSuspended, suspendPreview } from "../previewGate.svelte";
import { dockLayout } from "../dockLayoutSignal.svelte";
  import { WINDOW_ID } from "../windowContext";
  import { syncPreviewRect, hidePreview, destroyPreview, mapOverlayCursor } from "../dock/previewSurface";
  import ContextMenu, { type ContextMenuItem } from "../ContextMenu.svelte";
  import PropertyForm from "../properties/PropertyForm.svelte";
  import Modal from "../Modal.svelte";
  import { openFilters } from "../filterDialogOpener.svelte";
  import { transformMenu } from "../transformMenu";

  let {}: Record<string, unknown> = $props();

  let menu = $state<{ x: number; y: number; items: (ContextMenuItem | null)[] } | null>(null);
  let propsForSource = $state<string | null>(null);

  // Output-gate the Default canvas preview, mirroring the additional canvases (which
  // the reconciler adds/removes on their own `enabled` flag). CanvasInfo.enabled for
  // the Default canvas is `AnyEnabledForCanvas(default)` server-side, so the native
  // preview paints only while >=1 destination is enabled; otherwise the overlay is
  // hidden and a muted placeholder takes its place. This panel is the QMainWindow-
  // central anchor for the docks row, so it is never removed — only its content flips.
  // `enabled` recomputes on canvas.changed AND outputBinding.changed (a binding toggle
  // signals via the latter); the shared canvasStore refreshes on both, so derive off
  // it. Before the first load (empty list) default to painting (matches the old init).
  let defaultEnabled = $derived(
    canvasStore.canvases.find((c) => c.isDefault)?.enabled ?? !canvasStore.loaded,
  );

  // The Properties modal overlaps the native overlay. Acquire/release the preview
  // suspension together with `propsForSource` (not in a reactive $effect) so the
  // gate ref-count never transiently hits zero during the context-menu -> modal
  // handoff -- which would let the overlay re-raise above the modal.
  let propsRelease: (() => void) | null = null;
  function openProps(source: string) {
    propsForSource = source;
    propsRelease ??= suspendPreview();
  }
  function closeProps() {
    propsForSource = null;
    propsRelease?.();
    propsRelease = null;
  }

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
            openProps(p.source);
          }
        },
      },
      { label: "Filters", disabled: !p.source, action: () => p.source && openFilters(p.source) },
      ...(p.id != null ? [transformMenu({ scene: p.scene ?? undefined, id: p.id }, p.source ?? "(unnamed)")] : []),
      null,
      { label: p.visible ? "Hide" : "Show", action: () => void call("sceneItems.setVisible", { scene: p.scene, id: p.id, visible: !p.visible }) },
      { label: p.locked ? "Unlock" : "Lock", action: () => void call("sceneItems.setLocked", { scene: p.scene, id: p.id, locked: !p.locked }) },
      null,
      { label: "Move Up", action: () => void call("sceneItems.reorder", { scene: p.scene, id: p.id, direction: "up" }) },
      { label: "Move Down", action: () => void call("sceneItems.reorder", { scene: p.scene, id: p.id, direction: "down" }) },
      { label: "Move to Top", action: () => void call("sceneItems.reorder", { scene: p.scene, id: p.id, direction: "top" }) },
      { label: "Move to Bottom", action: () => void call("sceneItems.reorder", { scene: p.scene, id: p.id, direction: "bottom" }) },
      // Projector entries hidden pending the projector redesign (projectorMenu + its
      // bridge path are kept, just not surfaced here).
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
    // Output-gated off (no enabled destination on the Default canvas), or a modal
    // holds the preview gate: keep the native overlay hidden. Re-asserting the rect
    // during either would raise the native child window back above CEF (over the
    // modal, or over the muted placeholder).
    if (!defaultEnabled || previewSuspended()) {
      hidePreview();
      return;
    }
    syncPreviewRect(previewEl);
  }

  onMount(() => {
    canvasStore.start();
    reportRect();
    const ro = new ResizeObserver(reportRect);
    if (previewEl) {
      ro.observe(previewEl);
    }
    window.addEventListener("resize", reportRect);
    window.addEventListener("scroll", reportRect, true);

    // Right-click in the overlay: filter to the Default surface in this window with
    // a real hit, then map the device-px cursor to viewport coords via the rect.
    const offMenu = obs.on(EV.previewContextMenu, (p) => {
      if (p.canvas != null || p.window !== WINDOW_ID || p.id == null || !previewEl) {
        return;
      }
      const { x, y } = mapOverlayCursor(previewEl, p);
      menu = { x, y, items: buildItems(p) };
    });

    return () => {
      ro.disconnect();
      window.removeEventListener("resize", reportRect);
      window.removeEventListener("scroll", reportRect, true);
      offMenu();
      destroyPreview();
    };
  });

  // React to the output-gate flipping: paint the native overlay when re-enabled,
  // hide it when the last destination is disabled (the placeholder shows through).
  $effect(() => {
    if (defaultEnabled) {
      reportRect();
    } else {
      hidePreview();
    }
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

  // Re-measure on any dock layout change. A reorder/move swaps panel positions
  // without resizing them, so ResizeObserver never fires — measure next frame,
  // after Dockview has settled the new positions.
  $effect(() => {
    dockLayout.v;
    requestAnimationFrame(reportRect);
  });

  // The context menu opens at the cursor inside the preview; the native overlay
  // sits above CEF and would occlude it, so suspend the overlay while it's open.
  $effect(() => {
    if (menu) {
      return suspendPreview();
    }
  });
</script>

<section class="preview" class:gated={!defaultEnabled} bind:this={previewEl}>
  <span class="label">Default</span>
  {#if !defaultEnabled}
    <div class="placeholder">
      <p class="ph-title">Preview off</p>
      <p class="ph-sub">No enabled destination — turn one on in Canvases.</p>
    </div>
  {/if}
</section>

{#if menu}
  <ContextMenu x={menu.x} y={menu.y} items={menu.items} onClose={() => (menu = null)} />
{/if}

{#if propsForSource}
  <Modal title={"Properties — " + propsForSource} onClose={closeProps} width={560} maxHeight="80vh">
    <PropertyForm kind="source" ref={propsForSource} />
  </Modal>
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
  /* Output-gated off: no native overlay paints here, so give the region an opaque
     surface + a muted empty-state message instead of a see-through hole. */
  .preview.gated {
    background: var(--color-base);
  }
  .placeholder {
    position: absolute;
    inset: 0;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 4px;
    text-align: center;
    padding: 20px;
    pointer-events: none;
  }
  .ph-title {
    margin: 0;
    font-family: var(--font-ui);
    font-size: 12px;
    font-weight: 600;
    color: var(--color-dim);
  }
  .ph-sub {
    margin: 0;
    font-family: var(--font-mono);
    font-size: 10.5px;
    color: var(--color-muted);
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
</style>
