<script lang="ts">
  import { obs, type Stats, type OutputStat, type ViewerCounts, type ChatPlatform } from "../bridge";
  import PageHeader from "../PageHeader.svelte";
  import EmptyState from "../EmptyState.svelte";
  import { PLATFORM_COLORS, PLATFORM_LABELS, PLATFORM_ORDER } from "../theme/platformColors";

  // Live performance view. stats.get has no push, so this page owns a 1s poll; it
  // unmounts when off-page (App renders it conditionally), which clears the timer.
  let stats = $state<Stats | null>(null);

  function loadStats(): void {
    obs
      .call("stats.get")
      .then((s) => (stats = s))
      .catch(() => {});
  }

  $effect(() => {
    loadStats();
    const timer = setInterval(loadStats, 1000);
    return () => clearInterval(timer);
  });

  const STATE_COLOR: Record<OutputStat["state"], string> = {
    Idle: "var(--color-muted)",
    Connecting: "var(--meter-yellow)",
    Live: "var(--meter-green)",
    Error: "var(--color-live)",
  };

  // Six summary cards, derived from stats.general. value/unit/color per the mock.
  let cards = $derived.by<{ k: string; v: string; u: string; c: string }[]>(() => {
    if (!stats) {
      const empty = { k: "", v: "—", u: "", c: "var(--color-text)" };
      return [
        { ...empty, k: "CPU", u: "% utilization" },
        { ...empty, k: "MEMORY", u: "resident" },
        { ...empty, k: "FPS", u: "target", c: "var(--meter-green)" },
        { ...empty, k: "FRAME TIME", u: "ms average" },
        { ...empty, k: "RENDER LAG", u: "% skipped", c: "var(--meter-green)" },
        { ...empty, k: "ENCODE LAG", u: "% skipped", c: "var(--meter-green)" },
      ];
    }
    const g = stats.general;
    const memGb = g.memoryMB >= 1024;
    return [
      { k: "CPU", v: g.cpu.toFixed(1), u: "% utilization", c: "var(--color-text)" },
      {
        k: "MEMORY",
        v: memGb ? (g.memoryMB / 1024).toFixed(1) : String(Math.round(g.memoryMB)),
        u: memGb ? "GB resident" : "MB resident",
        c: "var(--color-text)",
      },
      { k: "FPS", v: g.fps.toFixed(1), u: "target", c: "var(--meter-green)" },
      { k: "FRAME TIME", v: g.avgFrameMs.toFixed(1), u: "ms average", c: "var(--color-text)" },
      {
        k: "RENDER LAG",
        v: g.renderLagPct.toFixed(1),
        u: "% skipped",
        c: g.renderLagPct > 1 ? "var(--meter-yellow)" : "var(--meter-green)",
      },
      {
        k: "ENCODE LAG",
        v: g.encodeSkipPct.toFixed(1),
        u: "% skipped",
        c: g.encodeSkipPct > 1 ? "var(--meter-yellow)" : "var(--meter-green)",
      },
    ];
  });

  function fmtDuration(ms: number): string {
    const total = Math.floor(ms / 1000);
    const h = Math.floor(total / 3600);
    const m = Math.floor((total % 3600) / 60);
    const s = total % 60;
    const pad = (n: number): string => String(n).padStart(2, "0");
    return h > 0 ? `${h}:${pad(m)}:${pad(s)}` : `${m}:${pad(s)}`;
  }

  // Aggregate viewer count (Phase 9.0), pushed via viewers.changed while live.
  let viewers = $state<ViewerCounts | null>(null);
  // viewers.changed only pushes while live and never sends a final zero, so clear
  // on stream-stop; otherwise the TOTAL + per-platform cards (live-only) keep the
  // last counts until navigation. Mirrors StudioPage's streaming.changed clear.
  $effect(() => {
    const offViewers = obs.on("viewers.changed", (p) => (viewers = p));
    const offStreaming = obs.on("streaming.changed", (s) => {
      if (!s.active) viewers = null;
    });
    return () => {
      offViewers();
      offStreaming();
    };
  });

  // One per-platform card for each platform reporting a count (stable order).
  let viewerCards = $derived.by<{ p: ChatPlatform; label: string; color: string; v: number }[]>(() => {
    const per = viewers?.perPlatform ?? {};
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
        <div class="metric">
          <span class="metric-k">{c.k}</span>
          <span class="metric-v" style:color={c.c}>{c.v}</span>
          <span class="metric-u">{c.u}</span>
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
              <span class="out-dot" style:background={STATE_COLOR[o.state]}></span>
              <span class="out-name">{o.profileLabel} &nbsp;→&nbsp; {o.canvasName}</span>
            </span>
            <span style:color={STATE_COLOR[o.state]}>{o.state}</span>
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
    display: flex;
    flex-direction: column;
    gap: 6px;
    padding: 16px;
    background: var(--color-surface);
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
  }
  .metric-u {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-dim);
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
