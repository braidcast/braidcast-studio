<script lang="ts">
  import {
    obs,
    type CanvasInfo,
    type EncoderType,
    type OutputBindingInfo,
    type StreamProfileInfo,
    type MultistreamStatus,
    type MultistreamState,
  } from "../bridge";
  import { canvasStore } from "../canvasStore.svelte";
  import { outputBindingStore } from "../outputBindingStore.svelte";
  import { streamProfileStore } from "../streamProfileStore.svelte";
  import CanvasEditor from "../CanvasEditor.svelte";
  import CollectionDialog, { type DialogSpec } from "../CollectionDialog.svelte";
  import { STATE_COLOR_EXT } from "../theme/stateColors";
  import { callOrToast } from "../callToast";
  import Icon from "../dock/Icon.svelte";

  // Canvases page: a master-detail layout. The left list selects a canvas; the right
  // pane is that canvas's live-applied detail editor (Video · Encoding · Audio ·
  // Destinations · Advanced). Going live is the Studio GO-LIVE bar, never per-row here.
  // Canvas / binding / profile lists come from the shared stores (one source of
  // truth); encoders + live-status rows stay local to this page.
  let canvases = $derived(canvasStore.canvases);
  let bindings = $derived(outputBindingStore.bindings);
  let profiles = $derived(streamProfileStore.profiles);
  let live = $state<MultistreamStatus[]>([]);
  let videoEncoders = $state<EncoderType[]>([]);
  let audioEncoders = $state<EncoderType[]>([]);
  let loaded = $state(false);
  let error = $state<string | null>(null);

  let dialog = $state<DialogSpec | null>(null);

  function loadLive(): void {
    obs
      .call("multistream.status")
      .then((r) => (live = r.outputs))
      .catch(() => {});
  }

  async function loadAll(): Promise<void> {
    error = null;
    try {
      const [venc, aenc, status] = await Promise.all([
        obs.call("encoderTypes.list", { kind: "video" }),
        obs.call("encoderTypes.list", { kind: "audio" }),
        obs.call("multistream.status"),
      ]);
      videoEncoders = venc;
      audioEncoders = aenc;
      live = status.outputs;
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
    void loadAll();
    // A binding toggle changes live status; re-poll (the binding LIST refreshes via
    // the store's own outputBinding.changed subscription).
    const offBindings = obs.on("outputBinding.changed", () => loadLive());
    const offMulti = obs.on("multistream.changed", (p) => (live = p.outputs));
    return () => {
      offBindings();
      offMulti();
    };
  });

  // bindingUuid -> live status row.
  const statusByBinding = $derived.by<Map<string, MultistreamStatus>>(() => {
    const m = new Map<string, MultistreamStatus>();
    for (const o of live) {
      m.set(o.bindingUuid, o);
    }
    return m;
  });

  // The strongest live state across a canvas's enabled bindings (drives the list dot).
  function canvasState(uuid: string): MultistreamState | "off" {
    const rows = bindings.filter((b) => b.canvasUuid === uuid && b.enabled);
    if (rows.length === 0) return "off";
    const states = rows.map((b) => statusByBinding.get(b.uuid)?.state ?? "idle");
    if (states.includes("live")) return "live";
    if (states.includes("error")) return "error";
    if (states.includes("connecting")) return "connecting";
    return "idle";
  }

  function fpsText(c: CanvasInfo): string {
    if (!(c.fpsDen > 0)) return String(c.fpsNum);
    return c.fpsDen > 1 ? (c.fpsNum / c.fpsDen).toFixed(2) : String(c.fpsNum);
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
  const selectedLive = $derived(
    selectedUuid ? canvasState(selectedUuid) === "live" || canvasState(selectedUuid) === "connecting" : false,
  );

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
    const label = b.profileLabel === "(unset)" ? "No destination" : b.profileLabel;
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
      <aside class="cv-clist">
        <div class="cv-clist__head">
          <span class="cv-clist__title">Canvases</span>
          <span class="cv-clist__count">{canvases.length}</span>
        </div>
        <div class="cv-clist__body">
          {#each canvases as c (c.uuid)}
            {@const st = canvasState(c.uuid)}
            <button class="cv-ci" class:on={c.uuid === selectedUuid} onclick={() => (selectedUuid = c.uuid)}>
              <span class="cv-ci__dot" style:background={STATE_COLOR_EXT[st]}></span>
              <span class="cv-ci__body">
                <span class="cv-ci__name">{c.name}</span>
                <span class="cv-ci__sub">{c.outputWidth}×{c.outputHeight} · {fpsText(c)}fps · {destCount(c.uuid)} dest</span>
              </span>
              {#if c.isDefault}<span class="cv-ci__badge">DEF</span>{/if}
            </button>
          {/each}
          <button class="cv-newcanvas" onclick={addCanvas}><Icon name="plus" size={13} /><span>New Canvas</span></button>
        </div>
      </aside>
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
    </div>
  {/if}
</div>

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
</style>
