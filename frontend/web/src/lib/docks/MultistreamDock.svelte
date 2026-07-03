<script lang="ts">
  import {
    obs,
    type CanvasInfo,
    type OutputBindingInfo,
    type MultistreamStatus,
    type MultistreamState,
  } from "../bridge";
  import { setPage } from "../pageStore.svelte";
  import { STATE_COLOR_EXT } from "../theme/stateColors";
  import ToggleSwitch from "../ToggleSwitch.svelte";
  import Icon from "../dock/Icon.svelte";

  // Host supplies tab chrome + strips __* keys; this body declares no props.
  let {}: Record<string, unknown> = $props();

  // A compact, dockable mirror of the Canvases -> Destinations toggles: group
  // bindings by canvas, one master toggle per canvas + one per destination. Toggling
  // is the only interaction; full management lives on the Canvases page.
  let canvases = $state<CanvasInfo[]>([]);
  let bindings = $state<OutputBindingInfo[]>([]);
  let live = $state<MultistreamStatus[]>([]);
  let loaded = $state(false);
  let error = $state<string | null>(null);

  function loadCanvases(): void {
    obs
      .call("canvas.list")
      .then((l) => (canvases = l))
      .catch((e) => (error = (e as Error).message));
  }
  function loadBindings(): void {
    obs
      .call("outputBinding.list")
      .then((l) => (bindings = l))
      .catch((e) => (error = (e as Error).message));
  }
  function loadLive(): void {
    obs
      .call("multistream.status")
      .then((r) => (live = r.outputs))
      .catch(() => {});
  }

  async function loadAll(): Promise<void> {
    try {
      const [cl, bl, status] = await Promise.all([
        obs.call("canvas.list"),
        obs.call("outputBinding.list"),
        obs.call("multistream.status"),
      ]);
      canvases = cl;
      bindings = bl;
      live = status.outputs;
      error = null;
    } catch (e) {
      error = (e as Error).message;
    } finally {
      loaded = true;
    }
  }

  $effect(() => {
    void loadAll();
    const offCanvas = obs.on("canvas.changed", () => loadCanvases());
    const offBindings = obs.on("outputBinding.changed", () => {
      loadBindings();
      loadLive();
    });
    const offMulti = obs.on("multistream.changed", (p) => (live = p.outputs));
    return () => {
      offCanvas();
      offBindings();
      offMulti();
    };
  });

  // bindingUuid -> live status row (only enabled bindings appear in `live`).
  const statusByBinding = $derived.by<Map<string, MultistreamStatus>>(() => {
    const m = new Map<string, MultistreamStatus>();
    for (const o of live) {
      m.set(o.bindingUuid, o);
    }
    return m;
  });

  // Canvases with >=1 binding, in canvas.list order (each carries its own rows).
  const groups = $derived(
    canvases
      .map((c) => ({ canvas: c, rows: bindings.filter((b) => b.canvasUuid === c.uuid) }))
      .filter((g) => g.rows.length > 0),
  );
  const hasAny = $derived(groups.length > 0);

  // The strongest live state across a canvas's enabled bindings (drives the header dot).
  function canvasState(rows: OutputBindingInfo[]): MultistreamState | "off" {
    const on = rows.filter((b) => b.enabled);
    if (on.length === 0) return "off";
    const states = on.map((b) => statusByBinding.get(b.uuid)?.state ?? "idle");
    if (states.includes("live")) return "live";
    if (states.includes("error")) return "error";
    if (states.includes("connecting")) return "connecting";
    return "idle";
  }

  // A destination's effective state: disabled bindings never go live.
  function rowState(b: OutputBindingInfo): MultistreamState | "disabled" {
    if (!b.enabled) return "disabled";
    return statusByBinding.get(b.uuid)?.state ?? "idle";
  }

  function isDangling(label: string): boolean {
    return label === "(deleted)";
  }
  function isUnset(label: string): boolean {
    return label === "(unset)";
  }
  function rowName(b: OutputBindingInfo): string {
    return isUnset(b.profileLabel) ? "No destination" : b.profileLabel;
  }

  async function toggleCanvas(rows: OutputBindingInfo[]): Promise<void> {
    if (rows.length === 0) return;
    const target = !rows.some((b) => b.enabled);
    try {
      await Promise.all(rows.map((b) => obs.call("outputBinding.setEnabled", { uuid: b.uuid, enabled: target })));
      loadBindings();
      loadLive();
    } catch (e) {
      error = (e as Error).message;
    }
  }
  async function toggleRow(b: OutputBindingInfo, enabled: boolean): Promise<void> {
    try {
      await obs.call("outputBinding.setEnabled", { uuid: b.uuid, enabled });
      loadLive();
    } catch (e) {
      error = (e as Error).message;
      // Revert the optimistic toggle (checked is two-way bound to b.enabled).
      b.enabled = !enabled;
    }
  }
</script>

<div class="dock-body">
  <div class="dock-toolbar">
    <button class="dock-add" title="Manage in Canvases" onclick={() => setPage("canvases")}>
      <Icon name="plus" size={12} />
    </button>
  </div>

  {#if error}
    <p class="dock-msg err">{error}</p>
  {/if}

  {#if !loaded}
    <p class="dock-msg">Loading…</p>
  {:else if !hasAny}
    <p class="dock-msg">No destinations yet — add one in Canvases.</p>
  {:else}
    <div class="groups">
      {#each groups as g (g.canvas.uuid)}
        {@const st = canvasState(g.rows)}
        {@const anyOn = g.rows.some((b) => b.enabled)}
        <section class="group">
          <div class="chead">
            <span class="dot" style:background={STATE_COLOR_EXT[st]} title={st}></span>
            <span class="cname">{g.canvas.name}</span>
            {#if g.canvas.isDefault}<span class="badge">DEF</span>{/if}
            <span class="spacer"></span>
            <span class="toggle-wrap" title={anyOn ? "Disable all" : "Enable all"}>
              <ToggleSwitch size="sm" checked={anyOn} onchange={() => void toggleCanvas(g.rows)} />
            </span>
          </div>
          {#each g.rows as b (b.uuid)}
            {@const rs = rowState(b)}
            <div class="drow" class:off={!b.enabled}>
              <span class="toggle-wrap" title={b.enabled ? "Disable" : "Enable"}>
                <ToggleSwitch size="sm" bind:checked={b.enabled} onchange={(v) => void toggleRow(b, v)} />
              </span>
              <span
                class="dname"
                class:deleted={isDangling(b.profileLabel)}
                class:unset={isUnset(b.profileLabel)}
              >
                {rowName(b)}
              </span>
              <span class="dtag" style:color={STATE_COLOR_EXT[rs]}>{rs.toUpperCase()}</span>
            </div>
          {/each}
        </section>
      {/each}
    </div>
  {/if}
</div>

<style>
  .dock-add {
    display: flex;
    align-items: center;
    justify-content: center;
  }
  .groups {
    padding: 6px;
    display: flex;
    flex-direction: column;
    gap: 8px;
  }
  .group {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .chead {
    display: flex;
    align-items: center;
    gap: 7px;
    padding: 6px 8px;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
  }
  .dot {
    width: 8px;
    height: 8px;
    flex-shrink: 0;
  }
  .cname {
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.08em;
    text-transform: uppercase;
    color: var(--color-text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .badge {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 8px;
    letter-spacing: 0.06em;
    padding: 1px 4px;
    color: var(--color-muted);
    border: var(--border-weight) solid var(--color-border);
  }
  .spacer {
    flex: 1;
  }
  .toggle-wrap {
    flex: 0 0 auto;
    display: inline-flex;
    align-items: center;
  }
  .drow {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 6px 8px 6px 18px;
    border: var(--border-weight) solid var(--color-border);
    border-top: 0;
    background: var(--color-base);
  }
  .dname {
    flex: 1;
    min-width: 0;
    font-size: 11px;
    color: var(--color-text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .dname.deleted {
    color: var(--color-live);
    font-style: italic;
  }
  .dname.unset {
    color: var(--color-muted);
    font-style: italic;
  }
  .dtag {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 8px;
    letter-spacing: 0.06em;
  }
  /* Multistream messages use a roomier pad than the shared 8px 7px default. */
  .dock-msg {
    padding: 10px 9px;
  }
</style>
