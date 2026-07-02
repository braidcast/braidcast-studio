<script lang="ts">
  import { obs, type Stats, type OutputStat } from "../bridge";
  import Icon from "../dock/Icon.svelte";

  // Host supplies tab chrome + strips __* keys; this body declares no props.
  let {}: Record<string, unknown> = $props();

  // Title-cased state -> dot color, same token mapping the Multistream dock uses.
  const STATE_COLOR: Record<OutputStat["state"], string> = {
    Idle: "var(--color-muted)",
    Connecting: "var(--meter-yellow)",
    Live: "var(--meter-green)",
    Error: "var(--color-live)",
  };

  // Polling cadence (ms). There is no push for stats, so the dock owns the loop.
  const POLL_MS = 1000;
  // A non-trivial lag/skip pct that warrants the warn color.
  const WARN_PCT = 1;

  let stats = $state<Stats | null>(null);
  let loaded = $state(false);
  let error = $state<string | null>(null);

  async function load() {
    try {
      stats = await obs.call("stats.get");
      error = null;
    } catch (e) {
      error = (e as Error).message;
    } finally {
      loaded = true;
    }
  }

  $effect(() => {
    void load();
    const timer = setInterval(() => void load(), POLL_MS);
    return () => clearInterval(timer);
  });

  function fmtMemory(mb: number): string {
    return mb >= 1024 ? (mb / 1024).toFixed(1) + " GB" : Math.round(mb) + " MB";
  }

  function fmtBitrate(kbps: number): string {
    return kbps >= 1000 ? (kbps / 1000).toFixed(1) + " Mb/s" : Math.round(kbps) + " kb/s";
  }

  function fmtDuration(ms: number): string {
    const total = Math.floor(ms / 1000);
    const h = Math.floor(total / 3600);
    const m = Math.floor((total % 3600) / 60);
    const s = total % 60;
    const pad = (n: number) => String(n).padStart(2, "0");
    return h > 0 ? `${h}:${pad(m)}:${pad(s)}` : `${m}:${pad(s)}`;
  }

  function fmtPct(pct: number): string {
    return pct.toFixed(1) + "%";
  }
</script>

<div class="dock-body">
  {#if error}
    <p class="dock-msg err">{error}</p>
  {/if}

  {#if !loaded}
    <p class="dock-msg">Loading…</p>
  {:else if stats}
    <div class="section">
      <div class="grid">
        <span class="lbl">CPU</span>
        <span class="val">{fmtPct(stats.general.cpu)}</span>
        <span class="lbl">Memory</span>
        <span class="val">{fmtMemory(stats.general.memoryMB)}</span>
        <span class="lbl">FPS</span>
        <span class="val">{stats.general.fps}</span>
        <span class="lbl">Avg frame</span>
        <span class="val">{stats.general.avgFrameMs.toFixed(1)} ms</span>
        <span class="lbl" class:warn={stats.general.renderLagPct > WARN_PCT}>Render lag</span>
        <span class="val" class:warn={stats.general.renderLagPct > WARN_PCT}>
          {fmtPct(stats.general.renderLagPct)}
          <span class="sub">({stats.general.renderLagged}/{stats.general.renderTotal})</span>
        </span>
        <span class="lbl" class:warn={stats.general.encodeSkipPct > WARN_PCT}>Encoder skip</span>
        <span class="val" class:warn={stats.general.encodeSkipPct > WARN_PCT}>
          {fmtPct(stats.general.encodeSkipPct)}
          <span class="sub">({stats.general.encodeSkipped}/{stats.general.encodeTotal})</span>
        </span>
      </div>
    </div>

    <div class="section outputs">
      {#if stats.outputs.length === 0}
        <p class="dock-msg">No outputs configured</p>
      {:else}
        <ul class="list">
          {#each stats.outputs as o (o.bindingUuid)}
            <li class="row">
              <span class="dot" style:background={STATE_COLOR[o.state]} title={o.state}></span>
              <div class="info">
                <div class="line1">
                  <span class="name">{o.profileLabel}</span>
                  <span class="arrow"><Icon name="caret-right" size={10} /></span>
                  <span class="canvas">{o.canvasName}</span>
                </div>
                <div class="line2">
                  <span class="stat">{fmtBitrate(o.bitrateKbps)}</span>
                  <span class="stat" class:warn={o.dropPct > 0}>
                    drop {o.droppedFrames} ({fmtPct(o.dropPct)})
                  </span>
                  <span class="stat">cong {fmtPct(o.congestionPct)}</span>
                  <span class="stat">{fmtDuration(o.durationMs)}</span>
                </div>
              </div>
            </li>
          {/each}
        </ul>
      {/if}
    </div>
  {/if}
</div>

<style>
  .section {
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .section.outputs {
    border-bottom: none;
  }
  .grid {
    display: grid;
    grid-template-columns: auto 1fr;
    gap: 3px 12px;
    padding: 8px 9px;
    font-size: 11px;
    font-variant-numeric: tabular-nums;
  }
  .lbl {
    color: var(--color-muted);
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
  }
  .val {
    color: var(--color-text);
    text-align: right;
    font-family: var(--font-mono);
    font-variant-numeric: tabular-nums;
  }
  .sub {
    color: var(--color-muted);
    font-family: var(--font-mono);
    font-variant-numeric: tabular-nums;
  }
  .warn {
    color: var(--meter-yellow);
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
    align-items: center;
    gap: 8px;
    padding: 7px 9px;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
  }
  .dot {
    width: 9px;
    height: 9px;
    flex-shrink: 0;
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
  .line2 {
    display: flex;
    flex-wrap: wrap;
    gap: 4px 10px;
    margin-top: 3px;
    font-size: 10px;
    color: var(--color-muted);
    font-variant-numeric: tabular-nums;
  }
  .stat {
    font-family: var(--font-mono);
    font-variant-numeric: tabular-nums;
  }
</style>
