<script lang="ts">
  import Modal from "./Modal.svelte";
  import { obs, type AdvancedAudio, type AudioMonitoringType } from "./bridge";

  interface Props {
    source: string;
    label: string;
    onClose: () => void;
  }
  let { source, label, onClose }: Props = $props();

  // The native preview overlay is suspended by the opener (openAdvAudio) for this
  // dialog's whole lifetime, so it never occludes the modal.

  let aa = $state<AdvancedAudio | null>(null);
  let loaded = $state(false);
  let error = $state<string | null>(null);

  const MONITORING: { label: string; value: AudioMonitoringType }[] = [
    { label: "Monitor Off", value: "none" },
    { label: "Monitor Only (mute output)", value: "monitorOnly" },
    { label: "Monitor and Output", value: "monitorAndOutput" },
  ];

  function report(e: unknown) {
    error = (e as Error).message;
  }

  async function load() {
    try {
      aa = await obs.call("audio.getAdvanced", { source });
      error = null;
    } catch (e) {
      report(e);
    } finally {
      loaded = true;
    }
  }

  // (Re)load when the source changes (the dialog is remounted per open, but the
  // source can also change in place if a different row is opened while mounted).
  $effect(() => {
    void source;
    loaded = false;
    void load();
  });

  // Reload on external edits (the source may be edited elsewhere, e.g. the mixer).
  $effect(() => {
    return obs.on("audio.changed", () => void load());
  });

  // Apply a partial: optimistic local merge, then reconcile from the authoritative
  // returned state. On failure, re-load to discard the optimism.
  async function commit(patch: Partial<AdvancedAudio>) {
    if (!aa) {
      return;
    }
    aa = { ...aa, ...patch };
    try {
      aa = await obs.call("audio.setAdvanced", { source, ...patch });
      error = null;
    } catch (e) {
      report(e);
      void load();
    }
  }

  // Numeric input parsing. Empty / non-finite falls back so a half-typed field
  // never commits NaN. `int` rounds to an integer.
  function num(value: string, fallback: number, opts: { int?: boolean } = {}): number {
    let n = Number(value);
    if (!Number.isFinite(n)) {
      n = fallback;
    }
    if (opts.int) {
      n = Math.round(n);
    }
    return n;
  }

  function onEnter(e: KeyboardEvent) {
    if (e.key === "Enter") {
      (e.target as HTMLInputElement).blur();
    }
  }

  function toggleTrack(i: number, on: boolean) {
    if (!aa) {
      return;
    }
    const tracks = aa.tracks.slice();
    tracks[i] = on;
    void commit({ tracks });
  }

  // Balance 0..1 -> readout. 0.5 = center; below = left, above = right.
  function balanceLabel(b: number): string {
    if (Math.abs(b - 0.5) < 0.005) {
      return "C";
    }
    const pct = Math.round(Math.abs(b - 0.5) * 200);
    return (b < 0.5 ? "L " : "R ") + pct;
  }
</script>

<Modal title="Advanced Audio Properties — {label}" {onClose} width={560}>
  {#if error}<p class="error">{error}</p>{/if}

  {#if !loaded}
    <p class="dim">Loading…</p>
  {:else if !aa}
    <p class="dim">No audio properties available.</p>
  {:else}
    <div class="grid">
      <div class="group">
        <div class="group-head">Volume</div>
        <label>
          <span>dB</span>
          <input
            type="number"
            step="0.1"
            placeholder="-∞"
            value={aa.volumeDb ?? ""}
            onkeydown={onEnter}
            onchange={(e) => void commit({ volumeDb: num(e.currentTarget.value, aa!.volumeDb ?? 0) })}
          />
        </label>
      </div>

      <div class="group">
        <div class="group-head">Downmix</div>
        <label>
          <span>Mono</span>
          <input
            type="checkbox"
            checked={aa.forceMono}
            onchange={(e) => void commit({ forceMono: e.currentTarget.checked })}
          />
        </label>
      </div>

      <div class="group span2">
        <div class="group-head">Balance</div>
        <label>
          <span>L / R</span>
          <input
            type="range"
            min="0"
            max="1"
            step="0.01"
            value={aa.balance}
            onchange={(e) => void commit({ balance: num(e.currentTarget.value, aa!.balance) })}
          />
          <em class="px">{balanceLabel(aa.balance)}</em>
        </label>
      </div>

      <div class="group">
        <div class="group-head">Sync Offset</div>
        <label>
          <span>ms</span>
          <input
            type="number"
            value={aa.syncOffsetMs}
            onkeydown={onEnter}
            onchange={(e) => void commit({ syncOffsetMs: num(e.currentTarget.value, aa!.syncOffsetMs, { int: true }) })}
          />
        </label>
      </div>

      <div class="group">
        <div class="group-head">Audio Monitoring</div>
        <label>
          <span>Mode</span>
          <select
            value={aa.monitoringType}
            onchange={(e) => void commit({ monitoringType: e.currentTarget.value as AudioMonitoringType })}
          >
            {#each MONITORING as m (m.value)}
              <option value={m.value}>{m.label}</option>
            {/each}
          </select>
        </label>
      </div>

      <div class="group span2">
        <div class="group-head">Tracks</div>
        <div class="tracks">
          {#each aa.tracks as on, i (i)}
            <label class="track">
              <input type="checkbox" checked={on} onchange={(e) => toggleTrack(i, e.currentTarget.checked)} />
              <span>{i + 1}</span>
            </label>
          {/each}
        </div>
      </div>
    </div>
  {/if}

  {#snippet footer()}
    <button class="btn" onclick={onClose}>Close</button>
  {/snippet}
</Modal>

<style>
  .grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 10px;
  }
  .group {
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
    padding: 8px;
    display: flex;
    flex-direction: column;
    gap: 6px;
  }
  .group.span2 {
    grid-column: 1 / -1;
  }
  .group-head {
    font-size: 9px;
    letter-spacing: var(--letter-spacing);
    text-transform: uppercase;
    color: var(--color-accent);
  }

  label {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 11px;
    color: var(--color-text);
  }
  label > span {
    flex: 0 0 64px;
    color: var(--color-muted);
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
  }

  .px {
    flex: 0 0 auto;
    font-style: normal;
    font-size: 10px;
    color: var(--color-muted);
    font-variant-numeric: tabular-nums;
  }

  .tracks {
    display: flex;
    flex-wrap: wrap;
    gap: 12px;
  }
  .track {
    flex: 0 0 auto;
    gap: 4px;
  }
  .track > span {
    flex: 0 0 auto;
    color: var(--color-text);
  }

  input[type="number"],
  select {
    flex: 1;
    min-width: 0;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    padding: 4px 6px;
  }
  input[type="number"]:focus,
  select:focus {
    outline: none;
    border-color: var(--color-accent);
  }
  input[type="range"] {
    flex: 1;
    min-width: 0;
    accent-color: var(--color-accent);
  }
  input[type="checkbox"] {
    accent-color: var(--color-accent);
  }

  .dim {
    color: var(--color-muted);
    margin: 0;
    padding: 8px;
    font-size: 11px;
  }
  .error {
    color: var(--color-live);
    margin: 0 0 8px;
    font-size: 11px;
  }
</style>
