<script lang="ts">
  import { untrack } from "svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import EmptyState from "$lib/ui/EmptyState.svelte";
  import Modal from "$lib/ui/Modal.svelte";
  import { STATE_COLOR } from "$lib/theme/stateColors";
  import { fmtBitrate, fmtDuration, titleState } from "$lib/utils/format";
  import {
    METER_TEXT,
    METER_GREEN,
    fmtNum,
    fmtMem,
    elevated,
    grade,
    pushRing,
    sparkPoints,
    sparkArea,
  } from "$lib/utils/statsMeter";
  import { statsStore } from "$lib/stores/statsStore.svelte";
  import { multistreamStatusStore } from "$lib/stores/multistreamStatusStore.svelte";
  import type { GeneralStats, OutputStat } from "$lib/api/bridge";

  // Host supplies tab chrome + strips __* keys; this body declares no props.
  let {}: Record<string, unknown> = $props();

  // Shared 1 Hz poll (also feeds the Studio bottom bar + Monitor page).
  const stats = $derived(statsStore.stats);
  const error = $derived(statsStore.error);
  const loaded = $derived(statsStore.stats !== null || statsStore.error !== null);
  $effect(() => statsStore.subscribe());

  // --- client-side rolling history --------------------------------------------
  // The store holds only the latest snapshot; keep a short local ring per metric so
  // the cells can draw a sparkline. This observes the derived snapshot — it does NOT
  // touch the store, poll, or bridge. Reads of `hist` are untracked so appending
  // inside the effect can't feed back into its own dependency set.
  const HIST = 48;
  let hist = $state<Record<"cpu" | "mem" | "fps" | "frame" | "render" | "encode", number[]>>({
    cpu: [],
    mem: [],
    fps: [],
    frame: [],
    render: [],
    encode: [],
  });

  const push = (arr: number[], v: number): void => pushRing(arr, v, HIST);

  $effect(() => {
    const g = statsStore.stats?.general;
    if (!g) {
      return;
    }
    untrack(() => {
      push(hist.cpu, g.cpu);
      push(hist.mem, g.memoryMB);
      push(hist.fps, g.fps);
      push(hist.frame, g.avgFrameMs);
      push(hist.render, g.renderLagPct);
      push(hist.encode, g.encodeSkipPct);
    });
  });

  interface Metric {
    k: string;
    v: string;
    u: string;
    sub?: string;
    color: string;
    series: number[];
    domain?: [number, number];
  }

  const metrics = $derived.by<Metric[]>(() => {
    if (!stats) {
      return [];
    }
    const g: GeneralStats = stats.general;
    const mem = fmtMem(g.memoryMB);
    return [
      { k: "CPU", v: g.cpu.toFixed(1), u: "%", color: elevated(g.cpu, 60, 85), series: hist.cpu, domain: [0, 100] },
      { k: "MEM", v: mem.v, u: mem.gb ? "GB" : "MB", color: METER_TEXT, series: hist.mem },
      { k: "FPS", v: fmtNum(g.fps, 2), u: "fps", color: METER_GREEN, series: hist.fps },
      { k: "FRAME", v: g.avgFrameMs.toFixed(1), u: "ms", color: elevated(g.avgFrameMs, 20, 40), series: hist.frame },
      {
        k: "RENDER LAG",
        v: g.renderLagPct.toFixed(1),
        u: "%",
        sub: `${g.renderLagged}/${g.renderTotal}`,
        color: grade(g.renderLagPct, 1, 5),
        series: hist.render,
      },
      {
        k: "ENCODE SKIP",
        v: g.encodeSkipPct.toFixed(1),
        u: "%",
        sub: `${g.encodeSkipped}/${g.encodeTotal}`,
        color: grade(g.encodeSkipPct, 1, 5),
        series: hist.encode,
      },
    ];
  });

  // --- sparkline geometry -----------------------------------------------------
  // Fixed 100×24 viewBox stretched (preserveAspectRatio="none") to the cell width.
  const SW = 100;
  const SH = 24;

  const outputs = $derived(stats?.outputs ?? []);

  // Severity thresholds (warn, crit) fed to statsMeter grade() for the drop-frame
  // and congestion reads. Drop mirrors the engine's %-health metrics (render lag /
  // encode skip); congestion is a slower-moving network-pressure gauge, so its band
  // sits higher before it reads red.
  const DROP_GRADE: [number, number] = [1, 5];
  const CONG_GRADE: [number, number] = [30, 60];

  // Cumulative read across every output, so trouble is visible before drilling into
  // rows: live/total, error count, summed dropped frames + worst drop%, summed
  // outgoing bitrate, and peak congestion.
  const summary = $derived.by(() => {
    const live = outputs.filter((o) => o.state === "live").length;
    const errors = outputs.filter((o) => o.state === "error").length;
    let droppedFrames = 0;
    let worstDropPct = 0;
    let bitrateKbps = 0;
    let maxCongestionPct = 0;
    for (const o of outputs) {
      droppedFrames += o.droppedFrames;
      bitrateKbps += o.bitrateKbps;
      if (o.dropPct > worstDropPct) worstDropPct = o.dropPct;
      if (o.congestionPct > maxCongestionPct) maxCongestionPct = o.congestionPct;
    }
    return { total: outputs.length, live, errors, droppedFrames, worstDropPct, bitrateKbps, maxCongestionPct };
  });

  // Per-stream error detail lives in the shared status store (statusByBinding carries
  // {state, lastError}); subscribe ref-counted (same pattern as statsStore above) and
  // read it when an errored row is opened. Never refetch here — one source of truth.
  $effect(() => multistreamStatusStore.subscribe());

  let errorRow = $state<OutputStat | null>(null);
  const errorDetail = $derived(
    errorRow
      ? (multistreamStatusStore.statusByBinding.get(errorRow.bindingUuid)?.lastError ?? "").trim() ||
          "Stream error (no detail reported)"
      : "",
  );

  function openError(o: OutputStat): void {
    errorRow = o;
  }
  function onRowKey(e: KeyboardEvent, o: OutputStat): void {
    if (e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      openError(o);
    }
  }
</script>

<div class="dock-body">
  {#if error}
    <p class="dock-msg err">{error}</p>
  {/if}

  {#if !loaded}
    <p class="dock-msg">Loading…</p>
  {:else if stats}
    <section class="block">
      <div class="sec-head">
        <span class="sec-label">Engine</span>
        <span class="sec-meta">1 Hz</span>
      </div>
      <div class="metrics">
        {#each metrics as m (m.k)}
          {@const pts = sparkPoints(m.series, m.domain, SW, SH)}
          <div class="cell" style:--sev={m.color}>
            <div class="cell-head">
              <span class="k">{m.k}</span>
              {#if m.sub}<span class="frac">{m.sub}</span>{/if}
            </div>
            <div class="v-row">
              <span class="v" style:color={m.color}>{m.v}</span>
              <span class="u">{m.u}</span>
            </div>
            <svg class="spark" viewBox="0 0 {SW} {SH}" preserveAspectRatio="none" aria-hidden="true">
              {#if pts}
                <polygon class="spark-fill" points={sparkArea(pts, SW, SH)} style:fill={m.color} />
                <polyline class="spark-line" points={pts} style:stroke={m.color} />
              {:else}
                <line class="spark-flat" x1="0" y1={SH - 1} x2={SW} y2={SH - 1} />
              {/if}
            </svg>
          </div>
        {/each}
      </div>
    </section>

    <section class="block outputs">
      <div class="sec-head">
        <span class="sec-label">Outputs</span>
        <span class="sec-meta">{outputs.length}</span>
      </div>
      {#if outputs.length === 0}
        <div class="empty-wrap">
          <EmptyState
            compact
            title="No outputs configured"
            sub="Bind a destination to a canvas to see live per-output telemetry."
          >
            {#snippet icon()}
              <Icon name="destinations" size={22} />
            {/snippet}
          </EmptyState>
        </div>
      {:else}
        <div class="summary">
          <span class="sm">
            <span class="sm-k">Live</span>
            <span class="sm-v">{summary.live}/{summary.total}</span>
          </span>
          {#if summary.errors > 0}
            <span class="sm err">
              <span class="sm-k">Err</span>
              <span class="sm-v">{summary.errors}</span>
            </span>
          {/if}
          <span class="sm">
            <span class="sm-k">Drop</span>
            <span class="sm-v" style:color={grade(summary.worstDropPct, DROP_GRADE[0], DROP_GRADE[1])}>
              {summary.droppedFrames} · {summary.worstDropPct.toFixed(1)}%
            </span>
          </span>
          <span class="sm">
            <span class="sm-k">Net</span>
            <span class="sm-v">{fmtBitrate(summary.bitrateKbps)}</span>
          </span>
          <span class="sm">
            <span class="sm-k">Cong</span>
            <span class="sm-v" style:color={grade(summary.maxCongestionPct, CONG_GRADE[0], CONG_GRADE[1])}>
              {summary.maxCongestionPct.toFixed(1)}%
            </span>
          </span>
        </div>
        <ul class="list">
          {#each outputs as o (o.bindingUuid)}
            {@const color = STATE_COLOR[o.state]}
            {@const isErr = o.state === "error"}
            <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
            <li
              class="row"
              class:err={isErr}
              style:--dot={color}
              role={isErr ? "button" : undefined}
              tabindex={isErr ? 0 : undefined}
              title={isErr ? "Show stream error detail" : undefined}
              onclick={isErr ? () => openError(o) : undefined}
              onkeydown={isErr ? (e) => onRowKey(e, o) : undefined}
            >
              <span class="dot" style:background={color} title={titleState(o.state)}></span>
              <div class="info">
                <div class="line1">
                  <span class="name">{o.profileLabel}</span>
                  <span class="arrow"><Icon name="caret-right" size={10} /></span>
                  <span class="canvas">{o.canvasName}</span>
                  <span class="state" style:color>{titleState(o.state)}</span>
                </div>
                <div class="line2">
                  <span class="stat">{fmtBitrate(o.bitrateKbps)}</span>
                  <span class="stat" style:color={elevated(o.dropPct, DROP_GRADE[0], DROP_GRADE[1])}>
                    drop {o.droppedFrames}
                    <span class="q">({o.dropPct.toFixed(1)}%)</span>
                  </span>
                  <span class="stat" style:color={elevated(o.congestionPct, CONG_GRADE[0], CONG_GRADE[1])}>
                    cong {o.congestionPct.toFixed(1)}%
                  </span>
                  <span class="stat">{fmtDuration(o.durationMs)}</span>
                  {#if isErr}
                    <div class="err-cover">
                      <Icon name="warn" size={12} />
                      <span>Error</span>
                    </div>
                  {/if}
                </div>
              </div>
            </li>
          {/each}
        </ul>
      {/if}
    </section>
  {/if}
</div>

{#if errorRow}
  <Modal title={"Stream error · " + errorRow.profileLabel} onClose={() => (errorRow = null)} width={440}>
    <div class="err-modal">
      <div class="err-target">{errorRow.profileLabel} → {errorRow.canvasName}</div>
      <p class="err-text">{errorDetail}</p>
    </div>
  </Modal>
{/if}

<style>
  .dock-body {
    display: flex;
    flex-direction: column;
  }

  .block {
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .block.outputs {
    border-bottom: none;
  }

  .sec-head {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    padding: 7px 9px 6px;
    background: var(--color-surface-2);
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .sec-label {
    font-family: var(--font-mono);
    font-size: 9.5px;
    letter-spacing: 0.12em;
    text-transform: uppercase;
    color: var(--color-muted);
  }
  .sec-meta {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.08em;
    color: var(--color-dim);
    font-variant-numeric: tabular-nums;
  }

  /* Hairline grid: 1px gap over a border-colored bg paints separators between
     cells, each cell repaints its own surface. Reflows down to one column. */
  .metrics {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(96px, 1fr));
    gap: var(--border-weight);
    background: var(--color-border);
  }
  .cell {
    position: relative;
    display: flex;
    flex-direction: column;
    gap: 4px;
    padding: 7px 8px 6px;
    background: var(--color-surface);
    overflow: hidden;
  }
  /* Status accent rail — the fastest good/warn/crit read at a glance. */
  .cell::before {
    content: "";
    position: absolute;
    left: 0;
    top: 0;
    bottom: 0;
    width: 2px;
    background: var(--sev);
    opacity: 0.85;
  }
  .cell-head {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    gap: 6px;
    min-width: 0;
  }
  .k {
    font-family: var(--font-mono);
    font-size: 8.5px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--color-muted);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .frac {
    font-family: var(--font-mono);
    font-size: 8.5px;
    color: var(--color-dim);
    font-variant-numeric: tabular-nums;
    flex-shrink: 0;
  }
  .v-row {
    display: flex;
    align-items: baseline;
    gap: 3px;
    min-width: 0;
  }
  .v {
    font-family: var(--font-mono);
    font-size: 18px;
    font-weight: 600;
    line-height: 1;
    font-variant-numeric: tabular-nums;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .u {
    font-family: var(--font-mono);
    font-size: 9px;
    color: var(--color-muted);
    flex-shrink: 0;
  }

  .spark {
    display: block;
    width: 100%;
    height: 16px;
    margin-top: 1px;
    overflow: visible;
  }
  .spark-line {
    fill: none;
    stroke-width: 1.25;
    vector-effect: non-scaling-stroke;
    opacity: 0.9;
  }
  .spark-fill {
    stroke: none;
    opacity: 0.12;
  }
  .spark-flat {
    stroke: var(--color-border);
    stroke-width: 1;
    vector-effect: non-scaling-stroke;
  }

  .empty-wrap {
    padding: 22px 12px;
  }

  .list {
    list-style: none;
    margin: 0;
    padding: 6px;
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .row {
    position: relative;
    display: flex;
    align-items: stretch;
    gap: 8px;
    padding: 7px 9px 7px 8px;
    border: var(--border-weight) solid var(--color-border);
    border-left: 3px solid var(--dot);
    background: color-mix(in srgb, var(--dot) 7%, var(--color-base));
  }
  /* Errored rows are clickable (open the detail modal); the stronger border + hover
     read as interactive without competing with the line2 overlay's loud signal. */
  .row.err {
    cursor: pointer;
    border-color: color-mix(in srgb, var(--color-live) 55%, var(--color-border));
  }
  .row.err:hover {
    background: color-mix(in srgb, var(--color-live) 14%, var(--color-base));
  }
  .row.err:focus-visible {
    outline: var(--border-weight) solid var(--color-live);
    outline-offset: 1px;
  }
  .dot {
    width: 7px;
    height: 7px;
    border-radius: 50%;
    flex-shrink: 0;
    margin-top: 3px;
  }
  .info {
    min-width: 0;
    flex: 1;
  }
  .line1 {
    display: flex;
    align-items: center;
    gap: 6px;
    font-size: 11px;
    min-width: 0;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
  }
  .name {
    color: var(--color-text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .arrow {
    display: inline-flex;
    align-items: center;
    color: var(--color-muted);
    flex-shrink: 0;
  }
  .canvas {
    color: var(--color-muted);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .state {
    margin-left: auto;
    font-family: var(--font-mono);
    font-size: 8.5px;
    letter-spacing: 0.08em;
    flex-shrink: 0;
  }
  .line2 {
    position: relative;
    display: flex;
    flex-wrap: wrap;
    gap: 4px 10px;
    margin-top: 4px;
    font-size: 10px;
    color: var(--color-dim);
    font-variant-numeric: tabular-nums;
  }
  .stat {
    font-family: var(--font-mono);
    font-variant-numeric: tabular-nums;
    white-space: nowrap;
  }
  .stat .q {
    color: var(--color-muted);
  }
  /* Red-tinted, blurred cover over the (meaningless) error-state numbers; line1
     stays visible so the user still sees which stream failed. */
  .err-cover {
    position: absolute;
    inset: 0;
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 0 4px;
    color: var(--color-live);
    background: color-mix(in srgb, var(--color-live) 22%, transparent);
    backdrop-filter: blur(3px);
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
  }

  .summary {
    display: flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: 4px 12px;
    padding: 6px 9px;
    background: var(--color-surface);
    border-bottom: var(--border-weight) solid var(--color-border);
    font-family: var(--font-mono);
    font-variant-numeric: tabular-nums;
  }
  .sm {
    display: flex;
    align-items: baseline;
    gap: 5px;
  }
  .sm-k {
    font-size: 8.5px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--color-muted);
  }
  .sm-v {
    font-size: 10px;
    color: var(--color-dim);
  }
  .sm.err .sm-v {
    color: var(--meter-red);
  }

  .err-modal {
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .err-target {
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.06em;
    color: var(--color-muted);
  }
  .err-text {
    margin: 0;
    padding: 10px 11px;
    font-size: 12px;
    line-height: 1.5;
    color: var(--color-text);
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    border-left: 2px solid var(--color-live);
    white-space: pre-wrap;
    word-break: break-word;
  }
</style>
