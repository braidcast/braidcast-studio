<script lang="ts">
  import {
    obs,
    type CanvasInfo,
    type MultistreamStatus,
    type MultistreamState,
    type Stats,
    type OutputStat,
  } from "../bridge";
  import { openSettings } from "../settingsOpener.svelte";

  // All-destinations view: encode-once per canvas, fan out to every enabled output
  // bound to it. Rows join the multistream status (authoritative live state, pushed)
  // with the polled per-output stats (bitrate/drops/congestion/duration).
  let canvases = $state<CanvasInfo[]>([]);
  let outputs = $state<MultistreamStatus[]>([]);
  let stats = $state<Stats | null>(null);
  let busy = $state(false);

  const STATE_COLOR: Record<MultistreamState, string> = {
    idle: "var(--color-muted)",
    connecting: "var(--meter-yellow)",
    live: "var(--meter-green)",
    error: "var(--color-live)",
  };

  function loadStatus(): void {
    obs
      .call("multistream.status")
      .then((res) => (outputs = res.outputs))
      .catch(() => {});
  }
  function loadCanvases(): void {
    obs
      .call("canvas.list")
      .then((list) => (canvases = list))
      .catch(() => {});
  }
  function loadStats(): void {
    obs
      .call("stats.get")
      .then((s) => (stats = s))
      .catch(() => {});
  }

  $effect(() => {
    loadStatus();
    loadCanvases();
    const offMulti = obs.on("multistream.changed", (p) => (outputs = p.outputs));
    const offBindings = obs.on("outputBinding.changed", loadStatus);
    const offCanvas = obs.on("canvas.changed", loadCanvases);
    return () => {
      offMulti();
      offBindings();
      offCanvas();
    };
  });

  // stats.get has no push; the page owns a 1s poll (cleared on unmount, which is the
  // page's lifecycle since App conditionally renders it).
  $effect(() => {
    loadStats();
    const timer = setInterval(loadStats, 1000);
    return () => clearInterval(timer);
  });

  // bindingUuid -> polled per-output stats row.
  let statByBinding = $derived.by<Map<string, OutputStat>>(() => {
    const m = new Map<string, OutputStat>();
    if (stats) {
      for (const o of stats.outputs) {
        m.set(o.bindingUuid, o);
      }
    }
    return m;
  });

  // One section per canvas (canvas.list order); a canvas with no enabled bindings is
  // omitted (multistream.status only carries enabled bindings).
  let groups = $derived.by<{ canvas: CanvasInfo; bindings: MultistreamStatus[] }[]>(() =>
    canvases
      .map((c) => ({ canvas: c, bindings: outputs.filter((o) => o.canvasUuid === c.uuid) }))
      .filter((g) => g.bindings.length > 0),
  );

  function isRunning(state: MultistreamState): boolean {
    return state === "live" || state === "connecting";
  }
  let anyRunning = $derived(outputs.some((o) => isRunning(o.state)));

  // Aggregate header trio, summed over the live per-output stats rows.
  let liveStatRows = $derived(stats ? stats.outputs.filter((o) => o.state === "Live") : []);
  let aggBitrate = $derived((liveStatRows.reduce((s, o) => s + o.bitrateKbps, 0) / 1000).toFixed(1) + " Mb/s");
  let aggDrop = $derived(String(liveStatRows.reduce((s, o) => s + o.droppedFrames, 0)));
  let aggUptime = $derived.by<string>(() => {
    if (liveStatRows.length === 0) {
      return "—";
    }
    let max = 0;
    for (const o of liveStatRows) {
      if (o.durationMs > max) {
        max = o.durationMs;
      }
    }
    return fmtDuration(max);
  });

  function fmtDuration(ms: number): string {
    const s = Math.floor(ms / 1000);
    const pad = (n: number): string => String(n).padStart(2, "0");
    return pad(Math.floor(s / 3600)) + ":" + pad(Math.floor((s % 3600) / 60)) + ":" + pad(s % 60);
  }

  function titleState(s: MultistreamState): string {
    return s.charAt(0).toUpperCase() + s.slice(1);
  }

  // Two-letter mark from the profile label: initials of the first two words, else
  // the first two characters.
  function initials(label: string): string {
    const words = label.trim().split(/\s+/).filter(Boolean);
    if (words.length >= 2) {
      return (words[0][0] + words[1][0]).toUpperCase();
    }
    return label.trim().slice(0, 2).toUpperCase();
  }

  function health(congestionPct: number): number {
    return Math.round(Math.max(0, Math.min(100, 100 - congestionPct)));
  }
  function healthColor(h: number): string {
    return h >= 90 ? "var(--meter-green)" : h >= 70 ? "var(--meter-yellow)" : "var(--color-live)";
  }

  // No stream URL is exposed by the bridge; the secondary line carries a live data
  // hint instead of the (unavailable) RTMP URL the mock shows.
  function secondary(o: MultistreamStatus, stat: OutputStat | undefined): string {
    if (o.state === "live" && stat) {
      return (stat.bitrateKbps / 1000).toFixed(1) + " Mb/s · " + stat.droppedFrames + " dropped";
    }
    if (o.state === "error") {
      return o.lastError || "error";
    }
    return "ready to go live";
  }

  // Section accent: any error -> red, else any live -> green, else muted.
  function sectionTone(bindings: MultistreamStatus[]): "error" | "live" | "idle" {
    if (bindings.some((o) => o.state === "error")) {
      return "error";
    }
    if (bindings.some((o) => o.state === "live")) {
      return "live";
    }
    return "idle";
  }
  const TONE_DOT: Record<"error" | "live" | "idle", string> = {
    error: "var(--color-live)",
    live: "var(--meter-green)",
    idle: "var(--color-muted)",
  };
  const TONE_TAG_BG: Record<"error" | "live" | "idle", string> = {
    error: "color-mix(in srgb, var(--color-live) 14%, transparent)",
    live: "color-mix(in srgb, var(--meter-green) 14%, transparent)",
    idle: "color-mix(in srgb, var(--color-muted) 12%, transparent)",
  };
  // Per-row status pill background, tinted off the same per-state colors as
  // STATE_COLOR so the chip follows the active theme.
  const STATE_TAG_BG: Record<MultistreamState, string> = {
    idle: "color-mix(in srgb, var(--color-muted) 12%, transparent)",
    connecting: "color-mix(in srgb, var(--meter-yellow) 14%, transparent)",
    live: "color-mix(in srgb, var(--meter-green) 14%, transparent)",
    error: "color-mix(in srgb, var(--color-live) 14%, transparent)",
  };

  async function toggleRow(o: MultistreamStatus): Promise<void> {
    try {
      if (isRunning(o.state)) {
        await obs.call("multistream.stopOutput", { uuid: o.bindingUuid });
      } else {
        await obs.call("multistream.startOutput", { uuid: o.bindingUuid });
      }
    } catch {
      // Authoritative state arrives via multistream.changed.
    }
  }

  // GO LIVE / END STREAM drives every binding across all canvases. No start-all
  // bridge method, so loop; per-call errors are ignored (state arrives via push).
  async function toggleAll(): Promise<void> {
    if (busy || outputs.length === 0) {
      return;
    }
    busy = true;
    try {
      const stopping = anyRunning;
      for (const o of outputs) {
        if (stopping) {
          if (isRunning(o.state)) {
            await obs.call("multistream.stopOutput", { uuid: o.bindingUuid }).catch(() => {});
          }
        } else {
          await obs.call("multistream.startOutput", { uuid: o.bindingUuid }).catch(() => {});
        }
      }
    } finally {
      busy = false;
    }
  }
</script>

<div class="page">
  <header class="head">
    <div class="head-left">
      <span class="title">Destinations</span>
      <span class="sub">encode once · fan out to every platform</span>
    </div>
    <div class="head-right">
      <div class="agg">
        <span class="agg-item"><span class="agg-k">OUT</span><span class="agg-v">{aggBitrate}</span></span>
        <span class="agg-item"><span class="agg-k">DROP</span><span class="agg-v">{aggDrop}</span></span>
        <span class="agg-item"><span class="agg-k">UPTIME</span><span class="agg-v">{aggUptime}</span></span>
      </div>
      <button
        class="golive"
        class:running={anyRunning}
        disabled={busy || outputs.length === 0}
        onclick={() => void toggleAll()}
      >
        {anyRunning ? "■  END STREAM" : "●  GO LIVE"}
      </button>
    </div>
  </header>

  <div class="body">
    {#if groups.length === 0}
      <p class="empty">No destinations configured.</p>
    {/if}

    {#each groups as g (g.canvas.uuid)}
      {@const tone = sectionTone(g.bindings)}
      {@const liveCount = g.bindings.filter((o) => o.state === "live").length}
      <section class="card">
        <div class="card-head">
          <span class="card-dot" style:background={TONE_DOT[tone]}></span>
          <span class="card-name">{g.canvas.name}</span>
          <span class="card-meta">
            {g.canvas.outputWidth}×{g.canvas.outputHeight} · {Math.round(g.canvas.fpsNum / g.canvas.fpsDen)} fps · {g
              .canvas.videoEncoder || "x264"}
          </span>
          <span class="card-spacer"></span>
          <span class="card-tag" style:color={TONE_DOT[tone]} style:background={TONE_TAG_BG[tone]}>
            {liveCount} / {g.bindings.length} LIVE
          </span>
        </div>

        <div class="dest-grid">
          {#each g.bindings as o (o.bindingUuid)}
            {@const stat = statByBinding.get(o.bindingUuid)}
            {@const live = o.state === "live"}
            {@const h = live && stat ? health(stat.congestionPct) : 0}
            <div class="dest-tile">
              <div class="dest-head">
                <span class="mark">{initials(o.profileLabel)}</span>
                <div class="dest-col">
                  <div class="dest-line1">
                    <span class="dest-name">{o.profileLabel}</span>
                    <span
                      class="dest-state"
                      style:color={STATE_COLOR[o.state]}
                      style:background={STATE_TAG_BG[o.state]}>{titleState(o.state).toUpperCase()}</span
                    >
                  </div>
                  <div class="dest-sub">{secondary(o, stat)}</div>
                </div>
              </div>

              <div class="stats-group">
                <div class="stat-cell">
                  <span class="stat-k">BITRATE</span>
                  <span class="stat-v">{live && stat ? (stat.bitrateKbps / 1000).toFixed(1) + " Mb/s" : "—"}</span>
                </div>
                <div class="stat-cell">
                  <span class="stat-k">DROPPED</span>
                  <span class="stat-v">
                    {live && stat ? stat.droppedFrames + " (" + stat.dropPct.toFixed(1) + "%)" : "—"}
                  </span>
                </div>
                <div class="stat-cell">
                  <span class="stat-k">CONGESTION</span>
                  <span class="stat-v">{live && stat ? stat.congestionPct.toFixed(1) + "%" : "—"}</span>
                </div>
              </div>

              <div class="health-col">
                <span class="stat-k">HEALTH</span>
                <div class="health-track">
                  <div
                    class="health-fill"
                    style:width={h + "%"}
                    style:background={live && stat ? healthColor(h) : "transparent"}
                  ></div>
                </div>
              </div>

              <button
                class="row-btn"
                class:stop={isRunning(o.state)}
                class:retry={o.state === "error"}
                onclick={() => void toggleRow(o)}
              >
                {isRunning(o.state) ? "Stop" : o.state === "error" ? "Retry" : "Start"}
              </button>
            </div>
          {/each}
        </div>
      </section>
    {/each}

    <button class="add-dest" onclick={() => openSettings("outputs")}>+ &nbsp;Add destination in Settings → Outputs</button>
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
  .head {
    flex: 0 0 auto;
    height: 58px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 16px;
    padding: 0 24px;
    border-bottom: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .head-left {
    display: flex;
    align-items: baseline;
    gap: 12px;
    min-width: 0;
  }
  .title {
    font-family: var(--font-ui);
    font-size: 16px;
    font-weight: 600;
    letter-spacing: -0.01em;
  }
  .sub {
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-muted);
  }
  .head-right {
    display: flex;
    align-items: center;
    gap: 18px;
    flex: 0 0 auto;
  }
  .agg {
    display: flex;
    align-items: center;
    gap: 16px;
    font-family: var(--font-mono);
  }
  .agg-item {
    display: flex;
    align-items: baseline;
    gap: 6px;
  }
  .agg-k {
    font-size: 9px;
    letter-spacing: 0.08em;
    color: var(--color-muted);
  }
  .agg-v {
    font-size: 12px;
    color: var(--color-dim);
  }
  .golive {
    display: flex;
    align-items: center;
    gap: 8px;
    height: auto;
    background: var(--color-accent);
    color: var(--color-accent-ink);
    border: 0;
    font-family: var(--font-ui);
    font-size: 12px;
    font-weight: 600;
    letter-spacing: 0.02em;
    padding: 9px 20px;
  }
  .golive.running {
    background: var(--color-live);
    color: #fff;
  }
  .golive:disabled {
    opacity: 0.5;
    cursor: default;
  }

  .body {
    flex: 1;
    min-height: 0;
    overflow: auto;
    padding: 22px 24px;
    display: flex;
    flex-direction: column;
    gap: 16px;
  }
  .empty {
    margin: 0;
    font-family: var(--font-mono);
    font-size: 12px;
    color: var(--color-muted);
  }

  .card {
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .card-head {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 13px 16px;
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .card-dot {
    flex: 0 0 auto;
    width: 9px;
    height: 9px;
  }
  .card-name {
    font-size: 13px;
    font-weight: 600;
    color: var(--color-text);
  }
  .card-meta {
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.06em;
    color: var(--color-muted);
  }
  .card-spacer {
    flex: 1;
  }
  .card-tag {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.06em;
    padding: 4px 8px;
  }

  .dest-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
    gap: 12px;
    padding: 14px 16px;
  }
  .dest-tile {
    display: flex;
    flex-direction: column;
    gap: 14px;
    padding: 14px;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface-2);
  }
  .dest-head {
    display: flex;
    align-items: center;
    gap: 12px;
  }
  .mark {
    flex: 0 0 auto;
    width: 34px;
    height: 34px;
    display: flex;
    align-items: center;
    justify-content: center;
    font-family: var(--font-mono);
    font-size: 12px;
    color: var(--color-dim);
    background: var(--color-surface-2);
    border: var(--border-weight) solid var(--color-border);
  }
  .dest-col {
    flex: 1;
    min-width: 0;
  }
  .dest-line1 {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .dest-name {
    font-size: 13px;
    font-weight: 500;
    color: var(--color-text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .dest-state {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 8px;
    letter-spacing: 0.06em;
    padding: 2px 6px;
  }
  .dest-sub {
    margin-top: 3px;
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .stats-group {
    display: flex;
    flex-wrap: wrap;
    justify-content: space-between;
    gap: 12px 22px;
    min-width: 0;
  }
  .stat-cell {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .stat-k {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.06em;
    text-transform: uppercase;
    color: var(--color-muted);
  }
  .stat-v {
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-dim);
  }
  .health-col {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }
  .health-track {
    height: 5px;
    background: var(--color-base);
  }
  .health-fill {
    height: 100%;
  }
  .row-btn {
    width: 100%;
    height: auto;
    padding: 8px 16px;
    font-family: var(--font-ui);
    font-size: 11px;
    background: transparent;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-dim);
  }
  .row-btn:hover {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  .row-btn.stop {
    background: var(--color-live);
    border-color: var(--color-live);
    color: #fff;
  }
  .row-btn.stop:hover {
    border-color: var(--color-live);
    color: #fff;
  }
  .row-btn.retry {
    color: var(--color-live);
    border-color: var(--color-live);
  }

  .add-dest {
    height: auto;
    padding: 13px;
    font-family: var(--font-ui);
    font-size: 12px;
    background: transparent;
    border: var(--border-weight) dashed var(--color-border);
    color: var(--color-dim);
  }
  .add-dest:hover {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
</style>
