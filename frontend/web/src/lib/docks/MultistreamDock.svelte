<script lang="ts">
  import { untrack } from "svelte";
  import type { OutputBindingInfo, MultistreamState } from "$lib/api/bridge";
  import { canvasStore } from "$lib/stores/canvasStore.svelte";
  import {
    outputBindingStore,
    bindingDisplayName,
    isBindingDangling,
    isBindingUnset,
  } from "$lib/stores/outputBindingStore.svelte";
  import {
    multistreamStatusStore,
    bindingRowState,
    bindingRowStatus,
  } from "$lib/stores/multistreamStatusStore.svelte";
  import { destinationIdentityStore } from "$lib/stores/destinationIdentityStore.svelte";
  import { viewerCountStore } from "$lib/stores/viewerCountStore.svelte";
  import { STATE_COLOR_EXT } from "$lib/theme/stateColors";
  import { isRetrying, retryDestination, setDestinationsEnabled } from "$lib/ui/destinationArming.svelte";
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
  // Whether anything is on the air, which is what decides how an enabled-but-not-live row
  // reads. Off the store's memoized derived rather than re-scanning the map here: this
  // dock reads the store directly, so there is nothing to gain from a second copy.
  const anyLive = $derived(multistreamStatusStore.anyLive);

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

  // The strongest live state across a canvas's enabled bindings (drives the header dot).
  function canvasState(rows: OutputBindingInfo[]): MultistreamState | "off" {
    return multistreamStatusStore.deriveCanvasState(rows);
  }

  // Per-uuid tier snapshot (0 = enabled, 1 = disabled), taken at a structural
  // boundary -- mount, or the canvas/binding uuid SET changing -- never merely
  // because `enabled` flipped. `groups` below sorts the LIVE store arrays by this
  // snapshot: a toggle changes `enabled` without changing its tier, so the row's
  // sort key -- and therefore its position -- doesn't move until the next
  // boundary. Sorting live data rather than holding a separate order list leaves
  // nothing to orphan or grow unboundedly.
  //
  // Re-homing a binding to another canvas changes no uuid, so it does not trigger
  // a re-snapshot: bindingTier stays right, but canvasTier can hold a canvas at
  // the enabled tier after it loses its last enabled binding. Ordering only, and
  // it corrects on the next add/remove.
  let canvasTier = $state<Map<string, 0 | 1>>(new Map());
  let bindingTier = $state<Map<string, 0 | 1>>(new Map());

  // Order-independent fingerprint of an id list, so the *same* uuid set in a
  // different order doesn't read as a structural change. A store-side reorder
  // therefore propagates immediately rather than waiting for a re-snapshot: the
  // tiers stay correct across it, and the stable sort below preserves whatever
  // new order the store hands back within each tier.
  function idsKey(ids: string[]): string {
    return ids.slice().sort().join(",");
  }
  let lastIdsKey = "";

  // Rebuilds canvasTier/bindingTier from the current stores. Called only from
  // inside the untrack() below, so every `.enabled` read here is a one-off
  // snapshot, never a live dependency of the trigger effect.
  function snapshotTiers(): void {
    const bindings = outputBindingStore.bindings;
    const nextBindingTier = new Map<string, 0 | 1>();
    const canvasHasEnabled = new Set<string>();
    for (const b of bindings) {
      nextBindingTier.set(b.uuid, b.enabled ? 0 : 1);
      if (b.enabled) {
        canvasHasEnabled.add(b.canvasUuid);
      }
    }
    bindingTier = nextBindingTier;

    const nextCanvasTier = new Map<string, 0 | 1>();
    for (const c of canvases) {
      nextCanvasTier.set(c.uuid, canvasHasEnabled.has(c.uuid) ? 0 : 1);
    }
    canvasTier = nextCanvasTier;
  }

  // Structural-boundary trigger: reads only `.uuid` fields outside untrack(), so a
  // binding's `.enabled` write -- the synchronous two-way-bound toggle, or the
  // refresh() it triggers -- never becomes a dependency of this effect regardless
  // of what snapshotTiers reads. outputBindingStore/canvasStore replace their
  // arrays wholesale on every refresh (including a toggle's), so this still fires
  // then; the idsKey comparison inside untrack() is what turns that into a no-op
  // unless a canvas/binding actually entered or left the set.
  $effect(() => {
    const canvasIds = canvases.map((c) => c.uuid);
    const bindingIds = outputBindingStore.bindings.map((b) => b.uuid);
    untrack(() => {
      const key = `${idsKey(canvasIds)}|${idsKey(bindingIds)}`;
      if (key === lastIdsKey) {
        return;
      }
      lastIdsKey = key;
      snapshotTiers();
    });
  });

  // Canvases with >=1 binding, sorted by the snapshot tiers above. Stable within
  // each tier. `.get(uuid) ?? 0` defaults an id the snapshot hasn't caught up to
  // yet (a canvas/binding created after the last boundary) into the enabled tier
  // rather than dropping or misplacing it -- true today because
  // OutputBinding::enabled defaults false server-side, so a brand-new binding is
  // disabled and this default only ever matters for the split second before the
  // next boundary catches up.
  const groups = $derived.by(() => {
    const rowsByCanvas = new Map<string, OutputBindingInfo[]>();
    for (const b of outputBindingStore.bindings) {
      const list = rowsByCanvas.get(b.canvasUuid);
      if (list) {
        list.push(b);
      } else {
        rowsByCanvas.set(b.canvasUuid, [b]);
      }
    }
    return canvases
      .filter((c) => (rowsByCanvas.get(c.uuid)?.length ?? 0) > 0)
      .map((c) => ({
        canvas: c,
        rows: (rowsByCanvas.get(c.uuid) ?? [])
          .slice()
          .sort((a, b) => (bindingTier.get(a.uuid) ?? 0) - (bindingTier.get(b.uuid) ?? 0)),
      }))
      .sort((a, b) => (canvasTier.get(a.canvas.uuid) ?? 0) - (canvasTier.get(b.canvas.uuid) ?? 0));
  });
  const hasAny = $derived(groups.length > 0);

  async function toggleCanvas(rows: OutputBindingInfo[]): Promise<void> {
    if (rows.length === 0) return;
    const target = !rows.some((b) => b.enabled);
    try {
      await setDestinationsEnabled(rows.map((b) => b.uuid), target);
      void multistreamStatusStore.refresh();
    } catch (e) {
      error = (e as Error).message;
    }
  }
  async function toggleRow(b: OutputBindingInfo, enabled: boolean): Promise<void> {
    try {
      await setDestinationsEnabled([b.uuid], enabled);
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
            {@const st = bindingRowStatus(b, statusByBinding, anyLive)}
            {@const identity = destinationIdentityStore.forProfile(b.profileUuid)}
            {@const name = identity ? identity.displayName : bindingDisplayName(b)}
            {@const viewers = rowViewerCount(b, st.state)}
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
                {name}
              </span>
              <span class="dstat">
                <!-- The dock is the compact mirror; the sentence behind the tag stays on
                     the title here and is rendered in full on the Canvases tab, where a
                     row has the width for it. -->
                <span class="dtag" style:color={STATE_COLOR_EXT[st.state]} title={st.detail || undefined}>
                  {st.label.toUpperCase()}
                </span>
                {#if st.retryable}
                  <!-- aria-disabled, not disabled: a native disable drops the control out
                       of the tab order the instant it is pressed, so the state it changed
                       into is never announced and focus is lost. retryDestination guards
                       its own re-entry, so nothing needs the DOM to refuse a second click. -->
                  <button
                    class="dretry"
                    aria-disabled={isRetrying(b.uuid)}
                    aria-busy={isRetrying(b.uuid)}
                    aria-label={isRetrying(b.uuid) ? "Starting " + name + ", please wait" : "Retry " + name}
                    title={"Bring " + name + " back onto this stream"}
                    onclick={() => void retryDestination(b.uuid, name)}
                  >
                    {isRetrying(b.uuid) ? "STARTING" : "RETRY"}
                  </button>
                {/if}
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
  /* Centered rather than baseline-aligned: a bordered box on the text baseline sits
     visibly low beside the tag it belongs to. 10px matches the canvas name and the viewer
     count on these rows -- the 8px the status tags use is a label size, not one to put on
     a control -- and min-width holds both words so the row does not reflow when the
     button changes to STARTING. */
  .dretry {
    flex: 0 0 auto;
    align-self: center;
    min-width: 62px;
    padding: 3px 6px;
    background: none;
    border: var(--border-weight) solid var(--color-live);
    color: var(--color-live);
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.06em;
    line-height: 12px;
    cursor: pointer;
  }
  .dretry:hover:not([aria-disabled="true"]) {
    background: color-mix(in srgb, var(--color-live) 16%, transparent);
  }
  .dretry:focus-visible {
    outline: var(--border-weight) solid var(--color-accent);
    outline-offset: 1px;
  }
  .dretry[aria-disabled="true"] {
    cursor: default;
    color: var(--color-muted);
    border-color: var(--color-border);
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
