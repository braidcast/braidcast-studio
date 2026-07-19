<script lang="ts">
  import { onMount } from "svelte";
  import { obs, type SceneItem, type DeinterlaceMode, type DeinterlaceFieldOrder, type TransitionType } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
  import { canvasStore } from "$lib/stores/canvasStore.svelte";
  import { previewSuspended, suspendPreview } from "$lib/stores/previewGate.svelte";
import { dockLayout } from "$lib/docking/dockLayoutSignal.svelte";
  import { WINDOW_ID } from "$lib/utils/windowContext";
  import { syncPreviewRect, hidePreview, destroyPreview, mapOverlayCursor } from "$lib/docking/previewSurface";
  import { isPreviewDisabled, setPreviewDisabled, DEFAULT_PREVIEW_KEY } from "$lib/docking/previewDisabledStore.svelte";
  import ContextMenu, { type ContextMenuItem } from "$lib/menus/ContextMenu.svelte";
  import PropertiesModal from "$lib/properties/PropertiesModal.svelte";
  import AddSourceModal from "$lib/dialogs/add-source/AddSourceModal.svelte";
  import { clipboard } from "$lib/stores/clipboardStore.svelte";
  import { copyItem, pasteReference, pasteDuplicate } from "$lib/stores/clipboardItemState";
  import { openFilters } from "$lib/dialogs/filterDialogOpener.svelte";
  import { transformMenu } from "$lib/menus/transformMenu";
  import { scaleFilterMenu } from "$lib/menus/scaleFilterMenu";
  import { blendModeMenu, blendMethodMenu } from "$lib/menus/blendMenu";
  import { showTransitionMenu, hideTransitionMenu, transitionTypes } from "$lib/menus/transitionMenu";
  import { deinterlaceMenu } from "$lib/menus/deinterlaceMenu";
  import { colorMenu } from "$lib/menus/colorMenu";

  let {}: Record<string, unknown> = $props();

  let menu = $state<{ x: number; y: number; items: (ContextMenuItem | null)[] } | null>(null);
  let propsForSource = $state<string | null>(null);

  // Add-source (opened from the empty-area menu). The Default surface is the global
  // channel-0 path, so the modal always passes canvas={null}; `addScene` is the
  // scene captured from the right-click that opened the menu.
  let adding = $state(false);
  let addScene = $state<string | null>(null);
  function beginAddSource(scene: string | null) {
    addScene = scene;
    adding = true;
  }
  function onSourceCreated(created: { id: number; source: string }) {
    adding = false;
    openProps(created.source);
  }

  const warn = (method: string) => (e: unknown) => console.log(method + " failed: " + (e as Error).message);

  // Deinterlacing is a source-level property (not in the event payload), fetched
  // just-in-time when the menu opens. Falls back to disabled state on error.
  async function fetchDeint(source: string): Promise<{ mode: DeinterlaceMode; fieldOrder: DeinterlaceFieldOrder }> {
    try {
      return await obs.call("sources.getDeinterlace", { source });
    } catch {
      return { mode: "disable", fieldOrder: "top" };
    }
  }

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

  // OBS parity: "Disable Preview" stops rendering the native surface to save GPU.
  // Destroying (not hiding) is what actually matters -- the native preview surface
  // holds a main-render ref that keeps the Main canvas compositing every frame;
  // only releasing the surface via destroy() drops that ref and lets it go idle.
  function disablePreview() {
    setPreviewDisabled(DEFAULT_PREVIEW_KEY, true);
    destroyPreview();
  }

  // Re-enable: flip the flag, then measure immediately so the surface repaints on
  // this frame instead of waiting for the next resize/layout event to trigger it.
  function enablePreview() {
    setPreviewDisabled(DEFAULT_PREVIEW_KEY, false);
    requestAnimationFrame(reportRect);
  }

  // Source context menu for the right-clicked scene item. Default surface, so every
  // call OMITS the canvas param (global channel-0 path). `it` is the scene item as
  // fetched just-in-time (the event payload lacks scale/blend/color/transitions);
  // `deint` + `transitionTypeList` are likewise fetched before the menu is built.
  function buildItems(
    p: { scene: string | null; id: number | null; source: string | null; visible: boolean; locked: boolean },
    it: SceneItem | null,
    deint: { mode: DeinterlaceMode; fieldOrder: DeinterlaceFieldOrder },
    transitionTypeList: TransitionType[],
  ): (ContextMenuItem | null)[] {
    // Inject the global-path target (scene + id) into every scene-item call.
    const call = (method: string, params: Record<string, unknown>) =>
      obs.call(method, { scene: p.scene, id: p.id, ...params }).catch(warn(method));
    const currentFilter = it?.scaleFilter ?? "disable";
    const currentBlendMode = it?.blendMode ?? "normal";
    const currentBlendMethod = it?.blendMethod ?? "default";
    const currentColor = it?.color ?? "";
    const currentShowTransition = it?.showTransition ?? null;
    const currentHideTransition = it?.hideTransition ?? null;
    return [
      { label: "Properties", action: () => p.source && openProps(p.source) },
      { label: "Filters", disabled: !p.source, action: () => p.source && openFilters(p.source) },
      ...(it?.interactive && p.source
        ? [{ label: "Interact", action: () => void obs.call("sources.interact", { source: p.source }).catch(warn("sources.interact")) }]
        : []),
      ...(p.id != null ? [transformMenu({ scene: p.scene ?? undefined, id: p.id }, p.source ?? "(unnamed)")] : []),
      scaleFilterMenu(currentFilter, (filter) => void call("sceneItems.setScaleFilter", { filter })),
      blendModeMenu(currentBlendMode, (mode) => void call("sceneItems.setBlendingMode", { mode })),
      blendMethodMenu(currentBlendMethod, (method) => void call("sceneItems.setBlendingMethod", { method })),
      showTransitionMenu(
        currentShowTransition,
        transitionTypeList,
        (type) =>
          void call("sceneItems.setShowTransition", { transition: type, duration: currentShowTransition?.duration ?? 300 }),
        (duration) => void call("sceneItems.setShowTransition", { transition: currentShowTransition?.type ?? null, duration }),
      ),
      hideTransitionMenu(
        currentHideTransition,
        transitionTypeList,
        (type) =>
          void call("sceneItems.setHideTransition", { transition: type, duration: currentHideTransition?.duration ?? 300 }),
        (duration) => void call("sceneItems.setHideTransition", { transition: currentHideTransition?.type ?? null, duration }),
      ),
      ...(p.source
        ? [
            deinterlaceMenu(
              deint.mode,
              deint.fieldOrder,
              (mode) => void obs.call("sources.setDeinterlace", { source: p.source, mode }).catch(warn("sources.setDeinterlace")),
              (fieldOrder) =>
                void obs.call("sources.setDeinterlace", { source: p.source, fieldOrder }).catch(warn("sources.setDeinterlace")),
            ),
          ]
        : []),
      colorMenu(currentColor, (color) => void call("sceneItems.setColor", { color })),
      { label: "Screenshot", disabled: !p.source, action: () => void call("screenshot.takeSource", {}) },
      null,
      {
        label: "Copy",
        disabled: !p.source,
        action: () => {
          if (it && p.id != null) {
            void copyItem({ scene: p.scene, id: p.id }, it);
          }
        },
      },
      {
        label: "Paste",
        disabled: !clipboard.source,
        action: () => void pasteReference({ scene: p.scene }).catch(warn("paste")),
      },
      {
        label: "Paste (Duplicate)",
        disabled: !clipboard.source,
        action: () => void pasteDuplicate({ scene: p.scene }).catch(warn("paste")),
      },
      {
        label: "Copy Transform",
        action: () =>
          void obs
            .call("sceneItems.getTransform", { scene: p.scene, id: p.id })
            .then((t) => (clipboard.transform = t))
            .catch(warn("sceneItems.getTransform")),
      },
      {
        label: "Paste Transform",
        disabled: !clipboard.transform,
        action: () => void call("sceneItems.setTransform", { transform: clipboard.transform }),
      },
      null,
      { label: "Group", action: () => void call("sceneItems.group", { ids: [p.id] }) },
      { label: "Ungroup", action: () => void call("sceneItems.ungroup", {}) },
      null,
      { label: p.visible ? "Hide" : "Show", action: () => void call("sceneItems.setVisible", { visible: !p.visible }) },
      { label: p.locked ? "Unlock" : "Lock", action: () => void call("sceneItems.setLocked", { locked: !p.locked }) },
      null,
      { label: "Move Up", action: () => void call("sceneItems.reorder", { direction: "up" }) },
      { label: "Move Down", action: () => void call("sceneItems.reorder", { direction: "down" }) },
      { label: "Move to Top", action: () => void call("sceneItems.reorder", { direction: "top" }) },
      { label: "Move to Bottom", action: () => void call("sceneItems.reorder", { direction: "bottom" }) },
      // Projector entries hidden pending the projector redesign (projectorMenu + its
      // bridge path are kept, just not surfaced here).
      null,
      { label: "Remove", danger: true, action: () => void call("sceneItems.remove", {}) },
      null,
      { label: "Disable Preview", action: disablePreview },
    ];
  }

  // Empty-area menu (right-click with no source under the cursor). Add Source + Paste
  // reuse the global add-source modal + clipboard paste. "New Group" is intentionally
  // absent: there is no empty-group-create bridge method (sceneItems.group requires
  // existing ids), so it can't be wired without inventing one.
  function buildEmptyItems(scene: string | null): (ContextMenuItem | null)[] {
    return [
      { label: "Add Source", disabled: !scene, action: () => beginAddSource(scene) },
      {
        label: "Paste",
        disabled: !clipboard.source,
        action: () => void pasteReference({ scene }).catch(warn("paste")),
      },
      {
        label: "Paste (Duplicate)",
        disabled: !clipboard.source,
        action: () => void pasteDuplicate({ scene }).catch(warn("paste")),
      },
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
    // Output-gated off (no enabled destination on the Default canvas), a modal
    // holds the preview gate, or the user disabled the preview: keep the native
    // overlay hidden. Re-asserting the rect during any of these would raise the
    // native child window back above CEF (over the modal, or over the placeholder).
    // While disabled the surface is already destroyed (see disablePreview above),
    // so hidePreview here is a harmless idempotent no-op.
    if (!defaultEnabled || previewSuspended() || isPreviewDisabled(DEFAULT_PREVIEW_KEY)) {
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

    // Right-click in the overlay: filter to the Default surface in this window, then
    // map the device-px cursor to viewport coords via the rect. A hit (id != null)
    // builds the source menu (item scale/blend/color/deint fetched just-in-time,
    // since the payload omits them); empty area builds the add/paste menu.
    const offMenu = obs.on(EV.previewContextMenu, (p) => {
      if (p.canvas != null || p.window !== WINDOW_ID || !previewEl) {
        return;
      }
      const { x, y } = mapOverlayCursor(previewEl, p);
      if (p.id == null) {
        menu = { x, y, items: buildEmptyItems(p.scene) };
        return;
      }
      void (async () => {
        const list = await obs.call("sceneItems.list", { scene: p.scene }).catch(() => [] as SceneItem[]);
        const it = list.find((i) => i.id === p.id) ?? null;
        const deint = p.source ? await fetchDeint(p.source) : { mode: "disable" as const, fieldOrder: "top" as const };
        const transitionTypeList = await transitionTypes().catch(() => []);
        menu = { x, y, items: buildItems(p, it, deint, transitionTypeList) };
      })();
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

  // The add-source modal overlaps the native overlay too; suspend while it's open.
  $effect(() => {
    if (adding) {
      return suspendPreview();
    }
  });
</script>

<section class="preview" class:gated={!defaultEnabled || isPreviewDisabled(DEFAULT_PREVIEW_KEY)} bind:this={previewEl}>
  <span class="label">Default</span>
  {#if isPreviewDisabled(DEFAULT_PREVIEW_KEY)}
    <div class="placeholder">
      <p class="ph-title">Preview disabled</p>
      <p class="ph-sub">Rendering is stopped to save GPU.</p>
      <button class="accent" onclick={enablePreview}>Re-enable Preview</button>
    </div>
  {:else if !defaultEnabled}
    <div class="placeholder">
      <p class="ph-title">Preview off</p>
      <p class="ph-sub">No enabled destination — turn one on in Canvases.</p>
    </div>
  {/if}
</section>

{#if menu}
  <ContextMenu x={menu.x} y={menu.y} items={menu.items} onClose={() => (menu = null)} />
{/if}

{#if adding}
  <AddSourceModal canvas={null} scene={addScene} onCreated={onSourceCreated} onClose={() => (adding = false)} />
{/if}

{#if propsForSource}
  <PropertiesModal
    kind="source"
    ref={propsForSource}
    title={"Properties — " + propsForSource}
    onClose={closeProps}
  />
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
  /* The disabled-preview placeholder adds a real action (Re-enable); opt back into
     pointer events for just that button rather than the whole overlay. */
  .placeholder button {
    pointer-events: auto;
    margin-top: 10px;
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
