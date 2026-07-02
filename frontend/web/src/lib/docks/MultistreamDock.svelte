<script lang="ts">
  import { obs, type MultistreamStatus, type MultistreamState } from "../bridge";
  import { setPage } from "../pageStore.svelte";
  import Icon from "../dock/Icon.svelte";

  // Host supplies tab chrome + strips __* keys; this body declares no props.
  let {}: Record<string, unknown> = $props();

  // State -> dot color, mapped to theme tokens (re-skins with the active preset).
  const STATE_COLOR: Record<MultistreamState, string> = {
    idle: "var(--color-muted)",
    connecting: "var(--meter-yellow)",
    live: "var(--meter-green)",
    error: "var(--color-live)",
  };

  let outputs = $state<MultistreamStatus[]>([]);
  let loaded = $state(false);
  let error = $state<string | null>(null);

  async function load() {
    try {
      const res = await obs.call("multistream.status");
      outputs = res.outputs;
      error = null;
    } catch (e) {
      error = (e as Error).message;
    } finally {
      loaded = true;
    }
  }

  $effect(() => {
    void load();
    // Authoritative status comes from the engine push; the enabled-binding set
    // changes the row list, so re-fetch on outputBinding.changed.
    const offChanged = obs.on("multistream.changed", (p) => {
      outputs = p.outputs;
    });
    const offBindings = obs.on("outputBinding.changed", () => void load());
    return () => {
      offChanged();
      offBindings();
    };
  });

</script>

<div class="dock-body">
  <div class="dock-toolbar">
    <button class="dock-add" title="Manage destinations (Canvases)" onclick={() => setPage("canvases")}>
      <Icon name="plus" size={12} />
    </button>
  </div>

  {#if error}
    <p class="dock-msg err">{error}</p>
  {/if}

  {#if !loaded}
    <p class="dock-msg">Loading…</p>
  {:else if outputs.length === 0}
    <p class="dock-msg">No enabled outputs — add one in Settings → Outputs.</p>
  {:else}
    <ul class="list">
      {#each outputs as o (o.bindingUuid)}
        <li class="row">
          <span class="dot" style:background={STATE_COLOR[o.state]} title={o.state}></span>
          <div class="info">
            <div class="line1">
              <span class="name">{o.profileLabel}</span>
              <span class="arrow"><Icon name="caret-right" size={10} /></span>
              <span class="canvas">{o.canvasName}</span>
            </div>
            {#if o.state === "error" && o.lastError}
              <div class="lasterr">{o.lastError}</div>
            {/if}
          </div>
        </li>
      {/each}
    </ul>
  {/if}
</div>

<style>
  .dock-add {
    display: flex;
    align-items: center;
    justify-content: center;
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
  .lasterr {
    margin-top: 2px;
    font-size: 10px;
    color: var(--color-live);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  /* Multistream messages use a roomier pad than the shared 8px 7px default. */
  .dock-msg {
    padding: 10px 9px;
  }
</style>
