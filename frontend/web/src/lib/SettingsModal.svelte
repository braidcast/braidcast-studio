<script lang="ts">
  import { obs, type VideoSettings, type AudioSettings, type SpeakerLayout } from "./bridge";
  import CanvasesTab from "./CanvasesTab.svelte";

  interface Props {
    onClose: () => void;
  }
  let { onClose }: Props = $props();

  // Data-driven tab list: add a tab by appending one row (and its render branch
  // below). Streams/Outputs arrive in 4.4.2/4.4.3.
  const tabs = [
    { id: "video", label: "Video" },
    { id: "audio", label: "Audio" },
    { id: "canvases", label: "Canvases" },
  ] as const;
  type TabId = (typeof tabs)[number]["id"];
  let activeTab = $state<TabId>("video");

  // Common presets the UI offers; custom values are still accepted via the fields.
  const resPresets: { label: string; w: number; h: number }[] = [
    { label: "1920 × 1080", w: 1920, h: 1080 },
    { label: "1280 × 720", w: 1280, h: 720 },
    { label: "2560 × 1440", w: 2560, h: 1440 },
    { label: "3840 × 2160", w: 3840, h: 2160 },
  ];
  const fpsPresets = [24, 30, 48, 60];
  const sampleRates = [44100, 48000];
  const speakerOptions: { label: string; value: SpeakerLayout }[] = [
    { label: "Mono", value: "mono" },
    { label: "Stereo", value: "stereo" },
  ];

  // Form state, hydrated from the live config on open.
  let baseWidth = $state(1920);
  let baseHeight = $state(1080);
  let outputWidth = $state(1920);
  let outputHeight = $state(1080);
  let fps = $state(60);
  let sampleRate = $state<number>(48000);
  let speakers = $state<SpeakerLayout>("stereo");

  let loaded = $state(false);
  let videoError = $state<string | null>(null);
  let audioError = $state<string | null>(null);
  let videoNotice = $state<string | null>(null);
  let audioNotice = $state<string | null>(null);
  let applyingVideo = $state(false);
  let applyingAudio = $state(false);

  // The live-change guard is server-side (AnyOutputActive); no output exists yet,
  // so Apply stays enabled. When 4.4 wires real outputs this becomes a reflected
  // flag from a settings.* call; for now the server simply rejects with an error.

  async function load() {
    try {
      const [v, a] = await Promise.all([obs.call("settings.getVideo"), obs.call("settings.getAudio")]);
      hydrateVideo(v);
      sampleRate = a.sampleRate;
      speakers = a.speakers;
    } catch (e) {
      videoError = (e as Error).message;
    } finally {
      loaded = true;
    }
  }

  function hydrateVideo(v: VideoSettings) {
    baseWidth = v.baseWidth;
    baseHeight = v.baseHeight;
    outputWidth = v.outputWidth;
    outputHeight = v.outputHeight;
    // Express FPS as a whole number when fpsDen is 1 (the common case).
    fps = v.fpsDen > 0 ? Math.round(v.fpsNum / v.fpsDen) : v.fpsNum;
  }

  $effect(() => {
    void load();
  });

  function applyBasePreset(w: number, h: number) {
    baseWidth = w;
    baseHeight = h;
  }
  function applyOutputPreset(w: number, h: number) {
    outputWidth = w;
    outputHeight = h;
  }

  function positiveInt(n: number, max: number): boolean {
    return Number.isInteger(n) && n > 0 && n <= max;
  }

  const videoValid = $derived(
    positiveInt(baseWidth, 16384) &&
      positiveInt(baseHeight, 16384) &&
      positiveInt(outputWidth, 16384) &&
      positiveInt(outputHeight, 16384) &&
      positiveInt(fps, 1000),
  );

  async function applyVideo() {
    if (!videoValid || applyingVideo) return;
    applyingVideo = true;
    videoError = null;
    videoNotice = null;
    try {
      const applied = await obs.call("settings.setVideo", {
        baseWidth,
        baseHeight,
        outputWidth,
        outputHeight,
        fpsNum: fps,
        fpsDen: 1,
      });
      hydrateVideo(applied);
      videoNotice = "Video settings applied.";
    } catch (e) {
      videoError = (e as Error).message;
    } finally {
      applyingVideo = false;
    }
  }

  async function applyAudio() {
    if (applyingAudio) return;
    applyingAudio = true;
    audioError = null;
    audioNotice = null;
    try {
      const applied = await obs.call("settings.setAudio", { sampleRate, speakers });
      sampleRate = applied.sampleRate;
      speakers = applied.speakers;
      audioNotice = "Audio settings applied.";
    } catch (e) {
      audioError = (e as Error).message;
    } finally {
      applyingAudio = false;
    }
  }

  function onKeydown(e: KeyboardEvent) {
    if (e.key === "Escape") onClose();
  }
</script>

<svelte:window onkeydown={onKeydown} />

<div
  class="modal-backdrop"
  role="presentation"
  onclick={(e) => {
    if (e.target === e.currentTarget) onClose();
  }}
>
  <div class="modal" role="dialog" aria-modal="true" aria-label="Settings">
    <header class="modal-head">
      <h3>Settings</h3>
      <button class="icon close" title="Close" onclick={onClose}>✕</button>
    </header>

    <div class="tabs" role="tablist">
      {#each tabs as t (t.id)}
        <button
          class="tab"
          class:active={activeTab === t.id}
          role="tab"
          aria-selected={activeTab === t.id}
          onclick={() => (activeTab = t.id)}>{t.label}</button
        >
      {/each}
    </div>

    <div class="modal-body">
      {#if activeTab === "canvases"}
        <CanvasesTab />
      {:else if !loaded}
        <p class="dim">Loading settings…</p>
      {:else if activeTab === "video"}
        <section class="group">
          <h4>Video</h4>

          <div class="field">
            <span class="flabel">Base (Canvas) Resolution</span>
            <div class="presets">
              {#each resPresets as p (p.label)}
                <button
                  class="chip"
                  class:active={baseWidth === p.w && baseHeight === p.h}
                  onclick={() => applyBasePreset(p.w, p.h)}>{p.label}</button
                >
              {/each}
            </div>
            <div class="wh">
              <input type="number" min="1" max="16384" bind:value={baseWidth} aria-label="Base width" />
              <span class="x">×</span>
              <input type="number" min="1" max="16384" bind:value={baseHeight} aria-label="Base height" />
            </div>
          </div>

          <div class="field">
            <span class="flabel">Output (Scaled) Resolution</span>
            <div class="presets">
              {#each resPresets as p (p.label)}
                <button
                  class="chip"
                  class:active={outputWidth === p.w && outputHeight === p.h}
                  onclick={() => applyOutputPreset(p.w, p.h)}>{p.label}</button
                >
              {/each}
            </div>
            <div class="wh">
              <input type="number" min="1" max="16384" bind:value={outputWidth} aria-label="Output width" />
              <span class="x">×</span>
              <input type="number" min="1" max="16384" bind:value={outputHeight} aria-label="Output height" />
            </div>
          </div>

          <div class="field">
            <span class="flabel">Frame Rate (FPS)</span>
            <div class="presets">
              {#each fpsPresets as f (f)}
                <button class="chip" class:active={fps === f} onclick={() => (fps = f)}>{f}</button>
              {/each}
            </div>
            <div class="wh">
              <input type="number" min="1" max="1000" bind:value={fps} aria-label="FPS" />
            </div>
          </div>

          {#if videoError}<p class="error">{videoError}</p>{/if}
          {#if videoNotice}<p class="notice">{videoNotice}</p>{/if}
          <div class="actions">
            <button class="btn primary" disabled={!videoValid || applyingVideo} onclick={() => void applyVideo()}>
              {applyingVideo ? "Applying…" : "Apply Video"}
            </button>
          </div>
        </section>
      {:else if activeTab === "audio"}
        <section class="group">
          <h4>Audio</h4>

          <div class="field">
            <span class="flabel">Sample Rate</span>
            <div class="presets">
              {#each sampleRates as r (r)}
                <button class="chip" class:active={sampleRate === r} onclick={() => (sampleRate = r)}>{r} Hz</button>
              {/each}
            </div>
          </div>

          <div class="field">
            <span class="flabel">Channels</span>
            <div class="presets">
              {#each speakerOptions as o (o.value)}
                <button class="chip" class:active={speakers === o.value} onclick={() => (speakers = o.value)}
                  >{o.label}</button
                >
              {/each}
            </div>
          </div>

          {#if audioError}<p class="error">{audioError}</p>{/if}
          {#if audioNotice}<p class="notice">{audioNotice}</p>{/if}
          <div class="actions">
            <button class="btn primary" disabled={applyingAudio} onclick={() => void applyAudio()}>
              {applyingAudio ? "Applying…" : "Apply Audio"}
            </button>
          </div>
        </section>
      {/if}
    </div>

    <footer class="modal-foot">
      <button class="btn ghost" onclick={onClose}>Close</button>
    </footer>
  </div>
</div>

<style>
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
    background: var(--bg-raised);
    border: 1px solid var(--border);
    border-radius: 12px;
    width: min(640px, 100%);
    max-height: 86vh;
    display: flex;
    flex-direction: column;
    box-shadow: 0 18px 48px rgba(0, 0, 0, 0.5);
  }
  .tabs {
    display: flex;
    gap: 2px;
    padding: 0 18px;
    border-bottom: 1px solid var(--border);
  }
  .tab {
    background: none;
    border: none;
    border-bottom: 2px solid transparent;
    color: var(--text-dim);
    font: inherit;
    font-size: 13px;
    padding: 10px 12px;
    cursor: pointer;
    margin-bottom: -1px;
  }
  .tab:hover {
    color: var(--text);
  }
  .tab.active {
    color: var(--text);
    border-bottom-color: var(--accent);
  }
  .modal-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 14px 18px;
    border-bottom: 1px solid var(--border);
  }
  .modal-head h3 {
    margin: 0;
    font-size: 14px;
    font-weight: 600;
  }
  .modal-body {
    padding: 6px 18px 14px;
    overflow: auto;
  }
  .modal-foot {
    display: flex;
    justify-content: flex-end;
    padding: 12px 18px;
    border-top: 1px solid var(--border);
  }
  .group {
    padding: 12px 0;
    border-bottom: 1px solid var(--border);
  }
  .group:last-child {
    border-bottom: none;
  }
  .group h4 {
    margin: 0 0 10px;
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--text-dim);
  }
  .field {
    margin-bottom: 12px;
  }
  .flabel {
    display: block;
    font-size: 12px;
    color: var(--text-soft);
    margin-bottom: 6px;
  }
  .presets {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
    margin-bottom: 6px;
  }
  .chip {
    border: 1px solid var(--border);
    background: var(--bg-sunken);
    color: var(--text-soft);
    border-radius: 999px;
    padding: 4px 11px;
    font: inherit;
    font-size: 12px;
    cursor: pointer;
  }
  .chip:hover {
    color: var(--text);
  }
  .chip.active {
    background: var(--accent);
    border-color: var(--accent);
    color: #fff;
  }
  .wh {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .x {
    color: var(--text-dim);
  }
  input {
    background: var(--bg-sunken);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 7px 10px;
    color: var(--text);
    font: inherit;
    width: 96px;
  }
  input:focus {
    outline: none;
    border-color: var(--accent);
  }
  .actions {
    display: flex;
    justify-content: flex-end;
    margin-top: 4px;
  }
  .icon {
    background: none;
    border: none;
    color: var(--text-dim);
    cursor: pointer;
    padding: 2px 4px;
    border-radius: 4px;
    font-size: 14px;
    line-height: 1;
  }
  .icon:hover {
    color: var(--text);
    background: var(--bg-sunken);
  }
  .btn {
    border-radius: 6px;
    padding: 7px 14px;
    font: inherit;
    cursor: pointer;
    border: 1px solid var(--border);
    background: var(--bg-sunken);
    color: var(--text-soft);
  }
  .btn:hover:not(:disabled) {
    color: var(--text);
  }
  .btn.primary {
    background: var(--accent);
    border-color: var(--accent);
    color: #fff;
  }
  .btn.primary:disabled {
    opacity: 0.45;
    cursor: default;
  }
  .btn.ghost {
    background: none;
  }
  .dim {
    color: var(--text-dim);
    margin: 0;
  }
  .error {
    color: var(--off, #d65a5a);
    margin: 6px 0 0;
    font-size: 12px;
  }
  .notice {
    color: var(--on, #4caf50);
    margin: 6px 0 0;
    font-size: 12px;
  }
</style>
