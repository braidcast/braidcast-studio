<script lang="ts">
  import { onDestroy, onMount } from "svelte";
  import type { DockviewApi } from "dockview-core";
  import DockHost from "../dock/DockHost.svelte";
  import { DOCKS, panelOptions } from "../dock/dockRegistry";
  import { layoutStore } from "../dock/layoutStore.svelte";
  import {
    startCanvasDockReconciler,
    reconcileCanvasDocks,
    setCanvasUserHidden,
    clearCanvasUserHidden,
  } from "../dock/canvasReconciler";
  import { obs, type CanvasInfo, type MultistreamStatus, type MultistreamState, type Stats } from "../bridge";
  import { pageStore } from "../pageStore.svelte";
  import { suspendPreview } from "../previewGate.svelte";
  import CollectionDialog, { type DialogSpec } from "../CollectionDialog.svelte";

  let api = $state<DockviewApi | undefined>(undefined);
  let visibleDocks = $state<Record<string, boolean>>({});
  let canvasDocksPresent = $state<Set<string>>(new Set());
  let stopReconciler: (() => void) | undefined;

  // CANVASES bar + GO-LIVE bar data, read independently of the Dockview lifecycle.
  let canvases = $state<CanvasInfo[]>([]);
  let outputs = $state<MultistreamStatus[]>([]);
  let stats = $state<Stats | null>(null);
  let focusedCanvasUuid = $state<string | null>(null);
  let busy = $state(false);
  let dialog = $state<DialogSpec | null>(null);

  // State -> color, same token mapping the Multistream/Stats docks use. Used for
  // the per-canvas dock header dots (idle = muted).
  const STATE_COLOR: Record<MultistreamState, string> = {
    idle: "var(--color-muted)",
    connecting: "var(--meter-yellow)",
    live: "var(--meter-green)",
    error: "var(--color-live)",
  };

  // The focused canvas drives the bottom bar: default to the active CANVASES chip,
  // else the Default canvas, else the first canvas. `null` only before the list
  // loads. Live state below is scoped to THIS canvas's outputs, not all outputs.
  let activeCanvasUuid = $derived(
    focusedCanvasUuid ?? canvases.find((c) => c.isDefault)?.uuid ?? canvases[0]?.uuid ?? null,
  );
  let focusedOutputs = $derived(outputs.filter((o) => o.canvasUuid === activeCanvasUuid));

  // Live state across the FOCUSED canvas's enabled outputs: live wins, then
  // connecting, then error, else idle. Drives the focus dot, live badge, button.
  let liveState = $derived.by<MultistreamState>(() => {
    if (focusedOutputs.some((o) => o.state === "live")) return "live";
    if (focusedOutputs.some((o) => o.state === "connecting")) return "connecting";
    if (focusedOutputs.some((o) => o.state === "error")) return "error";
    return "idle";
  });
  let anyRunning = $derived(focusedOutputs.some((o) => o.state === "live" || o.state === "connecting"));

  // Mock focusedDot: green when live, accent otherwise (connecting/error keep the
  // meter colors so the dot still reads transient states).
  let focusDotColor = $derived(
    liveState === "live"
      ? "var(--meter-green)"
      : liveState === "connecting"
        ? "var(--meter-yellow)"
        : liveState === "error"
          ? "var(--color-live)"
          : "var(--color-accent)",
  );

  let focusedCanvas = $derived(canvases.find((c) => c.uuid === activeCanvasUuid) ?? null);
  // Mock focusedLabel: "Name · WxH · fps".
  let focusedLabel = $derived(
    focusedCanvas
      ? focusedCanvas.name +
          " · " +
          focusedCanvas.outputWidth +
          "×" +
          focusedCanvas.outputHeight +
          " · " +
          Math.round(focusedCanvas.fpsNum / focusedCanvas.fpsDen)
      : "Default Canvas",
  );

  // Real elapsed for the live badge: the longest-running live output bound to the
  // focused canvas (stats.outputs carry durationMs, joined by bindingUuid). Updates
  // with the 1s stats poll.
  let liveDurationMs = $derived.by<number>(() => {
    if (!stats) return 0;
    const ids = new Set(focusedOutputs.filter((o) => o.state === "live").map((o) => o.bindingUuid));
    let max = 0;
    for (const o of stats.outputs) {
      if (ids.has(o.bindingUuid) && o.durationMs > max) max = o.durationMs;
    }
    return max;
  });

  function fmtDuration(ms: number): string {
    const s = Math.floor(ms / 1000);
    const pad = (n: number): string => String(n).padStart(2, "0");
    return pad(Math.floor(s / 3600)) + ":" + pad(Math.floor((s % 3600) / 60)) + ":" + pad(s % 60);
  }

  // Perf summary for the bottom bar, derived from the polled stats snapshot. NET is
  // the summed output bitrate; em-dash when stats are absent or no outputs exist.
  let perfRow = $derived.by<{ k: string; v: string }[]>(() => {
    if (!stats) {
      return [
        { k: "CPU", v: "—" },
        { k: "FPS", v: "—" },
        { k: "RENDER", v: "—" },
        { k: "ENCODE", v: "—" },
        { k: "NET", v: "—" },
      ];
    }
    const g = stats.general;
    const totalKbps = stats.outputs.reduce((sum, o) => sum + o.bitrateKbps, 0);
    const net =
      stats.outputs.length === 0
        ? "—"
        : totalKbps >= 1000
          ? (totalKbps / 1000).toFixed(1) + " Mb/s"
          : Math.round(totalKbps) + " kb/s";
    return [
      { k: "CPU", v: g.cpu.toFixed(1) + "%" },
      { k: "FPS", v: String(Math.round(g.fps)) },
      { k: "RENDER", v: g.renderLagPct.toFixed(1) + "%" },
      { k: "ENCODE", v: g.encodeSkipPct.toFixed(1) + "%" },
      { k: "NET", v: net },
    ];
  });

  onDestroy(() => stopReconciler?.());

  // Explicitly suspend every native preview overlay (Default + each CanvasDock,
  // which all hide off the ref-counted previewGate) while Studio is off-page,
  // instead of relying on the ResizeObserver -> 0x0 rect path. Restored on return.
  $effect(() => {
    if (pageStore.page !== "studio") {
      return suspendPreview();
    }
  });

  // Gate the 1s stats poll on the Studio page. StudioPage never unmounts (it just
  // hides), so an onMount interval would poll app-wide; this starts the poll only
  // while Studio is shown and clears it on leave / destroy.
  function loadStats(): void {
    obs
      .call("stats.get")
      .then((s) => (stats = s))
      .catch(() => {});
  }
  $effect(() => {
    if (pageStore.page !== "studio") return;
    loadStats();
    const timer = setInterval(loadStats, 1000);
    return () => clearInterval(timer);
  });

  // Drive each per-canvas dock header dot off that canvas's own live state, so the
  // dot tracks the output state instead of the reconciler's static placeholder.
  $effect(() => {
    if (!api) return;
    for (const c of canvases) {
      // The Default canvas lives in the static "preview" dock; non-default canvases
      // in reconciler-managed "canvas:<uuid>" panels. Both get the same live->green
      // dot mapping (updateParameters merges, so only __dot is touched).
      const panel = api.getPanel(c.isDefault ? "preview" : "canvas:" + c.uuid);
      if (!panel) continue;
      const co = outputs.filter((o) => o.canvasUuid === c.uuid);
      const state: MultistreamState = co.some((o) => o.state === "live")
        ? "live"
        : co.some((o) => o.state === "connecting")
          ? "connecting"
          : co.some((o) => o.state === "error")
            ? "error"
            : "idle";
      panel.api.updateParameters({ __dot: STATE_COLOR[state] });
    }
  });

  // Chip + bottom-bar data, refreshed off the same engine pushes the docks use.
  onMount(() => {
    const loadCanvases = () => {
      obs
        .call("canvas.list")
        .then((list) => {
          canvases = list;
          // A focused canvas that was deleted/disabled would otherwise stick the
          // GO-LIVE bar on a non-existent canvas; drop it so focus falls back to
          // the Default canvas via the activeCanvasUuid derivation.
          if (focusedCanvasUuid && !list.some((c) => c.uuid === focusedCanvasUuid)) {
            focusedCanvasUuid = null;
          }
        })
        .catch(() => {});
    };
    const loadStatus = () => {
      obs
        .call("multistream.status")
        .then((res) => (outputs = res.outputs))
        .catch(() => {});
    };
    loadCanvases();
    loadStatus();
    const offCanvas = obs.on("canvas.changed", loadCanvases);
    const offMulti = obs.on("multistream.changed", (p) => (outputs = p.outputs));
    const offBindings = obs.on("outputBinding.changed", loadStatus);
    return () => {
      offCanvas();
      offMulti();
      offBindings();
    };
  });

  // Tear-out seam (P3a). The ⧉ affordance asks the host to open a real OS window
  // owning this dock, then drops the panel from this (main) window. The dock comes
  // back via the window.closed subscription wired in onReady.
  async function detachDock(panelId: string): Promise<void> {
    try {
      await obs.call("window.detach", { dock: panelId });
      // The panel now lives in the detached window; remove it from this one.
      const p = api?.getPanel(panelId);
      if (p) api?.removePanel(p);
    } catch (e) {
      console.log("OBSSHELL: detach failed for " + panelId + ": " + (e as Error).message);
    }
  }

  // Build the mock's studio body (the `isStudio` block): a two-row grid.
  //   top    = canvas-previews row — `preview` (Default canvas) leftmost; the
  //            reconciler adds additional-canvas docks to its RIGHT, side-by-side.
  //   bottom = docks row — Scenes · Sources · Stats · Mixer, left-to-right.
  // The previews row is sized ~1.7x the bottom row (mock `flex:1.7` vs `flex:1`).
  // Multistream/Controls/Transitions are intentionally NOT in the default set
  // (Multistream -> Stream page, Controls -> the GO-LIVE bar, Transitions has no
  // studio home in the mock); they stay registered in DOCKS so the CANVASES-bar
  // restore chips can still reopen them.
  function buildDefaultLayout(dv: DockviewApi): void {
    dv.clear();
    dv.addPanel(panelOptions("preview", detachDock));
    // Bottom docks row, anchored below the previews row, then left-to-right.
    dv.addPanel(panelOptions("scenes", detachDock, { position: { referencePanel: "preview", direction: "below" } }));
    dv.addPanel(panelOptions("sources", detachDock, { position: { referencePanel: "scenes", direction: "right" } }));
    dv.addPanel(panelOptions("stats", detachDock, { position: { referencePanel: "sources", direction: "right" } }));
    dv.addPanel(panelOptions("mixer", detachDock, { position: { referencePanel: "stats", direction: "right" } }));
    sizeStudioRows(dv);
    logDocks(dv);
    refreshVisible();
  }

  // Approximate the mock's 1.7:1 previews-to-docks height split by sizing the
  // top (preview) group; the bottom row's groups share the remaining height. The
  // top-level vertical division persists when the reconciler later adds canvas
  // docks into the previews row. Skipped (default even split) if the host has not
  // been measured yet at build time; retried next frame in that case.
  function sizeStudioRows(dv: DockviewApi): void {
    const apply = (): void => {
      const total = dv.height;
      if (total <= 0) return;
      dv.getPanel("preview")?.api.setSize({ height: Math.round((total * 1.7) / 2.7) });
    };
    apply();
    if (dv.height <= 0) requestAnimationFrame(apply);
  }

  // One log line per dock added, so a headless smoke run can confirm the full
  // default layout was assembled (visual fidelity vs the mock is GUI-owed).
  function logDocks(dv: DockviewApi): void {
    for (const p of dv.panels) {
      console.log("OBSSHELL: dock added -> " + p.id);
    }
    console.log("OBSSHELL: default layout built (" + dv.panels.length + " docks)");
  }

  function refreshVisible(): void {
    if (!api) return;
    const present = new Set(api.panels.map((p) => p.id));
    const next: Record<string, boolean> = {};
    for (const d of DOCKS) next[d.id] = present.has(d.id);
    visibleDocks = next;
    const canvasSet = new Set<string>();
    for (const p of api.panels) {
      if (p.id.startsWith("canvas:")) canvasSet.add(p.id);
    }
    canvasDocksPresent = canvasSet;
  }

  // Coalesce the save bursts Dockview emits while a layout is being assembled
  // (one onDidLayoutChange per addPanel) into a single trailing write.
  let saveTimer: ReturnType<typeof setTimeout> | undefined;
  function persistLayoutSoon(dv: DockviewApi): void {
    clearTimeout(saveTimer);
    saveTimer = setTimeout(() => void layoutStore.save(dv), 250);
  }

  async function onReady(dv: DockviewApi): Promise<void> {
    api = dv;
    dv.onDidLayoutChange(() => {
      refreshVisible();
      persistLayoutSoon(dv);
    });
    // Restore the saved arrangement if one exists; otherwise build the default.
    // fromJSON itself fires onDidLayoutChange, which re-persists the restored
    // (or default) layout, so the on-disk copy stays current.
    const restored = await layoutStore.restore(dv);
    if (restored) {
      console.log("OBSSHELL: layout restored (" + dv.panels.length + " docks)");
      refreshVisible();
    } else {
      buildDefaultLayout(dv);
    }
    // Output-gated per-canvas composite docks (added/removed as canvases enable).
    stopReconciler = startCanvasDockReconciler(dv, detachDock);

    // A detached window closed (explicit redock or user OS-close): restore the
    // dock to the main window. Static docks re-add from DOCKS; dynamic canvas docks
    // are left to the reconciler. Main-window only -- App mounts only here.
    obs.on("window.closed", (p) => {
      if (!api) return;
      if (api.getPanel(p.dock)) return; // already present
      if (p.dock.startsWith("canvas:")) {
        void reconcileCanvasDocks(api, detachDock);
      } else if (DOCKS.some((d) => d.id === p.dock)) {
        api.addPanel(panelOptions(p.dock, detachDock));
      }
      refreshVisible();
    });
  }

  function toggleDock(id: string): void {
    if (!api) return;
    const existing = api.getPanel(id);
    if (existing) {
      api.removePanel(existing);
    } else {
      api.addPanel(panelOptions(id, detachDock));
    }
    refreshVisible();
  }

  function resetLayout(): void {
    if (!api) return;
    // A fresh default shows every output-gated canvas: drop the user-hidden set
    // before rebuilding so eye-hidden canvases reappear.
    clearCanvasUserHidden();
    buildDefaultLayout(api);
    // buildDefaultLayout clears the dynamic canvas docks; re-assert them (the
    // reconciler's event subscriptions stay live but only fire on canvas changes).
    void reconcileCanvasDocks(api, detachDock);
  }

  // The Default canvas maps to the static "preview" dock; a non-default canvas to a
  // reconciler-managed "canvas:<uuid>" panel. Eye-hiding a non-default canvas adds
  // it to the persistent user-hidden set (so the reconciler keeps it removed across
  // re-runs); eye-showing clears it and re-runs the reconciler (which re-adds it
  // only if still output-gated-enabled).
  function toggleCanvasPreview(c: CanvasInfo): void {
    if (!api) return;
    if (c.isDefault) {
      toggleDock("preview");
      return;
    }
    setCanvasUserHidden(c.uuid, canvasShown(c));
    void reconcileCanvasDocks(api, detachDock).then(() => refreshVisible());
  }

  function canvasShown(c: CanvasInfo): boolean {
    return c.isDefault ? !!visibleDocks["preview"] : canvasDocksPresent.has("canvas:" + c.uuid);
  }

  // Inline add-canvas: a name prompt -> canvas.create with the Default canvas's
  // resolution/FPS as defaults (1920x1080@60 if no Default is loaded yet). The new
  // canvas appears in the chip list via the canvas.changed subscription; its dock
  // only materializes once an enabled output binds it (output-gating).
  function addCanvas(): void {
    dialog = {
      kind: "prompt",
      title: "New Canvas",
      confirmLabel: "Create",
      onCommit: (name) => {
        if (!name) return;
        const def = canvases.find((c) => c.isDefault);
        obs
          .call("canvas.create", {
            name,
            baseWidth: def?.baseWidth ?? 1920,
            baseHeight: def?.baseHeight ?? 1080,
            outputWidth: def?.outputWidth,
            outputHeight: def?.outputHeight,
            fpsNum: def?.fpsNum ?? 60,
            fpsDen: def?.fpsDen ?? 1,
          })
          .catch(() => {});
      },
    };
  }

  async function toggleLive(): Promise<void> {
    if (busy || focusedOutputs.length === 0) return;
    busy = true;
    try {
      // No startAll bridge method -- drive each enabled output bound to the focused
      // canvas (the bottom bar is per-focused-canvas). Per-call errors are ignored;
      // the authoritative state arrives via multistream.changed.
      if (anyRunning) {
        for (const o of focusedOutputs) {
          if (o.state === "live" || o.state === "connecting") {
            await obs.call("multistream.stopOutput", { uuid: o.bindingUuid }).catch(() => {});
          }
        }
      } else {
        for (const o of focusedOutputs) {
          await obs.call("multistream.startOutput", { uuid: o.bindingUuid }).catch(() => {});
        }
      }
    } finally {
      busy = false;
    }
  }
</script>

<div class="studio" class:hidden={pageStore.page !== "studio"}>
  <div class="canvases-bar">
    <span class="bar-label">CANVASES</span>

    {#each canvases as c (c.uuid)}
      <div class="chip" class:active={c.uuid === activeCanvasUuid}>
        <button class="chip-main" onclick={() => (focusedCanvasUuid = c.uuid)}>
          <span class="chip-name">{c.name}</span>
          <span class="chip-sub">{c.outputWidth}×{c.outputHeight} · {Math.round(c.fpsNum / c.fpsDen)}</span>
        </button>
        <button
          class="eye"
          title={canvasShown(c) ? "Hide preview" : "Show preview"}
          onclick={() => toggleCanvasPreview(c)}
        >
          {#if canvasShown(c)}
            <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8">
              <path d="M2 12s3.6-6.5 10-6.5S22 12 22 12s-3.6 6.5-10 6.5S2 12 2 12Z" />
              <circle cx="12" cy="12" r="2.4" />
            </svg>
          {:else}
            <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8">
              <path d="M4 4 L20 20" />
              <path
                d="M9.5 5.7A10 10 0 0 1 12 5.5c6.4 0 10 6.5 10 6.5a17 17 0 0 1-2.7 3.4M6.2 7.6A17 17 0 0 0 2 12s3.6 6.5 10 6.5a10 10 0 0 0 3-.45"
              />
            </svg>
          {/if}
        </button>
      </div>
    {/each}

    <button class="add" title="Add canvas" onclick={addCanvas}>+</button>

    <div class="spacer"></div>

    {#each DOCKS as d (d.id)}
      {#if visibleDocks[d.id] === false}
        <button class="restore" onclick={() => toggleDock(d.id)}>
          <span class="restore-plus">+</span>
          {d.title}
        </button>
      {/if}
    {/each}

    <button class="restore reset" onclick={resetLayout}>Reset Layout</button>
  </div>

  <div class="host-area">
    <DockHost {onReady} />
  </div>

  <div class="golive-bar">
    <div class="cluster left">
      <span class="focus-dot" style:background={focusDotColor}></span>
      <span class="focus-name">{focusedLabel}</span>
      {#if liveState === "live"}
        <span class="badge live">● LIVE&nbsp;&nbsp;{fmtDuration(liveDurationMs)}</span>
      {/if}
    </div>

    <div class="cluster right">
      <div class="perf">
        {#each perfRow as e (e.k)}
          <span><span class="pk">{e.k}</span> {e.v}</span>
        {/each}
      </div>
      <button
        class="golive"
        class:running={anyRunning}
        disabled={busy || focusedOutputs.length === 0}
        onclick={() => void toggleLive()}
      >
        {anyRunning ? "■  END STREAM" : "●  GO LIVE"}
      </button>
    </div>
  </div>
</div>

{#if dialog}
  <CollectionDialog {...dialog} onClose={() => (dialog = null)} />
{/if}

<style>
  .studio {
    flex: 1;
    min-height: 0;
    display: flex;
    flex-direction: column;
  }
  .studio.hidden {
    display: none;
  }
  .host-area {
    flex: 1;
    min-height: 0;
  }

  .canvases-bar {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 7px 16px;
    border-bottom: var(--border-weight) solid var(--color-border);
    background: var(--color-surface-2);
    overflow: hidden;
  }
  .bar-label {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.12em;
    color: var(--color-muted);
  }
  .chip {
    flex: 0 0 auto;
    display: flex;
    align-items: stretch;
    gap: 8px;
    padding: 5px 0 5px 10px;
    border: var(--border-weight) solid var(--color-border);
    background: transparent;
    color: var(--color-dim);
  }
  .chip.active {
    border-color: var(--color-accent);
    background: color-mix(in srgb, var(--color-accent) 12%, transparent);
    color: var(--color-accent);
  }
  .chip-main {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 1px;
    background: none;
    border: 0;
    padding: 0;
    cursor: pointer;
    color: inherit;
    font-family: var(--font-ui);
    text-align: left;
  }
  .chip-name {
    font-size: 11px;
    font-weight: 600;
    line-height: 1.1;
  }
  .chip-sub {
    font-family: var(--font-mono);
    font-size: 8px;
    color: var(--color-muted);
    line-height: 1;
  }
  .eye {
    flex: 0 0 auto;
    width: 18px;
    align-self: stretch;
    display: flex;
    align-items: center;
    justify-content: center;
    background: none;
    border: 0;
    border-left: var(--border-weight) solid var(--color-border);
    color: var(--color-muted);
    cursor: pointer;
    font-size: 11px;
  }
  .add {
    flex: 0 0 auto;
    width: 26px;
    height: 34px;
    display: flex;
    align-items: center;
    justify-content: center;
    background: none;
    border: var(--border-weight) dashed var(--color-border);
    color: var(--color-dim);
    cursor: pointer;
    font-size: 15px;
    font-family: var(--font-ui);
  }
  .spacer {
    flex: 1;
  }
  .restore {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    gap: 5px;
    padding: 6px 10px;
    background: none;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-dim);
    cursor: pointer;
    font-size: 10px;
    font-family: var(--font-ui);
  }
  .restore-plus {
    color: var(--color-accent);
  }
  .reset {
    padding: 6px 12px;
  }

  .golive-bar {
    flex: 0 0 auto;
    height: 54px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 0 18px;
    border-top: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .cluster {
    display: flex;
    align-items: center;
  }
  .cluster.left {
    gap: 12px;
  }
  .cluster.right {
    gap: 18px;
  }
  .focus-dot {
    width: 9px;
    height: 9px;
    flex: 0 0 auto;
  }
  .focus-name {
    font-size: 14px;
    font-weight: 600;
    letter-spacing: -0.01em;
    color: var(--color-text);
  }
  .badge.live {
    display: flex;
    align-items: center;
    gap: 7px;
    font-family: var(--font-mono);
    font-size: 11px;
    font-weight: 600;
    color: var(--color-live);
    padding: 3px 9px;
    border: var(--border-weight) solid var(--color-live);
    background: color-mix(in srgb, var(--color-live) 10%, transparent);
  }
  .perf {
    display: flex;
    align-items: center;
    gap: 15px;
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-dim);
  }
  .pk {
    color: var(--color-muted);
  }
  .golive {
    display: flex;
    align-items: center;
    gap: 8px;
    background: var(--color-accent);
    color: var(--color-accent-ink);
    border: 0;
    font-family: var(--font-ui);
    font-size: 12px;
    font-weight: 600;
    letter-spacing: 0.02em;
    padding: 9px 22px;
    cursor: pointer;
  }
  .golive.running {
    background: var(--color-live);
    color: #fff;
  }
  .golive:disabled {
    opacity: 0.5;
    cursor: default;
  }
</style>
