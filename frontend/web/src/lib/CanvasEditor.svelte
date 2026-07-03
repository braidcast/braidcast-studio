<script lang="ts">
  import {
    type CanvasInfo,
    type EncoderType,
    type OutputBindingInfo,
    type StreamProfileInfo,
    type MultistreamStatus,
  } from "./bridge";
  import { seedForm, toUpdateParams } from "./canvas/canvasForm";
  import { callOrToast } from "./callToast";
  import Icon from "./dock/Icon.svelte";
  import CanvasVideoTab from "./canvas/CanvasVideoTab.svelte";
  import CanvasEncodingTab from "./canvas/CanvasEncodingTab.svelte";
  import CanvasAudioTab from "./canvas/CanvasAudioTab.svelte";
  import CanvasAdvancedTab from "./canvas/CanvasAdvancedTab.svelte";
  import CanvasDestinationsTab from "./canvas/CanvasDestinationsTab.svelte";

  interface Props {
    canvas: CanvasInfo;
    videoEncoders: EncoderType[];
    audioEncoders: EncoderType[];
    bindings: OutputBindingInfo[];
    profiles: StreamProfileInfo[];
    statusByBinding: Map<string, MultistreamStatus>;
    isLive: boolean;
    onDelete: () => void;
    onBindingsChanged: () => void;
    onRemoveBinding: (b: OutputBindingInfo) => void;
  }
  let {
    canvas,
    videoEncoders,
    audioEncoders,
    bindings,
    profiles,
    statusByBinding,
    isLive,
    onDelete,
    onBindingsChanged,
    onRemoveBinding,
  }: Props = $props();

  type Tab = "video" | "encoding" | "audio" | "destinations" | "advanced";
  const TABS: { id: Tab; label: string }[] = [
    { id: "video", label: "Video" },
    { id: "encoding", label: "Encoding" },
    { id: "audio", label: "Audio" },
    { id: "destinations", label: "Destinations" },
    { id: "advanced", label: "Advanced" },
  ];

  // Seeded once. The parent keys this component by canvas uuid, so it remounts (and
  // re-seeds) whenever a different canvas is selected.
  // svelte-ignore state_referenced_locally
  let form = $state(seedForm(canvas));
  let activeTab = $state<Tab>("video");
  const isDefault = $derived(canvas.isDefault);

  // Live-apply: every field change calls commit() (debounced) so there is no Save
  // button; encoder-type changes call commitNow() so the persisted blob exists before
  // PropertyForm re-fetches.
  let timer: ReturnType<typeof setTimeout> | null = null;
  async function push(): Promise<void> {
    await callOrToast("canvas.update", toUpdateParams(canvas.uuid, form), "Update canvas failed");
  }
  function commit(): void {
    if (timer) clearTimeout(timer);
    timer = setTimeout(() => {
      timer = null;
      void push();
    }, 200);
  }
  // Cancel any pending debounced commit if this editor is torn down ({#key}-remount).
  $effect(() => () => {
    if (timer) clearTimeout(timer);
  });
  async function commitNow(): Promise<void> {
    if (timer) {
      clearTimeout(timer);
      timer = null;
    }
    await push();
  }

  function encName(list: EncoderType[], id: string): string {
    return list.find((e) => e.id === id)?.name ?? id;
  }
  function fpsText(): string {
    return form.fpsDen > 1 ? (form.fpsNum / form.fpsDen).toFixed(2) : String(form.fpsNum);
  }
  const summary = $derived(`${form.width}×${form.height} · ${fpsText()}fps · ${encName(videoEncoders, form.videoEnc)}`);
</script>

<div class="detail">
  <header class="head">
    <input
      class="name"
      type="text"
      bind:value={form.name}
      disabled={isDefault}
      placeholder="Canvas name"
      aria-label="Canvas name"
      onchange={commit}
    />
    {#if isDefault}<span class="badge">Default</span>{/if}
    {#if isLive}<span class="live">● LIVE</span>{/if}
    <span class="spacer"></span>
    <span class="summary">{summary}</span>
    {#if !isDefault}
      <button
        class="del"
        disabled={isLive}
        title={isLive ? "Stop the stream first" : "Delete this canvas"}
        aria-label="Delete this canvas"
        onclick={onDelete}><Icon name="trash" size={13} /></button
      >
    {/if}
  </header>

  <div class="subtabs" role="tablist" aria-label="Canvas settings">
    {#each TABS as t (t.id)}
      <button
        type="button"
        class="subtab"
        class:on={activeTab === t.id}
        role="tab"
        aria-selected={activeTab === t.id}
        onclick={() => (activeTab = t.id)}>{t.label}</button
      >
    {/each}
  </div>

  <div class="panel">
    {#if activeTab === "video"}
      <CanvasVideoTab {form} {isLive} {isDefault} {commit} />
    {:else if activeTab === "encoding"}
      <CanvasEncodingTab
        {form}
        canvasUuid={canvas.uuid}
        {videoEncoders}
        {isLive}
        {isDefault}
        {commit}
        {commitNow}
      />
    {:else if activeTab === "audio"}
      <CanvasAudioTab {form} canvasUuid={canvas.uuid} {audioEncoders} {isLive} {isDefault} {commit} {commitNow} />
    {:else if activeTab === "destinations"}
      <CanvasDestinationsTab
        canvasUuid={canvas.uuid}
        {bindings}
        {profiles}
        {statusByBinding}
        onChanged={onBindingsChanged}
        onRemove={onRemoveBinding}
      />
    {:else}
      <CanvasAdvancedTab {form} {isLive} {isDefault} {commit} />
    {/if}
  </div>
</div>

<style>
  .detail {
    display: flex;
    flex-direction: column;
    height: 100%;
    min-height: 0;
  }
  .head {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 16px 20px;
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .name {
    background: none;
    border: var(--border-weight) solid transparent;
    color: var(--color-text);
    font: inherit;
    font-size: 16px;
    font-weight: 600;
    padding: 4px 6px;
    max-width: 320px;
  }
  .name:hover:not(:disabled),
  .name:focus {
    border-color: var(--color-border);
    outline: none;
  }
  .badge {
    font-family: var(--font-mono);
    font-size: 8px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--color-accent-ink);
    background: var(--color-accent);
    padding: 2px 5px;
  }
  .live {
    font-family: var(--font-mono);
    font-size: 9px;
    color: #fff;
    background: var(--color-live);
    padding: 2px 6px;
  }
  .spacer {
    flex: 1;
  }
  .summary {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
  }
  .del {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 28px;
    height: 26px;
    background: none;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-muted);
    cursor: pointer;
  }
  .del:hover:not(:disabled) {
    color: var(--color-live);
    border-color: var(--color-live);
  }
  .del:disabled {
    opacity: 0.4;
    cursor: default;
  }
  .subtabs {
    display: flex;
    gap: 0;
    padding: 0 20px;
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .subtab {
    border: 0;
    border-bottom: 2px solid transparent;
    background: none;
    color: var(--color-muted);
    font-family: var(--font-ui);
    font-size: 12px;
    font-weight: 600;
    padding: 10px 14px;
    margin-bottom: -1px;
    cursor: pointer;
  }
  .subtab:hover {
    color: var(--color-text);
  }
  .subtab.on {
    color: var(--color-text);
    border-bottom-color: var(--color-accent);
  }
  .panel {
    flex: 1;
    min-height: 0;
    overflow: auto;
    padding: 20px;
  }
</style>
