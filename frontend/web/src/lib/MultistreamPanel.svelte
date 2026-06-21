<script lang="ts">
  import { obs, type MultistreamStatus, type MultistreamState } from "./bridge";

  // State -> dot color, mirrors the legacy StateColor palette (Contract).
  const STATE_COLOR: Record<MultistreamState, string> = {
    idle: "#888888",
    connecting: "#e0a000",
    live: "#2ecc40",
    error: "#ff4136",
  };

  let outputs = $state<MultistreamStatus[]>([]);
  let loaded = $state(false);
  let error = $state<string | null>(null);

  async function load() {
    try {
      const res = await obs.call("multistream.status");
      outputs = res.outputs;
    } catch (e) {
      error = (e as Error).message;
    } finally {
      loaded = true;
    }
  }

  $effect(() => {
    void load();
    // Authoritative status comes from the engine push; binding enable/disable
    // changes the row set, so re-fetch on that.
    const offChanged = obs.on("multistream.changed", (p) => {
      outputs = p.outputs;
    });
    const offBindings = obs.on("outputBinding.changed", () => void load());
    return () => {
      offChanged();
      offBindings();
    };
  });

  // Start when stopped (idle/error); Stop when running (connecting/live).
  function isRunning(state: MultistreamState): boolean {
    return state === "connecting" || state === "live";
  }

  async function toggle(o: MultistreamStatus) {
    error = null;
    try {
      if (isRunning(o.state)) {
        await obs.call("multistream.stopOutput", { uuid: o.bindingUuid });
      } else {
        await obs.call("multistream.startOutput", { uuid: o.bindingUuid });
      }
      // The authoritative row update arrives via multistream.changed.
    } catch (e) {
      error = (e as Error).message;
    }
  }
</script>

<section class="panel">
  <header>
    <h2>Multistream</h2>
  </header>

  {#if error}
    <p class="error">{error}</p>
  {/if}

  {#if !loaded}
    <p class="dim">Loading…</p>
  {:else if outputs.length === 0}
    <p class="dim">No enabled outputs — enable one in Settings → Outputs.</p>
  {:else}
    <ul class="list">
      {#each outputs as o (o.bindingUuid)}
        <li class="row">
          <span class="dot" style:background={STATE_COLOR[o.state]} title={o.state}></span>
          <div class="info">
            <div class="line1">
              <span class="name">{o.profileLabel}</span>
              <span class="arrow">→</span>
              <span class="canvas">{o.canvasName}</span>
            </div>
            {#if o.state === "error" && o.lastError}
              <div class="lasterr">{o.lastError}</div>
            {/if}
          </div>
          <button class="mini" onclick={() => void toggle(o)}>
            {isRunning(o.state) ? "Stop" : "Start"}
          </button>
        </li>
      {/each}
    </ul>
  {/if}
</section>

<style>
  .panel {
    border: 1px solid var(--border);
    border-radius: 10px;
    background: var(--bg-raised);
    padding: 14px 16px;
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  header {
    display: flex;
    align-items: center;
    justify-content: space-between;
  }
  h2 {
    margin: 0;
    font-size: 13px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--text-dim);
  }
  .list {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .row {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 8px 10px;
    border: 1px solid var(--border);
    border-radius: 8px;
    background: var(--bg-sunken);
  }
  .dot {
    width: 9px;
    height: 9px;
    border-radius: 50%;
    flex-shrink: 0;
  }
  .info {
    min-width: 0;
    flex: 1;
  }
  .line1 {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 13px;
    min-width: 0;
  }
  .name {
    color: var(--text);
    font-weight: 500;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .arrow {
    color: var(--text-dim);
    flex-shrink: 0;
  }
  .canvas {
    color: var(--text-soft);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .lasterr {
    margin-top: 2px;
    font-size: 11px;
    color: var(--off, #d65a5a);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .mini {
    background: none;
    border: 1px solid var(--border);
    border-radius: 6px;
    color: var(--text-soft);
    cursor: pointer;
    font: inherit;
    font-size: 12px;
    padding: 4px 10px;
    line-height: 1;
    flex-shrink: 0;
  }
  .mini:hover:not(:disabled) {
    color: var(--text);
    background: var(--bg-raised);
  }
  .dim {
    color: var(--text-dim);
    margin: 0;
  }
  .error {
    color: var(--off, #d65a5a);
    margin: 0;
    font-size: 12px;
  }
</style>
