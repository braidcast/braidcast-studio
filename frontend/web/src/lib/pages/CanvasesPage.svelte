<script lang="ts">
  import {
    obs,
    type CanvasInfo,
    type EncoderType,
    type OutputBindingInfo,
    type StreamProfileInfo,
    type MultistreamState,
  } from "$lib/api/bridge";
  import { canvasStore } from "$lib/stores/canvasStore.svelte";
  import { outputBindingStore, bindingDisplayName } from "$lib/stores/outputBindingStore.svelte";
  import { streamProfileStore } from "$lib/stores/streamProfileStore.svelte";
  import { multistreamStatusStore, isActiveState } from "$lib/stores/multistreamStatusStore.svelte";
  import { fmtFps } from "$lib/utils/format";
  import CanvasEditor from "$lib/canvas/CanvasEditor.svelte";
  import CollectionDialog, { type DialogSpec } from "$lib/dialogs/CollectionDialog.svelte";
  import { STATE_COLOR_EXT } from "$lib/theme/stateColors";
  import { callOrToast } from "$lib/utils/callToast";
  import Icon from "$lib/ui/Icon.svelte";
  import SplitPane from "$lib/ui/SplitPane.svelte";
  import { createReorder } from "$lib/utils/listReorder.svelte";

  // Canvases page: a master-detail layout. The left list selects a canvas; the right
  // pane is that canvas's live-applied detail editor (Video · Encoding · Audio ·
  // Destinations · Advanced). Going live is the Studio GO-LIVE bar, never per-row here.
  // Canvas / binding / profile lists AND live status come from the shared stores
  // (one source of truth per leg); only the encoder lists stay local to this page.
  let canvases = $derived(canvasStore.canvases);
  // The Default canvas is pinned to the top of the list and excluded from
  // drag-to-reorder; only the remaining canvases participate in reorder.
  let defaultCanvas = $derived(canvases.find((c) => c.isDefault) ?? null);
  let otherCanvases = $derived(canvases.filter((c) => !c.isDefault));
  let bindings = $derived(outputBindingStore.bindings);
  let profiles = $derived(streamProfileStore.profiles);
  let statusByBinding = $derived(multistreamStatusStore.statusByBinding);
  let videoEncoders = $state<EncoderType[]>([]);
  let audioEncoders = $state<EncoderType[]>([]);
  let loaded = $state(false);
  let error = $state<string | null>(null);

  let dialog = $state<DialogSpec | null>(null);

  async function loadEncoders(): Promise<void> {
    error = null;
    try {
      const [venc, aenc] = await Promise.all([
        obs.call("encoderTypes.list", { kind: "video" }),
        obs.call("encoderTypes.list", { kind: "audio" }),
      ]);
      videoEncoders = venc;
      audioEncoders = aenc;
    } catch (e) {
      error = (e as Error).message;
    } finally {
      loaded = true;
    }
  }

  $effect(() => {
    canvasStore.start();
    outputBindingStore.start();
    streamProfileStore.start();
    void loadEncoders();
    // Live status (fetch + multistream.changed + outputBinding.changed re-poll) is
    // owned by the shared store.
    return multistreamStatusStore.subscribe();
  });

  // The strongest live state across a canvas's enabled bindings (drives the list dot).
  function canvasState(uuid: string): MultistreamState | "off" {
    return multistreamStatusStore.deriveCanvasState(bindings.filter((b) => b.canvasUuid === uuid));
  }

  // Destinations bound to a canvas (drives the "· N dest" meta on each list row).
  function destCount(uuid: string): number {
    return bindings.filter((b) => b.canvasUuid === uuid).length;
  }

  // Selection: default to the Default canvas (or first) once loaded, and re-pick if the
  // current selection disappears (deleted).
  let selectedUuid = $state<string | null>(null);
  let wantUuid = $state<string | null>(null);
  $effect(() => {
    if (wantUuid && canvases.some((c) => c.uuid === wantUuid)) {
      selectedUuid = wantUuid;
      wantUuid = null;
      return;
    }
    if (selectedUuid && canvases.some((c) => c.uuid === selectedUuid)) return;
    if (wantUuid) return; // waiting for the new canvas to appear in the list
    selectedUuid = (canvases.find((c) => c.isDefault) ?? canvases[0])?.uuid ?? null;
  });
  const selected = $derived(canvases.find((c) => c.uuid === selectedUuid) ?? null);
  const selectedLive = $derived(selectedUuid ? isActiveState(canvasState(selectedUuid)) : false);

  // Pointer drag-to-reorder the canvas list, restricted to the non-default canvases
  // (the Default canvas is pinned first and never part of `order`). Optimistically
  // reorders the shared store array (avoids flicker), then persists via
  // canvas.reorder; the backend's canvas.changed emit + store refresh reconciles, so
  // a failed call is non-fatal. The Default's uuid is re-prepended before the
  // backend call so ReorderByUuid (which appends ids absent from `order`) can't drop
  // it to the end of the persisted order.
  const reorder = createReorder({
    getIds: () => canvasStore.canvases.filter((c) => !c.isDefault).map((c) => c.uuid),
    commit: async (order) => {
      const by = new Map(canvasStore.canvases.map((c) => [c.uuid, c]));
      const def = canvasStore.canvases.find((c) => c.isDefault);
      const reordered = order.map((id) => by.get(id)).filter((c): c is CanvasInfo => !!c);
      canvasStore.canvases = def ? [def, ...reordered] : reordered;
      try {
        await obs.call("canvas.reorder", { order: def ? [def.uuid, ...order] : order });
      } catch {
        // Reconciled by the canvas.changed emit + store refresh.
      }
    },
  });
  const dragRow = reorder.row;

  // Inline add-canvas: a name prompt -> canvas.create seeded with the Default canvas's
  // resolution/FPS and the spec's inheritance defaults (Audio + Advanced inherited).
  function addCanvas(): void {
    dialog = {
      kind: "prompt",
      title: "New Canvas",
      confirmLabel: "Create",
      onCommit: async (name) => {
        if (!name) return;
        const def = canvases.find((c) => c.isDefault);
        const r = await callOrToast(
          "canvas.create",
          {
            name,
            baseWidth: def?.baseWidth ?? 1920,
            baseHeight: def?.baseHeight ?? 1080,
            fpsNum: def?.fpsNum ?? 60,
            fpsDen: def?.fpsDen ?? 1,
            audioUseDefault: true,
            color: { useDefault: true },
          },
          "Create canvas failed",
        );
        if (r) wantUuid = r.uuid;
      },
    };
  }
  function confirmDeleteCanvas(c: CanvasInfo): void {
    dialog = {
      kind: "confirm",
      title: "Delete Canvas",
      message: `Delete canvas "${c.name}"? This removes the canvas and unbinds its destinations.`,
      confirmLabel: "Delete",
      onCommit: () => void callOrToast("canvas.remove", { uuid: c.uuid }, "Delete canvas failed"),
    };
  }
  function confirmRemoveBinding(b: OutputBindingInfo): void {
    const label = bindingDisplayName(b);
    dialog = {
      kind: "confirm",
      title: "Unbind Destination",
      message: `Unbind "${label}" from this canvas?`,
      confirmLabel: "Unbind",
      onCommit: async () => {
        await callOrToast("outputBinding.remove", { uuid: b.uuid }, "Unbind failed");
        void outputBindingStore.refresh();
      },
    };
  }
</script>

<div class="page">
  <div class="cv-crumb">
    <span class="cv-crumb__ic"><Icon name="canvas" size={15} /></span>
    <span class="cv-crumb__t">Canvases</span>
    <span class="cv-crumb__sep">/</span>
    <span class="cv-crumb__sub">Configuration</span>
    <span class="cv-crumb__spacer"></span>
    <span class="cv-crumb__note">Each canvas = one independent encode target</span>
  </div>
  {#if error}<p class="err">{error}</p>{/if}
  {#if !loaded}
    <p class="dim">Loading canvases…</p>
  {:else}
    <div class="split">
      <SplitPane storageKey="braidcast.split.canvases" default={236} left={leftList} right={detailPane} />
    </div>
  {/if}
</div>

{#snippet leftList()}
  <aside class="cv-clist">
    <div class="cv-clist__head">
      <span class="cv-clist__title">Canvases</span>
      <span class="cv-clist__count">{canvases.length}</span>
    </div>
    <div class="cv-clist__body">
      {#if defaultCanvas}
        {@const c = defaultCanvas}
        {@const st = canvasState(c.uuid)}
        <button
          class="cv-ci"
          class:on={c.uuid === selectedUuid}
          onclick={() => (selectedUuid = c.uuid)}
        >
          <span class="cv-ci__dot" style:background={STATE_COLOR_EXT[st]}></span>
          <span class="cv-ci__body">
            <span class="cv-ci__name">{c.name}</span>
            <span class="cv-ci__sub">{c.outputWidth}×{c.outputHeight} · {fmtFps(c.fpsNum, c.fpsDen)}fps · {destCount(c.uuid)} dest</span>
          </span>
          <span class="cv-ci__badge">DEF</span>
        </button>
        <div class="cv-clist__divider" role="separator"></div>
      {/if}
      {#each otherCanvases as c, i (c.uuid)}
        {@const st = canvasState(c.uuid)}
        {#if reorder.dragging && reorder.dropIndex === i}<div class="reorder-line"></div>{/if}
        <button
          class="cv-ci"
          class:on={c.uuid === selectedUuid}
          class:lifting={reorder.dragIndex === i}
          use:dragRow={i}
          onclick={() => {
            if (reorder.consumeClick()) return;
            selectedUuid = c.uuid;
          }}
        >
          <span class="cv-ci__dot" style:background={STATE_COLOR_EXT[st]}></span>
          <span class="cv-ci__body">
            <span class="cv-ci__name">{c.name}</span>
            <span class="cv-ci__sub">{c.outputWidth}×{c.outputHeight} · {fmtFps(c.fpsNum, c.fpsDen)}fps · {destCount(c.uuid)} dest</span>
          </span>
        </button>
      {/each}
      {#if reorder.dragging && reorder.dropIndex === otherCanvases.length}<div class="reorder-line"></div>{/if}
      <button class="cv-newcanvas" onclick={addCanvas}><Icon name="plus" size={13} /><span>New Canvas</span></button>
    </div>
  </aside>
{/snippet}

{#snippet detailPane()}
  <section class="pane">
    {#if selected}
      {#key selected.uuid}
        <CanvasEditor
          canvas={selected}
          {videoEncoders}
          {audioEncoders}
          {bindings}
          {profiles}
          {statusByBinding}
          isLive={selectedLive}
          onDelete={() => confirmDeleteCanvas(selected)}
          onBindingsChanged={() => void outputBindingStore.refresh()}
          onRemoveBinding={confirmRemoveBinding}
        />
      {/key}
    {/if}
  </section>
{/snippet}

{#if dialog}
  <CollectionDialog {...dialog} onClose={() => (dialog = null)} />
{/if}

<style>
  .page {
    height: 100%;
    display: flex;
    flex-direction: column;
    min-height: 0;
    background: var(--color-base);
    color: var(--color-text);
  }
  .split {
    flex: 1;
    min-height: 0;
    display: flex;
  }
  .pane {
    flex: 1;
    min-width: 0;
  }
  .dim {
    color: var(--color-muted);
    margin: 16px 24px;
  }
  .err {
    margin: 12px 24px;
    color: var(--color-live);
    font-size: 12px;
  }
  .reorder-line {
    height: 2px;
    margin: 0 6px;
    background: var(--color-accent);
  }
  .cv-ci.lifting {
    opacity: 0.4;
  }
</style>
