<script lang="ts">
  import { untrack } from "svelte";
  import { obs, type ViewerCounts, type ChatPlatform } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
  import PageHeader from "$lib/ui/PageHeader.svelte";
  import EmptyState from "$lib/ui/EmptyState.svelte";
  import { PLATFORM_COLORS, PLATFORM_LABELS, PLATFORM_ORDER } from "$lib/theme/platformColors";
  import { OUTPUT_STATE_COLOR } from "$lib/theme/stateColors";
  import { fmtDuration } from "$lib/utils/format";
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

  // Live performance view. stats.get has no push; the shared 1 Hz store owns the
  // interval. This page subscribes while mounted (App renders it conditionally, so
  // leaving the page unsubscribes and, if no other consumer remains, stops the poll).
  const stats = $derived(statsStore.stats);
  $effect(() => statsStore.subscribe());

  // Short client-side ring per metric so each card can draw a sparkline. Observes the
  // derived snapshot only — no store/poll/bridge change. Reads of `hist` are untracked
  // so the append can't feed back into the effect's dependency set.
  const HIST = 60;
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

  interface Card {
    k: string;
    v: string;
    u: string;
    c: string;
    series: number[];
    domain?: [number, number];
  }

  // Six summary cards, derived from stats.general. value/unit/color per the mock.
  let cards = $derived.by<Card[]>(() => {
    if (!stats) {
      const e = { v: "—", c: METER_TEXT, series: [] as number[] };
      return [
        { ...e, k: "CPU", u: "% utilization" },
        { ...e, k: "MEMORY", u: "resident" },
        { ...e, k: "FPS", u: "target" },
        { ...e, k: "FRAME TIME", u: "ms average" },
        { ...e, k: "RENDER LAG", u: "% skipped" },
        { ...e, k: "ENCODE LAG", u: "% skipped" },
      ];
    }
    const g = stats.general;
    const mem = fmtMem(g.memoryMB);
    return [
      { k: "CPU", v: g.cpu.toFixed(1), u: "% utilization", c: elevated(g.cpu, 60, 85), series: hist.cpu, domain: [0, 100] },
      {
        k: "MEMORY",
        v: mem.v,
        u: mem.gb ? "GB resident" : "MB resident",
        c: METER_TEXT,
        series: hist.mem,
      },
      { k: "FPS", v: fmtNum(g.fps, 2), u: "target", c: METER_GREEN, series: hist.fps },
      { k: "FRAME TIME", v: g.avgFrameMs.toFixed(1), u: "ms average", c: elevated(g.avgFrameMs, 20, 40), series: hist.frame },
      {
        k: "RENDER LAG",
        v: g.renderLagPct.toFixed(1),
        u: `% skipped · ${g.renderLagged}/${g.renderTotal}`,
        c: grade(g.renderLagPct, 1, 5),
        series: hist.render,
      },
      {
        k: "ENCODE LAG",
        v: g.encodeSkipPct.toFixed(1),
        u: `% skipped · ${g.encodeSkipped}/${g.encodeTotal}`,
        c: grade(g.encodeSkipPct, 1, 5),
        series: hist.encode,
      },
    ];
  });

  // Sparkline geometry — fixed 100×26 viewBox stretched to the card width.
  const SW = 100;
  const SH = 26;

  // Aggregate viewer count (Phase 9.0), pushed via viewers.changed while live.
  let viewers = $state<ViewerCounts | null>(null);
  // viewers.changed only pushes while live and never sends a final zero, so clear
  // on stream-stop; otherwise the TOTAL + per-platform cards (live-only) keep the
  // last counts until navigation. Mirrors StudioPage's streaming.changed clear.
  $effect(() => {
    const offViewers = obs.on(EV.viewersChanged, (p) => (viewers = p));
    const offStreaming = obs.on(EV.streamingChanged, (s) => {
      if (!s.active) viewers = null;
    });
    return () => {
      offViewers();
      offStreaming();
    };
  });

  // One per-platform card for each platform reporting a count (stable order). The host
  // emits per accountId ("providerId:userId"); sum by providerId so two accounts on one
  // platform add into a single card.
  let viewerCards = $derived.by<{ p: ChatPlatform; label: string; color: string; v: number }[]>(() => {
    const per: Partial<Record<ChatPlatform, number>> = {};
    for (const [accountId, n] of Object.entries(viewers?.perAccount ?? {})) {
      const providerId = accountId.split(":")[0] as ChatPlatform;
      per[providerId] = (per[providerId] ?? 0) + n;
    }
    return (PLATFORM_ORDER as readonly ChatPlatform[])
      .filter((p) => per[p] !== undefined)
      .map((p) => ({ p, label: PLATFORM_LABELS[p], color: PLATFORM_COLORS[p], v: per[p] ?? 0 }));
  });
</script>

<div class="page">
  <PageHeader title="Monitor" sub="live performance · polled 1×/s" />

  <div class="body">
    <div class="cards">
      {#each cards as c (c.k)}
        {@const pts = sparkPoints(c.series, c.domain, SW, SH)}
        <div class="metric" style:--sev={c.c}>
          <span class="metric-k">{c.k}</span>
          <span class="metric-v" style:color={c.c}>{c.v}</span>
          <span class="metric-u">{c.u}</span>
          <svg class="spark" viewBox="0 0 {SW} {SH}" preserveAspectRatio="none" aria-hidden="true">
            {#if pts}
              <polygon class="spark-fill" points={sparkArea(pts, SW, SH)} style:fill={c.c} />
              <polyline class="spark-line" points={pts} style:stroke={c.c} />
            {:else}
              <line class="spark-flat" x1="0" y1={SH - 1} x2={SW} y2={SH - 1} />
            {/if}
          </svg>
        </div>
      {/each}
    </div>

    <h2 class="section-title">AGGREGATE VIEWERS</h2>

    <div class="vcards">
      <div class="metric">
        <span class="metric-k">TOTAL</span>
        <span class="metric-v">{viewers ? viewers.total.toLocaleString() : "—"}</span>
        <span class="metric-u">concurrent · live only</span>
      </div>
      {#each viewerCards as c (c.p)}
        <div class="metric">
          <span class="metric-k">{c.label}</span>
          <span class="metric-v" style:color={c.color}>{c.v.toLocaleString()}</span>
          <span class="metric-u">viewers</span>
        </div>
      {/each}
    </div>

    <h2 class="section-title">PER-OUTPUT STREAMS</h2>

    <div class="table">
      <div class="thead">
        <span>OUTPUT</span>
        <span>STATE</span>
        <span>BITRATE</span>
        <span>DROPPED</span>
        <span>CONGESTION</span>
        <span>DURATION</span>
      </div>
      {#if !stats || stats.outputs.length === 0}
        <div class="table-empty">
          <EmptyState title="No outputs configured" sub="Bind a destination to a canvas to see per-output stats." />
        </div>
      {:else}
        {#each stats.outputs as o (o.bindingUuid)}
          {@const live = o.state === "Live"}
          <div class="trow">
            <span class="out">
              <span class="out-dot" style:background={OUTPUT_STATE_COLOR[o.state]}></span>
              <span class="out-name">{o.profileLabel} &nbsp;→&nbsp; {o.canvasName}</span>
            </span>
            <span style:color={OUTPUT_STATE_COLOR[o.state]}>{o.state}</span>
            <span>{live ? (o.bitrateKbps / 1000).toFixed(1) + " Mb/s" : "—"}</span>
            <span>{live ? String(o.droppedFrames) : "—"}</span>
            <span>{live ? o.congestionPct.toFixed(1) + "%" : "—"}</span>
            <span>{live ? fmtDuration(o.durationMs) : "—"}</span>
          </div>
        {/each}
      {/if}
    </div>
  </div>
</div>

<style>
  .page {
    height: 100%;
    display: flex;
    flex-direction: column;
    background: var(--color-base);
    color: var(--color-text);
  }
  .body {
    flex: 1;
    min-height: 0;
    overflow: auto;
    padding: 22px 24px;
  }

  /* 1px gap over a border-colored background paints hairline separators between
     the cards (each card repaints its own surface on top). */
  .cards {
    display: grid;
    grid-template-columns: repeat(6, 1fr);
    gap: 1px;
    background: var(--color-border);
    border: 1px solid var(--color-border);
  }
  /* Viewer cards: same hairline grid as .cards but auto-fit so the TOTAL + only
     the reporting platforms show without forcing six columns. */
  .vcards {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(150px, 1fr));
    gap: 1px;
    background: var(--color-border);
    border: 1px solid var(--color-border);
  }
  .metric {
    position: relative;
    display: flex;
    flex-direction: column;
    gap: 6px;
    padding: 16px 16px 14px;
    background: var(--color-surface);
    overflow: hidden;
  }
  /* Status accent rail — the fastest good/warn/crit read at a glance. */
  .metric::before {
    content: "";
    position: absolute;
    left: 0;
    top: 0;
    bottom: 0;
    width: 2px;
    background: var(--sev, var(--color-border));
    opacity: 0.85;
  }
  .metric-k {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--color-muted);
  }
  .metric-v {
    font-family: var(--font-mono);
    font-size: 24px;
    font-weight: 600;
    line-height: 1;
    font-variant-numeric: tabular-nums;
  }
  .metric-u {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-dim);
    font-variant-numeric: tabular-nums;
  }
  .spark {
    display: block;
    width: 100%;
    height: 24px;
    margin-top: 2px;
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
    opacity: 0.1;
  }
  .spark-flat {
    stroke: var(--color-border);
    stroke-width: 1;
    vector-effect: non-scaling-stroke;
  }

  .section-title {
    margin: 26px 0 10px;
    font-family: var(--font-mono);
    font-size: 10px;
    font-weight: 400;
    letter-spacing: 0.12em;
    text-transform: uppercase;
    color: var(--color-muted);
  }

  .table {
    border: var(--border-weight) solid var(--color-border);
  }
  .thead,
  .trow {
    display: grid;
    grid-template-columns: 1.6fr 1fr 1fr 1fr 1fr 1fr;
    align-items: center;
  }
  .thead {
    padding: 9px 16px;
    background: var(--color-surface-2);
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.06em;
    text-transform: uppercase;
    color: var(--color-muted);
  }
  .trow {
    padding: 11px 16px;
    border-top: var(--border-weight) solid var(--color-border-2);
    background: var(--color-surface);
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-dim);
  }
  .table-empty {
    padding: 22px 16px;
    border-top: var(--border-weight) solid var(--color-border-2);
  }
  .out {
    display: flex;
    align-items: center;
    gap: 9px;
    min-width: 0;
  }
  .out-dot {
    flex: 0 0 auto;
    width: 7px;
    height: 7px;
  }
  .out-name {
    color: var(--color-text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
</style>
