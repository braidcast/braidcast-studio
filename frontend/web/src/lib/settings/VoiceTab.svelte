<script lang="ts">
  import {
    bridgeErrorText,
    obs,
    type VoiceModelStatus,
    type VoicePayload,
    type VoiceSettingsState,
  } from "$lib/api/bridge";
  import { setSettingsTab } from "$lib/dialogs/settingsOpener.svelte";
  import { voiceStore } from "$lib/stores/voiceStore.svelte";
  import Button from "$lib/ui/Button.svelte";
  import ToggleSwitch from "$lib/ui/ToggleSwitch.svelte";
  import { EV } from "$lib/utils/eventNames";
  import { RequestGuard } from "$lib/utils/requestGuard";
  import { voiceEnableGate, voiceIndicator, voiceModelLabel } from "$lib/voice/voiceStatus";

  // Voice control settings, live-applied like the General tab: each change pushes only
  // its own key through settings.setVoice and reconciles from the full payload it
  // answers. settings.voiceChanged keeps this in sync with any other window, and each
  // download step arrives as one model's voice.model.status.
  const DEFAULTS: VoiceSettingsState = {
    enabled: false,
    model: "",
    logTranscripts: false,
    cueVolume: 0.6,
    sendMode: "countdown",
    countdownSec: 3,
    triggerMode: "ptt",
    wakePhrase: "Braidcast",
    readBack: false,
    language: "en",
  };

  let s = $state<VoiceSettingsState>({ ...DEFAULTS });
  let models = $state<VoiceModelStatus[]>([]);
  let cpu = $state<{ supported: boolean; reason: string }>({ supported: true, reason: "" });
  // Why viewers would hear the cues ("" when they would not); the host decides.
  let cueWarning = $state("");
  let loaded = $state(false);
  let error = $state<string | null>(null);
  const guard = new RequestGuard();
  // What the host last confirmed: a rejected apply rolls back to this, not to the value
  // the control saw (with two applies in flight that one may carry a rejected patch).
  let confirmed: VoiceSettingsState = { ...DEFAULTS };

  function adopt(p: VoicePayload): void {
    s = p.settings;
    confirmed = { ...p.settings };
    models = p.models;
    cpu = p.cpu;
    cueWarning = p.cueWarning;
  }

  $effect(() => {
    let active = true;
    const current = guard.claim();
    obs
      .call("settings.getVoice")
      .then((p) => {
        if (active && current()) adopt(p);
      })
      .catch((e) => {
        if (active) error = bridgeErrorText(e) || "Could not read the voice settings.";
      })
      .finally(() => {
        if (active) loaded = true;
      });
    const offSettings = obs.on(EV.settingsVoiceChanged, (p) => {
      // The host's own account of what it holds outranks every reply still in flight.
      guard.supersede();
      adopt(p);
    });
    const offModels = obs.on(EV.voiceModelStatus, (m) => {
      models = models.map((cur) => (cur.id === m.id ? m : cur));
    });
    return () => {
      active = false;
      offSettings();
      offModels();
    };
  });

  // The live state, for the status line under the switch (why voice is not ready).
  $effect(() => voiceStore.subscribe());
  const status = $derived(voiceIndicator(voiceStore.state));

  async function apply(patch: Partial<VoiceSettingsState>): Promise<void> {
    error = null;
    const current = guard.claim();
    s = { ...s, ...patch };
    try {
      const p = await obs.call("settings.setVoice", patch);
      if (current()) adopt(p);
    } catch (e) {
      error = bridgeErrorText(e) || "The change was not applied.";
      if (current()) s = { ...confirmed };
    }
  }

  async function download(id: string): Promise<void> {
    error = null;
    try {
      const r = await obs.call("voice.model.download", { id });
      models = r.models;
    } catch (e) {
      error = bridgeErrorText(e) || "The download did not start.";
    }
  }

  async function cancelDownload(id: string): Promise<void> {
    try {
      const r = await obs.call("voice.model.cancel", { id });
      models = r.models;
    } catch (e) {
      // Usually the download finished as Cancel was pressed; the next status event says so.
      error = bridgeErrorText(e) || "The download could not be cancelled.";
    }
  }

  const speechModels = $derived(models.filter((m) => m.selectable));
  const selectedKnown = $derived(speechModels.some((m) => m.id === s.model));
  const gate = $derived(voiceEnableGate(s, cpu, models));

  const ENABLE_HINT_ID = "voice-enable-hint";
  const MODEL_ID = "voice-model";
  const LOG_HINT_ID = "voice-log-hint";
  const CUE_VOLUME_ID = "voice-cue-volume";
  const CUE_HINT_ID = "voice-cue-hint";
  const SEND_MODE_ID = "voice-send-mode";
  const COUNTDOWN_ID = "voice-countdown";
  const SEND_HINT_ID = "voice-send-hint";

  // How a dictated chat message goes out, safest first. The host refuses anything else.
  const SEND_MODES: { value: VoiceSettingsState["sendMode"]; label: string }[] = [
    { value: "countdown", label: "Show it, then send it" },
    { value: "say", label: "Wait until I say \u201csend\u201d" },
    { value: "instant", label: "Send it straight away" },
  ];

  function applySendMode(value: string): void {
    const mode = SEND_MODES.find((m) => m.value === value);
    if (mode) {
      void apply({ sendMode: mode.value });
    }
  }

  // A cleared or non-numeric box keeps the last good value; the host clamps to 1-10.
  function applyCountdown(input: HTMLInputElement): void {
    const seconds = input.valueAsNumber;
    if (Number.isFinite(seconds)) {
      void apply({ countdownSec: seconds });
    } else {
      input.value = String(s.countdownSec);
    }
  }
</script>

{#if !loaded}
  <p class="dim">Loading settings…</p>
{:else}
  <section class="group">
    <h4>Voice control</h4>
    {#if !cpu.supported}
      <p class="note status warn" role="status">{cpu.reason}</p>
    {/if}
    <label class="check" class:dis={gate.blocked}>
      <ToggleSwitch
        size="sm"
        checked={s.enabled}
        disabled={gate.blocked}
        ariaDescribedBy={ENABLE_HINT_ID}
        onchange={(v) => void apply({ enabled: v })}
      />
      Enable voice control
    </label>
    <p id={ENABLE_HINT_ID} class="dim note">
      Hold the push-to-talk key and speak a command; your microphone is muted on stream while you hold it. Recognition
      runs on this computer, and nothing you say is sent anywhere.
      {#if gate.blocked && cpu.supported}{gate.why}{/if}
    </p>
    {#if s.enabled && status.visible}
      <p class="note status" class:dim={status.tone !== "warn"} class:warn={status.tone === "warn"} role="status">
        {status.label}{status.detail ? ` — ${status.detail}` : ""}
      </p>
    {/if}
    <div class="actions">
      <Button size="sm" onclick={() => setSettingsTab("hotkeys")}>Set the push-to-talk key…</Button>
    </div>
    <p class="dim note">
      Under Hotkeys, "Voice Control: Push to Talk" has no key until you choose one. "Voice Control: Cancel" drops a
      command in progress and defaults to Escape.
    </p>
  </section>

  <section class="group">
    <h4>Command sounds</h4>
    <div class="field">
      <label class="flabel" for={CUE_VOLUME_ID}>Cue volume</label>
      <div class="slider">
        <!-- Shown as it moves, applied once on release: one settings write per drag. -->
        <input
          id={CUE_VOLUME_ID}
          type="range"
          min="0"
          max="1"
          step="0.05"
          value={s.cueVolume}
          aria-describedby={CUE_HINT_ID}
          oninput={(e) => (s = { ...s, cueVolume: Number(e.currentTarget.value) })}
          onchange={(e) => void apply({ cueVolume: Number(e.currentTarget.value) })}
        />
        <span class="pct">{Math.round(s.cueVolume * 100)}%</span>
      </div>
    </div>
    <p id={CUE_HINT_ID} class="dim note">
      A short sound answers each command, so you can keep your eyes on the game. It plays on your monitoring device
      only, never into the stream. Set it to zero for silence.
    </p>
    {#if cueWarning}
      <p class="note status warn" role="status">{cueWarning}</p>
    {/if}
  </section>

  <section class="group">
    <h4>Chat messages</h4>
    <div class="field">
      <label class="flabel" for={SEND_MODE_ID}>Sending a dictated message</label>
      <select
        id={SEND_MODE_ID}
        value={s.sendMode}
        aria-describedby={SEND_HINT_ID}
        onchange={(e) => applySendMode(e.currentTarget.value)}
      >
        {#each SEND_MODES as m (m.value)}
          <option value={m.value}>{m.label}</option>
        {/each}
      </select>
    </div>
    {#if s.sendMode === "countdown"}
      <div class="field">
        <label class="flabel" for={COUNTDOWN_ID}>Show it for (seconds)</label>
        <input
          id={COUNTDOWN_ID}
          class="num"
          type="number"
          min="1"
          max="10"
          step="1"
          value={s.countdownSec}
          onchange={(e) => applyCountdown(e.currentTarget)}
        />
      </div>
    {/if}
    <p id={SEND_HINT_ID} class="dim note">
      Say "send to chat", "reply to" someone who just chatted, or name a platform, and the message appears above the
      Multichat composer before it goes, so you can cancel or edit it. With push-to-talk, anything that is not a
      command is a message to chat. Sending straight away skips that look, which is quick and unforgiving.
    </p>
  </section>

  <section class="group">
    <h4>Speech model</h4>
    <div class="field">
      <label class="flabel" for={MODEL_ID}>Model</label>
      <select id={MODEL_ID} value={s.model} onchange={(e) => void apply({ model: e.currentTarget.value })}>
        {#if !selectedKnown}
          <!-- A model this build does not offer (a hand edit): shown, but not one to choose. -->
          <option value={s.model} disabled>Not recognized</option>
        {/if}
        {#each speechModels as m (m.id)}
          <option value={m.id}>{m.label}</option>
        {/each}
      </select>
    </div>

    <ul class="models" aria-label="Speech models">
      {#each speechModels as m (m.id)}
        <li class="model">
          <span class="mname">{m.label}</span>
          <span class="mstate" class:warn={m.state === "failed"}>{voiceModelLabel(m)}</span>
          {#if m.state === "downloading"}
            <Button size="xs" onclick={() => void cancelDownload(m.id)}>Cancel</Button>
            <progress class="bar" max={m.bytes} value={m.received} aria-label="{m.label} download progress"></progress>
          {:else if m.state !== "ready"}
            <Button size="xs" onclick={() => void download(m.id)}>{m.state === "failed" ? "Retry" : "Download"}</Button>
          {:else}
            <span></span>
          {/if}
        </li>
      {/each}
    </ul>
    <p class="dim note">
      Models download once from Hugging Face and are checked against a pinned fingerprint before use. The first one
      also fetches a 1 MB voice-activity model.
    </p>
  </section>

  <section class="group">
    <h4>Troubleshooting</h4>
    <label class="check">
      <ToggleSwitch
        size="sm"
        checked={s.logTranscripts}
        ariaDescribedBy={LOG_HINT_ID}
        onchange={(v) => void apply({ logTranscripts: v })}
      />
      Write recognized speech to the log
    </label>
    <p id={LOG_HINT_ID} class="dim note">
      Only while debug logging is on as well (Diagnostics). The session log may be shared when reporting a problem,
      so leave this off otherwise.
    </p>
  </section>

  {#if error}<p class="error" role="alert">{error}</p>{/if}
{/if}

<style>
  /* The General tab's rules, so this tab reads as one more of its neighbours. */
  .group {
    padding: 12px 0;
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .group:last-of-type {
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
  .check {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-bottom: 8px;
    font-size: 13px;
    color: var(--color-text);
    cursor: pointer;
  }
  .check.dis {
    color: var(--color-muted);
    cursor: default;
  }
  .num,
  select {
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    padding: 7px 10px;
    color: var(--color-text);
    font: inherit;
    width: 100%;
    max-width: 320px;
  }
  .num {
    max-width: 120px;
  }
  .num:focus,
  select:focus {
    outline: none;
    border-color: var(--color-accent);
  }
  .dim {
    color: var(--color-muted);
    margin: 0;
  }
  .note {
    font-size: 12px;
    margin-top: 8px;
  }
  .status.warn,
  .mstate.warn {
    color: var(--color-warn);
  }
  .actions {
    margin-top: 10px;
  }
  .slider {
    display: flex;
    align-items: center;
    gap: 10px;
    max-width: 320px;
  }
  .slider input {
    flex: 1;
    min-width: 0;
  }
  .pct {
    flex: 0 0 auto;
    min-width: 36px;
    text-align: right;
    font-size: 12px;
    color: var(--color-muted);
    font-variant-numeric: tabular-nums;
  }
  .models {
    list-style: none;
    margin: 0;
    padding: 0;
    max-width: 480px;
    display: flex;
    flex-direction: column;
    gap: 6px;
  }
  .model {
    display: grid;
    grid-template-columns: 1fr auto auto;
    align-items: center;
    column-gap: 10px;
    row-gap: 4px;
    min-height: 24px;
    font-size: 13px;
    color: var(--color-text);
  }
  .mstate {
    font-size: 12px;
    color: var(--color-muted);
    font-variant-numeric: tabular-nums;
  }
  .bar {
    grid-column: 1 / -1;
    inline-size: 100%;
    block-size: 4px;
    accent-color: var(--color-accent);
  }
  .error {
    color: var(--color-live-text);
    margin: 6px 0 0;
    font-size: 12px;
  }
</style>
