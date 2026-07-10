<script lang="ts">
  import { obs, type SpeakerLayout, type AudioDevice, type GlobalAudioSlot, type MonitorDevice } from "./bridge";
  import { EV } from "./eventNames";

  // Audio settings, lifted verbatim from the former SettingsModal audio pane.
  // Applies live on change (the page model has no Apply boundary).
  const sampleRates = [44100, 48000];
  const speakerOptions: { label: string; value: SpeakerLayout }[] = [
    { label: "Mono", value: "mono" },
    { label: "Stereo", value: "stereo" },
  ];

  let sampleRate = $state<number>(48000);
  let speakers = $state<SpeakerLayout>("stereo");
  let monitoringDevice = $state<MonitorDevice>({ id: "default", name: "Default" });
  let monitorDevices = $state<MonitorDevice[]>([]);
  const monKnown = $derived(monitorDevices.some((d) => d.id === monitoringDevice.id));

  let globalSlots = $state<GlobalAudioSlot[]>([]);
  let inputDevices = $state<AudioDevice[]>([]);
  let outputDevices = $state<AudioDevice[]>([]);
  let deviceError = $state<string | null>(null);

  let loaded = $state(false);
  let audioError = $state<string | null>(null);

  async function load() {
    try {
      const [a, slots, ins, outs, mons] = await Promise.all([
        obs.call("settings.getAudio"),
        obs.call("audio.getGlobalDevices"),
        obs.call("audio.listDevices", { kind: "input" }),
        obs.call("audio.listDevices", { kind: "output" }),
        obs.call("audio.listMonitorDevices"),
      ]);
      sampleRate = a.sampleRate;
      speakers = a.speakers;
      monitoringDevice = a.monitoringDevice;
      globalSlots = slots;
      inputDevices = ins;
      outputDevices = outs;
      monitorDevices = mons;
    } catch (e) {
      audioError = (e as Error).message;
    } finally {
      loaded = true;
    }
  }

  $effect(() => {
    void load();
    // Keep the open tab in sync with external audio-settings edits (MCP / a second
    // window), mirroring AdvancedTab/GeneralTab. The push carries the full state.
    const off = obs.on(EV.settingsAudioChanged, (a) => {
      sampleRate = a.sampleRate;
      speakers = a.speakers;
      monitoringDevice = a.monitoringDevice;
    });
    return off;
  });

  async function setAudio(patch: { sampleRate?: number; speakers?: SpeakerLayout; monitoringDevice?: MonitorDevice }) {
    audioError = null;
    const next = { sampleRate, speakers, monitoringDevice, ...patch };
    sampleRate = next.sampleRate;
    speakers = next.speakers;
    monitoringDevice = next.monitoringDevice;
    try {
      const applied = await obs.call("settings.setAudio", next);
      sampleRate = applied.sampleRate;
      speakers = applied.speakers;
      monitoringDevice = applied.monitoringDevice;
    } catch (e) {
      audioError = (e as Error).message;
      // The optimistic values above may not match what the backend kept; reload the
      // authoritative state so the controls reflect reality after a failed apply.
      void load();
    }
  }

  function setMonitoringDevice(id: string) {
    const dev = monitorDevices.find((d) => d.id === id);
    if (dev) {
      void setAudio({ monitoringDevice: dev });
    }
  }

  async function setGlobalDevice(channel: number, value: string) {
    deviceError = null;
    const deviceId = value === "" ? null : value;
    try {
      await obs.call("audio.setGlobalDevice", { channel, deviceId });
      globalSlots = globalSlots.map((s) =>
        s.channel === channel ? { ...s, deviceId, active: deviceId !== null } : s,
      );
    } catch (e) {
      deviceError = (e as Error).message;
    }
  }
</script>

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
          <button class="chip" class:active={speakers === o.value} onclick={() => void setAudio({ speakers: o.value })}
            >{o.label}</button
          >
        {/each}
      </div>
    </div>

    <div class="field">
      <span class="flabel">Monitoring Device</span>
      <select value={monitoringDevice.id} onchange={(e) => setMonitoringDevice(e.currentTarget.value)}>
        {#each monitorDevices as d (d.id)}
          <option value={d.id}>{d.name}</option>
        {/each}
        {#if !monKnown}
          <option value={monitoringDevice.id}>{monitoringDevice.name} (unavailable)</option>
        {/if}
      </select>
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

<style>
  .group {
    padding: 12px 0;
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .group:last-child {
    border-bottom: none;
  }
  .group h4 {
    margin: 0 0 10px;
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--color-dim);
  }
  .field {
    margin-bottom: 12px;
  }
  .flabel {
    display: block;
    font-size: 12px;
    color: var(--color-dim);
    margin-bottom: 6px;
  }
  .presets {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
    margin-bottom: 6px;
  }
  .chip {
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
    color: var(--color-dim);
    padding: 4px 11px;
    font: inherit;
    font-size: 12px;
    cursor: pointer;
  }
  .chip:hover {
    color: var(--color-text);
  }
  .chip.active {
    background: var(--color-accent);
    border-color: var(--color-accent);
    color: var(--color-accent-contrast);
  }
  select {
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    padding: 7px 10px;
    color: var(--color-text);
    font: inherit;
    width: 100%;
    max-width: 320px;
  }
  select:focus {
    outline: none;
    border-color: var(--color-accent);
  }
  .dim {
    color: var(--color-muted);
    margin: 0;
  }
  .hint {
    font-size: 12px;
    margin-top: 8px;
  }
  .error {
    color: var(--color-live);
    margin: 6px 0 0;
    font-size: 12px;
  }
</style>
