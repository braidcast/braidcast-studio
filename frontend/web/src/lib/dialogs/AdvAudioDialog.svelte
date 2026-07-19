<script lang="ts">
  import Modal from "$lib/ui/Modal.svelte";
  import { obs, type AdvancedAudio, type AudioSource, type AudioMonitoringType } from "$lib/api/bridge";
  import { EV } from "$lib/utils/eventNames";

  interface Props {
    // Optional row the opener targeted (name or uuid) — highlighted on open. The
    // dialog is the multi-source table, so it renders every source regardless.
    source?: string;
    label?: string;
    onClose: () => void;
  }
  let { source, label, onClose }: Props = $props();

  // The native preview overlay is suspended by the opener (openAdvAudio) for this
  // dialog's whole lifetime, so it never occludes the modal.

  // One table row: the mixer source (for name/uuid/hidden) plus its advanced audio.
  type Row = { src: AudioSource; aa: AdvancedAudio };

  let rows = $state<Row[]>([]);
  let loaded = $state(false);
  let error = $state<string | null>(null);

  // Header toggles. `usePct` flips the whole Volume column dB<->percentage; `activeOnly`
  // hides suppressed sources (see below). Both default to OBS's Advanced Audio defaults.
  let usePct = $state(false);
  let activeOnly = $state(true);

  // audio.list already returns only currently-active audio sources (AudioMonitor
  // filters on obs_source_audio_active), and AudioSource carries no `active` flag, so
  // the closest suppression signal is `hidden` (mixer_hidden): "Active only" hides
  // hidden rows, unchecking reveals them.
  const visible = $derived(activeOnly ? rows.filter((r) => !r.src.hidden) : rows);

  const MONITORING: { label: string; value: AudioMonitoringType }[] = [
    { label: "Monitor Off", value: "none" },
    { label: "Monitor Only (mute output)", value: "monitorOnly" },
    { label: "Monitor and Output", value: "monitorAndOutput" },
  ];

  function report(e: unknown) {
    error = (e as Error).message;
  }

  // dB<->linear-multiplier: OBS treats the multiplier as the percent (mul 1.0 = 100%).
  const dbToMul = (db: number) => Math.pow(10, db / 20);
  const mulToDb = (mul: number) => 20 * Math.log10(mul);
  // Percentage view of the underlying volume. null (muted to -inf) reads as 0%.
  const dbToPct = (db: number | null) => (db === null ? 0 : dbToMul(db) * 100);
  // Back to the same underlying value the dB path writes; 0% (or less) collapses to -inf.
  const pctToDb = (pct: number): number | null => (pct <= 0 ? null : mulToDb(pct / 100));

  async function load() {
    try {
      const list = (await obs.call("audio.list")).sources;
      // Hydrate each source's advanced audio in parallel (same pattern as the mixer's
      // loadMonitoring). A source that can't report falls back so its row still renders.
      const hydrated = await Promise.all(
        list.map(async (src): Promise<Row> => {
          try {
            return { src, aa: await obs.call("audio.getAdvanced", { uuid: src.uuid }) };
          } catch {
            return { src, aa: { volumeDb: 0, forceMono: false, balance: 0.5, syncOffsetMs: 0, tracks: [], monitoringType: "none" } };
          }
        }),
      );
      rows = hydrated;
      error = null;
    } catch (e) {
      report(e);
    } finally {
      loaded = true;
    }
  }

  // Reload on the source set changing and on external advanced-audio edits (the mixer
  // or another dialog can mutate the same source).
  $effect(() => {
    void load();
    return obs.on(EV.audioChanged, () => void load());
  });

  // Apply a partial to one row: optimistic local merge, then reconcile from the
  // authoritative returned state. On failure, re-load to discard the optimism.
  async function commit(row: Row, patch: Partial<AdvancedAudio>) {
    row.aa = { ...row.aa, ...patch };
    try {
      row.aa = await obs.call("audio.setAdvanced", { uuid: row.src.uuid, ...patch });
      error = null;
    } catch (e) {
      report(e);
      void load();
    }
  }

  // Numeric input parsing. Empty / non-finite falls back so a half-typed field never
  // commits NaN. `int` rounds to an integer.
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

  function setVolume(row: Row, value: string) {
    if (usePct) {
      void commit(row, { volumeDb: pctToDb(num(value, dbToPct(row.aa.volumeDb))) });
    } else {
      void commit(row, { volumeDb: num(value, row.aa.volumeDb ?? 0) });
    }
  }

  // Value shown in the Volume cell for the current unit. % rounds to 1 decimal.
  function volumeValue(aa: AdvancedAudio): string {
    if (usePct) {
      return String(Math.round(dbToPct(aa.volumeDb) * 10) / 10);
    }
    return aa.volumeDb === null ? "" : String(aa.volumeDb);
  }

  function toggleTrack(row: Row, i: number, on: boolean) {
    const tracks = row.aa.tracks.slice();
    tracks[i] = on;
    void commit(row, { tracks });
  }

  // Balance 0..1 -> readout. 0.5 = center; below = left, above = right.
  function balanceLabel(b: number): string {
    if (Math.abs(b - 0.5) < 0.005) {
      return "C";
    }
    const pct = Math.round(Math.abs(b - 0.5) * 200);
    return (b < 0.5 ? "L " : "R ") + pct;
  }

  function isFocused(row: Row): boolean {
    return source != null && (row.src.uuid === source || row.src.name === source);
  }

  const title = $derived(label ? `Advanced Audio Properties — ${label}` : "Advanced Audio Properties");
</script>

<Modal {title} {onClose} width={920}>
  {#if error}<p class="error">{error}</p>{/if}

  <div class="tools">
    <label class="chk">
      <input type="checkbox" bind:checked={activeOnly} />
      <span>Active only</span>
    </label>
    <label class="chk">
      <input type="checkbox" bind:checked={usePct} />
      <span>%</span>
    </label>
  </div>

  {#if !loaded}
    <p class="dim">Loading…</p>
  {:else if visible.length === 0}
    <p class="dim">No {activeOnly ? "active " : ""}audio sources.</p>
  {:else}
    <div class="scroll">
      <table class="grid">
        <thead>
          <tr>
            <th class="name-col">Name</th>
            <th>Volume ({usePct ? "%" : "dB"})</th>
            <th>Mono</th>
            <th class="bal-col">Balance</th>
            <th>Sync (ms)</th>
            <th>Monitoring</th>
            <th>Tracks</th>
          </tr>
        </thead>
        <tbody>
          {#each visible as row (row.src.uuid)}
            <tr class:focus={isFocused(row)}>
              <td class="name-col" title={row.src.name}>{row.src.name}</td>
              <td>
                <input
                  type="number"
                  step="0.1"
                  min={usePct ? "0" : undefined}
                  placeholder={usePct ? "0" : "-∞"}
                  value={volumeValue(row.aa)}
                  onkeydown={onEnter}
                  onchange={(e) => setVolume(row, e.currentTarget.value)}
                />
              </td>
              <td class="ctr">
                <input
                  type="checkbox"
                  checked={row.aa.forceMono}
                  onchange={(e) => void commit(row, { forceMono: e.currentTarget.checked })}
                />
              </td>
              <td class="bal-col">
                <input
                  type="range"
                  min="0"
                  max="1"
                  step="0.01"
                  value={row.aa.balance}
                  onchange={(e) => void commit(row, { balance: num(e.currentTarget.value, row.aa.balance) })}
                />
                <em class="px">{balanceLabel(row.aa.balance)}</em>
              </td>
              <td>
                <input
                  type="number"
                  value={row.aa.syncOffsetMs}
                  onkeydown={onEnter}
                  onchange={(e) => void commit(row, { syncOffsetMs: num(e.currentTarget.value, row.aa.syncOffsetMs, { int: true }) })}
                />
              </td>
              <td>
                <select
                  value={row.aa.monitoringType}
                  onchange={(e) => void commit(row, { monitoringType: e.currentTarget.value as AudioMonitoringType })}
                >
                  {#each MONITORING as m (m.value)}
                    <option value={m.value}>{m.label}</option>
                  {/each}
                </select>
              </td>
              <td>
                <div class="tracks">
                  {#each row.aa.tracks as on, i (i)}
                    <label class="track">
                      <input type="checkbox" checked={on} onchange={(e) => toggleTrack(row, i, e.currentTarget.checked)} />
                      <span>{i + 1}</span>
                    </label>
                  {/each}
                </div>
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
  {/if}

  {#snippet footer()}
    <button class="btn" onclick={onClose}>Close</button>
  {/snippet}
</Modal>

<style>
  .tools {
    display: flex;
    align-items: center;
    gap: 16px;
    margin-bottom: 12px;
  }
  .chk {
    display: flex;
    align-items: center;
    gap: 6px;
    font-size: 11px;
    color: var(--color-text);
  }
  .chk > span {
    color: var(--color-muted);
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
  }

  .scroll {
    overflow-x: auto;
  }
  .grid {
    width: 100%;
    border-collapse: collapse;
    font-size: 11px;
    color: var(--color-text);
  }
  th,
  td {
    padding: 6px 8px;
    text-align: left;
    vertical-align: middle;
    border-bottom: var(--border-weight) solid var(--color-border);
    white-space: nowrap;
  }
  th {
    font-size: 9px;
    letter-spacing: var(--letter-spacing);
    text-transform: uppercase;
    color: var(--color-accent);
    border-bottom-color: var(--color-border);
  }
  .name-col {
    max-width: 180px;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .bal-col {
    min-width: 120px;
  }
  td.ctr {
    text-align: center;
  }
  tr.focus td {
    background: var(--color-base);
  }

  .px {
    font-style: normal;
    font-size: 10px;
    color: var(--color-muted);
    font-variant-numeric: tabular-nums;
    margin-left: 6px;
  }

  .tracks {
    display: flex;
    gap: 10px;
  }
  .track {
    display: flex;
    align-items: center;
    gap: 3px;
    color: var(--color-muted);
  }

  input[type="number"],
  select {
    width: 100%;
    min-width: 72px;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    padding: 4px 6px;
  }
  select {
    min-width: 150px;
  }
  input[type="number"]:focus,
  select:focus {
    outline: none;
    border-color: var(--color-accent);
  }
  input[type="range"] {
    width: 88px;
    accent-color: var(--color-accent);
    vertical-align: middle;
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
