<script lang="ts">
  import { untrack } from "svelte";
  import Icon from "../dock/Icon.svelte";
  import EmptyState from "../EmptyState.svelte";
  import { OUTPUT_STATE_COLOR } from "../theme/stateColors";
  import { fmtBitrate, fmtDuration } from "../format";
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
  } from "../statsMeter";
  import { statsStore } from "../statsStore.svelte";
  import type { GeneralStats } from "../bridge";

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
        <ul class="list">
          {#each outputs as o (o.bindingUuid)}
            {@const color = OUTPUT_STATE_COLOR[o.state]}
            <li class="row" style:--dot={color}>
              <span class="dot" style:background={color} title={o.state}></span>
              <div class="info">
                <div class="line1">
                  <span class="name">{o.profileLabel}</span>
                  <span class="arrow"><Icon name="caret-right" size={10} /></span>
                  <span class="canvas">{o.canvasName}</span>
                  <span class="state" style:color>{o.state}</span>
                </div>
                <div class="line2">
                  <span class="stat">{fmtBitrate(o.bitrateKbps)}</span>
                  <span class="stat" class:warn={o.dropPct > 0}>
                    drop {o.droppedFrames}
                    <span class="q">({o.dropPct.toFixed(1)}%)</span>
                  </span>
                  <span class="stat">cong {o.congestionPct.toFixed(1)}%</span>
                  <span class="stat">{fmtDuration(o.durationMs)}</span>
                </div>
              </div>
            </li>
          {/each}
        </ul>
      {/if}
    </section>
  {/if}
</div>

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
    display: flex;
    align-items: stretch;
    gap: 8px;
    padding: 7px 9px 7px 8px;
    border: var(--border-weight) solid var(--color-border);
    border-left: 2px solid var(--dot);
    background: var(--color-base);
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
  .stat.warn {
    color: var(--meter-yellow);
  }
</style>
