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
  import CanvasEditor from "../CanvasEditor.svelte";
  import CollectionDialog, { type DialogSpec } from "../CollectionDialog.svelte";
  import { STATE_COLOR_EXT } from "../theme/stateColors";
  import { callOrToast } from "../callToast";
  import PageHeader from "../PageHeader.svelte";
  import Icon from "../dock/Icon.svelte";

  // Canvases page: a master-detail layout. The left list selects a canvas; the right
  // pane is that canvas's live-applied detail editor (Video · Encoding · Audio ·
  // Destinations · Advanced). Going live is the Studio GO-LIVE bar, never per-row here.
  let canvases = $state<CanvasInfo[]>([]);
  let bindings = $state<OutputBindingInfo[]>([]);
  let profiles = $state<StreamProfileInfo[]>([]);
  let live = $state<MultistreamStatus[]>([]);
  let videoEncoders = $state<EncoderType[]>([]);
  let audioEncoders = $state<EncoderType[]>([]);
  let loaded = $state(false);
  let error = $state<string | null>(null);

  let dialog = $state<DialogSpec | null>(null);

  async function loadCanvases(): Promise<void> {
    try {
      canvases = await obs.call("canvas.list");
    } catch (e) {
      error = (e as Error).message;
    }
  }
  function loadBindings(): void {
    obs
      .call("outputBinding.list")
      .then((l) => (bindings = l))
      .catch(() => {});
  }
  function loadProfiles(): void {
    obs
      .call("streamProfile.list")
      .then((l) => (profiles = l))
      .catch(() => {});
  }
  function loadLive(): void {
    obs
      .call("multistream.status")
      .then((r) => (live = r.outputs))
      .catch(() => {});
  }

  async function loadAll(): Promise<void> {
    error = null;
    try {
      const [list, venc, aenc, binds, profs, status] = await Promise.all([
        obs.call("canvas.list"),
        obs.call("encoderTypes.list", { kind: "video" }),
        obs.call("encoderTypes.list", { kind: "audio" }),
        obs.call("outputBinding.list"),
        obs.call("streamProfile.list"),
        obs.call("multistream.status"),
      ]);
      canvases = list;
      videoEncoders = venc;
      audioEncoders = aenc;
      bindings = binds;
      profiles = profs;
      live = status.outputs;
    } catch (e) {
      error = (e as Error).message;
    } finally {
      loaded = true;
    }
  }

  $effect(() => {
    void loadAll();
    const offCanvas = obs.on("canvas.changed", () => void loadCanvases());
    const offBindings = obs.on("outputBinding.changed", () => {
      loadBindings();
      loadLive();
    });
    const offMulti = obs.on("multistream.changed", (p) => (live = p.outputs));
    const offProfiles = obs.on("streamProfile.changed", loadProfiles);
    return () => {
      offCanvas();
      offBindings();
      offMulti();
      offProfiles();
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

  // Selection: default to the Default canvas (or first) once loaded, and re-pick if the
  // current selection disappears (deleted).
  let selectedUuid = $state<string | null>(null);
  $effect(() => {
    if (selectedUuid && canvases.some((c) => c.uuid === selectedUuid)) return;
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
        if (r) selectedUuid = r.uuid;
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
        loadBindings();
      },
    };
  }
</script>

<div class="page">
  <PageHeader title="Canvases" sub="encode targets · one per resolution/FPS" />
  {#if error}<p class="err">{error}</p>{/if}
  {#if !loaded}
    <p class="dim">Loading canvases…</p>
  {:else}
    <div class="split">
      <aside class="list">
        {#each canvases as c (c.uuid)}
          {@const st = canvasState(c.uuid)}
          <button class="ci" class:on={c.uuid === selectedUuid} onclick={() => (selectedUuid = c.uuid)}>
            <span class="ci-dot" style:background={STATE_COLOR_EXT[st]}></span>
            <span class="ci-body">
              <span class="ci-name">{c.name}</span>
              <span class="ci-sub">{c.outputWidth}×{c.outputHeight} · {fpsText(c)}fps</span>
            </span>
            {#if c.isDefault}<span class="ci-badge">DEF</span>{/if}
          </button>
        {/each}
        <button class="ci-add" onclick={addCanvas}><Icon name="plus" size={13} /><span>New Canvas</span></button>
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
              onBindingsChanged={loadBindings}
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
  .list {
    flex: 0 0 260px;
    display: flex;
    flex-direction: column;
    border-right: var(--border-weight) solid var(--color-border);
    overflow: auto;
  }
  .ci {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 12px 14px;
    text-align: left;
    background: none;
    border: 0;
    border-bottom: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    cursor: pointer;
  }
  .ci:hover {
    background: var(--color-surface);
  }
  .ci.on {
    background: var(--color-surface-2);
    box-shadow: inset 3px 0 0 var(--color-accent);
  }
  .ci-dot {
    width: 7px;
    height: 7px;
    flex: 0 0 auto;
  }
  .ci-body {
    display: flex;
    flex-direction: column;
    gap: 2px;
    min-width: 0;
    flex: 1;
  }
  .ci-name {
    font-size: 13px;
    font-weight: 600;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .ci-sub {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
  }
  .ci-badge {
    font-family: var(--font-mono);
    font-size: 8px;
    letter-spacing: 0.06em;
    color: var(--color-accent-ink);
    background: var(--color-accent);
    padding: 2px 4px;
    flex: 0 0 auto;
  }
  .ci-add {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    padding: 12px 14px;
    background: none;
    border: 0;
    border-top: var(--border-weight) dashed var(--color-border);
    color: var(--color-dim);
    cursor: pointer;
    font-family: var(--font-ui);
    font-size: 12px;
    margin-top: auto;
  }
  .ci-add:hover {
    color: var(--color-accent);
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
