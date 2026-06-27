<script lang="ts">
  import { obs, type AdvancedSettings } from "./bridge";

  // Advanced app settings, live-applied (the page model has no Apply boundary):
  // each control change pushes only its changed key via settings.setAdvanced and
  // reconciles from the returned full state. settings.advancedChanged keeps this in
  // sync with any external edit.
  const priorities: { label: string; value: string }[] = [
    { label: "Normal", value: "normal" },
    { label: "Above Normal", value: "aboveNormal" },
    { label: "High", value: "high" },
  ];

  let s = $state<AdvancedSettings>({
    processPriority: "normal",
    streamDelayEnabled: false,
    streamDelaySec: 0,
    streamDelayPreserve: false,
    reconnectEnabled: true,
    reconnectRetryDelaySec: 2,
    reconnectMaxRetries: 25,
    bindIP: "default",
    newSocketLoop: false,
    lowLatencyMode: false,
    dynamicBitrate: false,
    browserHwAccel: true,
  });

  let loaded = $state(false);
  let error = $state<string | null>(null);

  $effect(() => {
    let active = true;
    obs
      .call("settings.getAdvanced")
      .then((a) => {
        if (active) s = a;
      })
      .catch((e) => {
        if (active) error = (e as Error).message;
      })
      .finally(() => {
        if (active) loaded = true;
      });
    const off = obs.on("settings.advancedChanged", (a) => (s = a));
    return () => {
      active = false;
      off();
    };
  });

  // Optimistic local set, then reconcile from the echoed full state.
  async function apply(patch: Partial<AdvancedSettings>): Promise<void> {
    error = null;
    s = { ...s, ...patch };
    try {
      s = await obs.call("settings.setAdvanced", patch);
    } catch (e) {
      error = (e as Error).message;
    }
  }
</script>

{#if !loaded}
  <p class="dim">Loading settings…</p>
{:else}
  <p class="dim note top">Advanced output settings apply to streams started after the change.</p>

  <section class="group">
    <h4>Process</h4>
    <div class="field">
      <span class="flabel">Process priority</span>
      <select value={s.processPriority} onchange={(e) => void apply({ processPriority: e.currentTarget.value })}>
        {#each priorities as p (p.value)}
          <option value={p.value}>{p.label}</option>
        {/each}
      </select>
    </div>
    <p class="dim note">Applies immediately and on next launch.</p>
  </section>

  <section class="group">
    <h4>Stream Delay</h4>
    <label class="check">
      <input
        type="checkbox"
        checked={s.streamDelayEnabled}
        onchange={(e) => void apply({ streamDelayEnabled: e.currentTarget.checked })}
      />
      Enable stream delay
    </label>
    <div class="field">
      <span class="flabel">Delay (seconds)</span>
      <input
        class="num"
        type="number"
        min="0"
        step="1"
        disabled={!s.streamDelayEnabled}
        value={s.streamDelaySec}
        onchange={(e) =>
          void apply({ streamDelaySec: Number.isFinite(e.currentTarget.valueAsNumber) ? e.currentTarget.valueAsNumber : s.streamDelaySec })}
      />
    </div>
    <label class="check">
      <input
        type="checkbox"
        disabled={!s.streamDelayEnabled}
        checked={s.streamDelayPreserve}
        onchange={(e) => void apply({ streamDelayPreserve: e.currentTarget.checked })}
      />
      Preserve delay on disconnect/reconnect
    </label>
  </section>

  <section class="group">
    <h4>Reconnect</h4>
    <label class="check">
      <input
        type="checkbox"
        checked={s.reconnectEnabled}
        onchange={(e) => void apply({ reconnectEnabled: e.currentTarget.checked })}
      />
      Automatically reconnect
    </label>
    <div class="field">
      <span class="flabel">Retry delay (seconds)</span>
      <input
        class="num"
        type="number"
        min="0"
        step="1"
        disabled={!s.reconnectEnabled}
        value={s.reconnectRetryDelaySec}
        onchange={(e) =>
          void apply({
            reconnectRetryDelaySec: Number.isFinite(e.currentTarget.valueAsNumber) ? e.currentTarget.valueAsNumber : s.reconnectRetryDelaySec,
          })}
      />
    </div>
    <div class="field">
      <span class="flabel">Maximum retries</span>
      <input
        class="num"
        type="number"
        min="0"
        step="1"
        disabled={!s.reconnectEnabled}
        value={s.reconnectMaxRetries}
        onchange={(e) =>
          void apply({
            reconnectMaxRetries: Number.isFinite(e.currentTarget.valueAsNumber) ? e.currentTarget.valueAsNumber : s.reconnectMaxRetries,
          })}
      />
    </div>
  </section>

  <section class="group">
    <h4>Network</h4>
    <div class="field">
      <span class="flabel">Bind to IP</span>
      <input
        type="text"
        placeholder="default"
        value={s.bindIP}
        onchange={(e) => void apply({ bindIP: e.currentTarget.value })}
      />
    </div>
    <label class="check">
      <input
        type="checkbox"
        checked={s.newSocketLoop}
        onchange={(e) => void apply({ newSocketLoop: e.currentTarget.checked })}
      />
      Enable new networking code
    </label>
    <label class="check">
      <input
        type="checkbox"
        checked={s.lowLatencyMode}
        onchange={(e) => void apply({ lowLatencyMode: e.currentTarget.checked })}
      />
      Low-latency mode
    </label>
    <label class="check">
      <input
        type="checkbox"
        checked={s.dynamicBitrate}
        onchange={(e) => void apply({ dynamicBitrate: e.currentTarget.checked })}
      />
      Dynamically change bitrate when dropping frames
    </label>
  </section>

  <section class="group">
    <h4>Browser</h4>
    <label class="check">
      <input
        type="checkbox"
        checked={s.browserHwAccel}
        onchange={(e) => void apply({ browserHwAccel: e.currentTarget.checked })}
      />
      Enable browser source hardware acceleration
    </label>
    <p class="dim note">Takes effect after restart.</p>
  </section>

  {#if error}<p class="error">{error}</p>{/if}
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
  .check {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-bottom: 8px;
    font-size: 13px;
    color: var(--color-text);
    cursor: pointer;
  }
  .check input {
    cursor: pointer;
  }
  .check:has(input:disabled) {
    color: var(--color-muted);
    cursor: default;
  }
  .num,
  input[type="text"],
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
  input[type="text"]:focus,
  select:focus {
    outline: none;
    border-color: var(--color-accent);
  }
  .num:disabled {
    color: var(--color-muted);
  }
  .dim {
    color: var(--color-muted);
    margin: 0;
  }
  .note {
    font-size: 12px;
    margin-top: 8px;
  }
  .note.top {
    margin-top: 0;
    margin-bottom: 4px;
  }
  .error {
    color: var(--color-live);
    margin: 6px 0 0;
    font-size: 12px;
  }
</style>
