<script lang="ts">
  import type { OutputBindingInfo, MultistreamState } from "$lib/api/bridge";
  import { canvasStore } from "$lib/stores/canvasStore.svelte";
  import {
    outputBindingStore,
    bindingDisplayName,
    isBindingDangling,
    isBindingUnset,
  } from "$lib/stores/outputBindingStore.svelte";
  import { multistreamStatusStore, bindingRowState, bindingRowDetail } from "$lib/stores/multistreamStatusStore.svelte";
  import { destinationIdentityStore } from "$lib/stores/destinationIdentityStore.svelte";
  import { viewerCountStore } from "$lib/stores/viewerCountStore.svelte";
  import { STATE_COLOR_EXT } from "$lib/theme/stateColors";
  import { fmtCompact } from "$lib/utils/format";
  import ToggleSwitch from "$lib/ui/ToggleSwitch.svelte";
  import PlatformMark from "$lib/ui/PlatformMark.svelte";

  // Host supplies tab chrome + strips __* keys; this body declares no props.
  let {}: Record<string, unknown> = $props();

  // A compact, dockable mirror of the Canvases -> Destinations toggles: group
  // bindings by canvas, one master toggle per canvas + one per destination. Toggling
  // is the only interaction; full management lives on the Canvases page.
  // Canvas + binding lists + live status all come from the shared stores (one source
  // of truth for each leg of the model).
  let canvases = $derived(canvasStore.canvases);
  let bindings = $derived(outputBindingStore.bindings);
  let error = $state<string | null>(null);
  let loaded = $derived(multistreamStatusStore.loaded);

  $effect(() => {
    canvasStore.start();
    outputBindingStore.start();
    destinationIdentityStore.start();
    const offStatus = multistreamStatusStore.subscribe();
    const offViewers = viewerCountStore.subscribe();
    return () => {
      offStatus();
      offViewers();
    };
  });

  const statusByBinding = $derived(multistreamStatusStore.statusByBinding);

  // A destination's viewer count, or null when it isn't known -- off, still
  // connecting, or live but the count hasn't arrived yet. Never 0 unless a real
  // report says so: only a reported row can produce a number, so "not yet reporting"
  // can't be mistaken for "reporting zero". The store keys rows the host's way, so the
  // binding contributes its account + profile pair and nothing joins them here.
  function rowViewerCount(b: OutputBindingInfo, rs: MultistreamState | "disabled"): number | null {
    if (rs !== "live") return null;
    const accountId = destinationIdentityStore.forProfile(b.profileUuid)?.accountId;
    return accountId ? viewerCountStore.countFor(accountId, b.profileUuid) : null;
  }

  // A canvas's viewer total: the sum of its enabled destinations that are actually
  // reporting. `partial` marks a group where at least one enabled destination isn't
  // reporting yet, so the header can flag the sum as a floor rather than read as
  // complete. null (no header count shown) only when nothing in the group is
  // reporting -- the "say nothing" rule extended to the group total.
  function canvasViewerCount(rows: OutputBindingInfo[]): { total: number; partial: boolean } | null {
    const enabled = rows.filter((b) => b.enabled);
    if (enabled.length === 0) return null;
    let total = 0;
    let known = 0;
    for (const b of enabled) {
      const v = rowViewerCount(b, bindingRowState(b, statusByBinding));
      if (v !== null) {
        total += v;
        known++;
      }
    }
    return known === 0 ? null : { total, partial: known < enabled.length };
  }

  // Canvases with >=1 binding, in canvas.list order (each carries its own rows).
  const groups = $derived(
    canvases
      .map((c) => ({ canvas: c, rows: bindings.filter((b) => b.canvasUuid === c.uuid) }))
      .filter((g) => g.rows.length > 0),
  );
  const hasAny = $derived(groups.length > 0);

  // The strongest live state across a canvas's enabled bindings (drives the header dot).
  function canvasState(rows: OutputBindingInfo[]): MultistreamState | "off" {
    return multistreamStatusStore.deriveCanvasState(rows);
  }

  async function toggleCanvas(rows: OutputBindingInfo[]): Promise<void> {
    if (rows.length === 0) return;
    const target = !rows.some((b) => b.enabled);
    try {
      await outputBindingStore.setEnabled(rows.map((b) => b.uuid), target);
      void multistreamStatusStore.refresh();
    } catch (e) {
      error = (e as Error).message;
    }
  }
  async function toggleRow(b: OutputBindingInfo, enabled: boolean): Promise<void> {
    try {
      await outputBindingStore.setEnabled([b.uuid], enabled);
      void multistreamStatusStore.refresh();
    } catch (e) {
      error = (e as Error).message;
      // Revert the optimistic toggle (checked is two-way bound to b.enabled).
      b.enabled = !enabled;
    }
  }
</script>

<div class="dock-body">
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
        {@const vc = canvasViewerCount(g.rows)}
        <section class="group">
          <div class="chead">
            <span class="dot" style:background={STATE_COLOR_EXT[st]} title={st}></span>
            <span class="cname">{g.canvas.name}</span>
            {#if g.canvas.isDefault}<span class="badge">DEF</span>{/if}
            <span class="spacer"></span>
            {#if vc}
              <span
                class="dcount"
                title={vc.partial ? "Partial total — not every destination in this canvas is reporting yet" : undefined}
              >{fmtCompact(vc.total)}{vc.partial ? "+" : ""}</span>
            {/if}
            <span class="toggle-wrap" title={anyOn ? "Disable all" : "Enable all"}>
              <ToggleSwitch size="sm" checked={anyOn} onchange={() => void toggleCanvas(g.rows)} />
            </span>
          </div>
          {#each g.rows as b (b.uuid)}
            {@const rs = bindingRowState(b, statusByBinding)}
            {@const identity = destinationIdentityStore.forProfile(b.profileUuid)}
            {@const viewers = rowViewerCount(b, rs)}
            <div class="drow" class:off={!b.enabled}>
              <span class="toggle-wrap" title={b.enabled ? "Disable" : "Enable"}>
                <ToggleSwitch size="sm" bind:checked={b.enabled} onchange={(v) => void toggleRow(b, v)} />
              </span>
              {#if identity}
                <span class="dmark"><PlatformMark platform={identity.platform} size={12} /></span>
              {/if}
              <span
                class="dname"
                class:deleted={isBindingDangling(b.profileLabel)}
                class:unset={isBindingUnset(b.profileLabel)}
              >
                {identity ? identity.displayName : bindingDisplayName(b)}
              </span>
              <span class="dstat">
                <span class="dtag" style:color={STATE_COLOR_EXT[rs]} title={bindingRowDetail(b, statusByBinding) || undefined}>
                  {rs.toUpperCase()}
                </span>
                {#if viewers !== null}
                  <span class="dsep">|</span>
                  <span class="dcount">{fmtCompact(viewers)}</span>
                {/if}
              </span>
            </div>
          {/each}
        </section>
      {/each}
    </div>
  {/if}
</div>

<style>
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
  .dmark {
    flex: 0 0 auto;
    display: grid;
    place-items: center;
  }
  /* State tag and viewer count share one mono cluster so the row keeps a single
     right-hand column. tabular-nums stops the count jittering as it ticks. */
  .dstat {
    flex: 0 0 auto;
    display: flex;
    align-items: baseline;
    gap: 4px;
  }
  .dsep {
    color: var(--color-border);
    font-family: var(--font-mono);
    font-size: 8px;
  }
  .dcount {
    font-family: var(--font-mono);
    font-size: 9.5px;
    color: var(--color-dim);
    font-variant-numeric: tabular-nums;
  }
  .chead .dcount {
    font-size: 10px;
  }
  /* Multistream messages use a roomier pad than the shared 8px 7px default. */
  .dock-msg {
    padding: 10px 9px;
  }
</style>
