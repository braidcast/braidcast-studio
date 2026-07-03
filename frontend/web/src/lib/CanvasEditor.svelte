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
  import Icon, { type IconName } from "./dock/Icon.svelte";
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
  // Each sub-tab carries its mock icon; `noInherit` marks a tab with no Use-Default
  // strip (Destinations) so it shows the mock's trailing "·" marker.
  const TABS: { id: Tab; label: string; icon: IconName; noInherit?: boolean }[] = [
    { id: "video", label: "Video", icon: "video" },
    { id: "encoding", label: "Encoding", icon: "cpu" },
    { id: "audio", label: "Audio", icon: "audio" },
    { id: "destinations", label: "Destinations", icon: "destinations", noInherit: true },
    { id: "advanced", label: "Advanced", icon: "advanced" },
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
</script>

<div class="detail">
  <header class="cfhead">
    <div class="cfhead__top">
      <div class="cfhead__id">
        <input
          class="cfhead__name"
          type="text"
          bind:value={form.name}
          disabled={isDefault}
          placeholder="Canvas name"
          aria-label="Canvas name"
          onchange={commit}
        />
        {#if isDefault}<span class="cv-badge cv-badge--default">Default</span>{/if}
        {#if isLive}<span class="cv-badge cv-badge--live"><i></i>Live</span>{/if}
      </div>
      <div class="cfhead__meta">
        <b>{form.width}×{form.height}</b> · {fpsText()}fps · {encName(videoEncoders, form.videoEnc)}
      </div>
      {#if !isDefault}
        <button
          class="cfhead__del"
          disabled={isLive}
          title={isLive ? "Stop the stream first" : "Delete this canvas"}
          aria-label="Delete this canvas"
          onclick={onDelete}><Icon name="trash" size={14} /></button
        >
      {/if}
    </div>
    <div class="cfhead__note" class:live={isLive}>
      <Icon name="lock" size={12} />
      <span>Resolution &amp; FPS lock while live · encoding edits apply on next stream start</span>
    </div>
  </header>

  <div class="cv-subtabs" role="tablist" aria-label="Canvas settings">
    {#each TABS as t (t.id)}
      <button
        type="button"
        class="cv-subtab"
        class:on={activeTab === t.id}
        role="tab"
        aria-selected={activeTab === t.id}
        onclick={() => (activeTab = t.id)}
      >
        <Icon name={t.icon} size={14} />
        <span>{t.label}</span>
        {#if t.noInherit}<span class="cv-subtab__n">·</span>{/if}
      </button>
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
  .panel {
    flex: 1;
    min-height: 0;
    overflow: auto;
  }
</style>
