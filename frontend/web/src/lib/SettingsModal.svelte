<script lang="ts">
  import {
    obs,
    type SpeakerLayout,
    type AudioDevice,
    type GlobalAudioSlot,
  } from "./bridge";
  import CanvasesTab from "./CanvasesTab.svelte";
  import StreamsTab from "./StreamsTab.svelte";
  import OutputsTab from "./OutputsTab.svelte";
  import HotkeysTab from "./HotkeysTab.svelte";
  import McpTab from "./McpTab.svelte";
  import SettingsIcon from "./SettingsIcon.svelte";
  import { suspendPreview } from "./previewGate.svelte";

  interface Props {
    onClose: () => void;
    initialTab?: TabId;
    /** When opening on the Canvases tab, a canvas uuid to open for editing. */
    editCanvas?: string | null;
  }
  let { onClose, initialTab, editCanvas = null }: Props = $props();

  // Hide the native preview overlay while this modal is open (it would otherwise
  // paint over the modal's center).
  $effect(() => suspendPreview());

  // Sidebar categories, in setup/dependency order. Resolution/FPS used to be a
  // Video tab; it now lives on the Default canvas in the Canvases pane.
  const cats = [
    { id: "canvases", label: "Canvases" },
    { id: "streams", label: "Streams" },
    { id: "outputs", label: "Outputs" },
    { id: "audio", label: "Audio" },
    { id: "mcp", label: "AI Control" },
    { id: "hotkeys", label: "Hotkeys" },
  ] as const;
  type TabId = (typeof cats)[number]["id"];
  // initialTab is an open-time seed; the user switches tabs freely afterward.
  // svelte-ignore state_referenced_locally
  let activeTab = $state<TabId>(initialTab ?? "canvases");

  const sampleRates = [44100, 48000];
  const speakerOptions: { label: string; value: SpeakerLayout }[] = [
    { label: "Mono", value: "mono" },
    { label: "Stereo", value: "stereo" },
  ];

  // Audio form state, hydrated from the live config on open. Applied live on
  // change (no Apply button); the footer's transactional revert is Task 4.
  let sampleRate = $state<number>(48000);
  let speakers = $state<SpeakerLayout>("stereo");

  // Global audio device pickers (live, not gated behind Apply).
  let globalSlots = $state<GlobalAudioSlot[]>([]);
  let inputDevices = $state<AudioDevice[]>([]);
  let outputDevices = $state<AudioDevice[]>([]);
  let deviceError = $state<string | null>(null);

  let loaded = $state(false);
  let audioError = $state<string | null>(null);

  async function load() {
    try {
      const [a, slots, ins, outs] = await Promise.all([
        obs.call("settings.getAudio"),
        obs.call("audio.getGlobalDevices"),
        obs.call("audio.listDevices", { kind: "input" }),
        obs.call("audio.listDevices", { kind: "output" }),
      ]);
      sampleRate = a.sampleRate;
      speakers = a.speakers;
      globalSlots = slots;
      inputDevices = ins;
      outputDevices = outs;
    } catch (e) {
      audioError = (e as Error).message;
    } finally {
      loaded = true;
    }
  }

  $effect(() => {
    void load();
  });

  async function setAudio(patch: { sampleRate?: number; speakers?: SpeakerLayout }) {
    audioError = null;
    const next = { sampleRate, speakers, ...patch };
    // Reflect locally so the chips stay in sync immediately.
    sampleRate = next.sampleRate;
    speakers = next.speakers;
    try {
      const applied = await obs.call("settings.setAudio", next);
      sampleRate = applied.sampleRate;
      speakers = applied.speakers;
    } catch (e) {
      audioError = (e as Error).message;
    }
  }

  async function setGlobalDevice(channel: number, value: string) {
    deviceError = null;
    const deviceId = value === "" ? null : value;
    try {
      await obs.call("audio.setGlobalDevice", { channel, deviceId });
      // reflect the change locally so the select stays in sync
      globalSlots = globalSlots.map((s) =>
        s.channel === channel ? { ...s, deviceId, active: deviceId !== null } : s,
      );
    } catch (e) {
      deviceError = (e as Error).message;
    }
  }

  // Task 4 wires the real transactional apply; stub for now.
  function applyChanges() {}

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

    <div class="modal-body">
      <div class="side" role="tablist" aria-label="Settings categories">
        {#each cats as c (c.id)}
          <button
            class="item"
            class:active={activeTab === c.id}
            role="tab"
            aria-selected={activeTab === c.id}
            onclick={() => (activeTab = c.id)}
          >
            <SettingsIcon id={c.id} />
            <span>{c.label}</span>
          </button>
        {/each}
      </div>

      <div class="pane">
        {#if activeTab === "canvases"}
          <CanvasesTab {editCanvas} />
        {:else if activeTab === "streams"}
          <StreamsTab />
        {:else if activeTab === "outputs"}
          <OutputsTab />
        {:else if activeTab === "hotkeys"}
          <HotkeysTab />
        {:else if activeTab === "mcp"}
          <McpTab />
        {:else if activeTab === "audio"}
          {#if !loaded}
            <p class="dim">Loading settings…</p>
          {:else}
            <section class="group">
              <h4>Audio</h4>

              <div class="field">
                <span class="flabel">Sample Rate</span>
                <div class="presets">
                  {#each sampleRates as r (r)}
                    <button class="chip" class:active={sampleRate === r} onclick={() => void setAudio({ sampleRate: r })}
                      >{r} Hz</button
                    >
                  {/each}
                </div>
              </div>

              <div class="field">
                <span class="flabel">Channels</span>
                <div class="presets">
                  {#each speakerOptions as o (o.value)}
                    <button
                      class="chip"
                      class:active={speakers === o.value}
                      onclick={() => void setAudio({ speakers: o.value })}>{o.label}</button
                    >
                  {/each}
                </div>
              </div>

              {#if audioError}<p class="error">{audioError}</p>{/if}
              <p class="dim hint">Changes apply immediately.</p>
            </section>

            <section class="group">
              <h4>Global Audio Devices</h4>

              {#each globalSlots as slot (slot.channel)}
                {@const devices = slot.isInput ? inputDevices : outputDevices}
                {@const known = slot.deviceId === null || devices.some((d) => d.id === slot.deviceId)}
                <div class="field">
                  <span class="flabel">{slot.label}</span>
                  <select
                    value={slot.deviceId ?? ""}
                    onchange={(e) => void setGlobalDevice(slot.channel, e.currentTarget.value)}
                  >
                    <option value="">Disabled</option>
                    {#each devices as d (d.id)}
                      <option value={d.id}>{d.name}</option>
                    {/each}
                    {#if !known && slot.deviceId !== null}
                      <option value={slot.deviceId}>{slot.deviceId} (unavailable)</option>
                    {/if}
                  </select>
                </div>
              {/each}

              {#if deviceError}<p class="error">{deviceError}</p>{/if}
              <p class="dim hint">Changes apply immediately.</p>
            </section>
          {/if}
        {/if}
      </div>
    </div>

    <footer class="modal-foot">
      <button class="btn primary" onclick={onClose}>OK</button>
      <button class="btn" onclick={applyChanges}>Apply</button>
      <button class="btn ghost" onclick={onClose}>Cancel</button>
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
    width: min(880px, 100%);
    max-height: 86vh;
    display: flex;
    flex-direction: column;
    box-shadow: 0 18px 48px rgba(0, 0, 0, 0.5);
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
    display: flex;
    flex: 1;
    min-height: 360px;
    overflow: hidden;
  }
  .side {
    flex: 0 0 180px;
    width: 180px;
    border-right: 1px solid var(--border);
    padding: 6px 0;
    overflow-y: auto;
  }
  .item {
    display: flex;
    align-items: center;
    gap: 10px;
    width: 100%;
    height: auto;
    padding: 8px 12px;
    background: none;
    border: none;
    border-left: 3px solid transparent;
    color: var(--text-dim);
    font: inherit;
    font-size: 13px;
    text-align: left;
    cursor: pointer;
  }
  .item:hover {
    color: var(--text);
  }
  .item.active {
    background: var(--bg-sunken);
    color: var(--text);
    border-left-color: var(--accent);
  }
  .item.active :global(svg) {
    color: var(--accent);
  }
  .pane {
    flex: 1;
    min-width: 0;
    padding: 6px 16px 14px;
    overflow: auto;
  }
  .modal-foot {
    display: flex;
    justify-content: flex-end;
    gap: 6px;
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
    color: var(--color-accent-contrast);
  }
  select {
    background: var(--bg-sunken);
    border: 1px solid var(--border);
    padding: 7px 10px;
    color: var(--text);
    font: inherit;
    width: 100%;
    max-width: 320px;
  }
  select:focus {
    outline: none;
    border-color: var(--accent);
  }
  .icon {
    background: none;
    border: none;
    color: var(--text-dim);
    cursor: pointer;
    padding: 2px 4px;
    font-size: 14px;
    line-height: 1;
  }
  .icon:hover {
    color: var(--text);
    background: var(--bg-sunken);
  }
  .btn {
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
    color: var(--color-accent-contrast);
  }
  .btn.ghost {
    background: none;
  }
  .dim {
    color: var(--text-dim);
    margin: 0;
  }
  .hint {
    font-size: 12px;
    margin-top: 8px;
  }
  .error {
    color: var(--off, #d65a5a);
    margin: 6px 0 0;
    font-size: 12px;
  }
</style>
