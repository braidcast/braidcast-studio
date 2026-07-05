<script lang="ts">
  import { onDestroy, onMount } from "svelte";
  import type { DockviewApi } from "dockview-core";
  import DockHost from "../dock/DockHost.svelte";
  import { DOCKS, panelOptions } from "../dock/dockRegistry";
  import { layoutStore } from "../dock/layoutStore.svelte";
import { bumpDockLayout } from "../dockLayoutSignal.svelte";
  import {
    startCanvasDockReconciler,
    reconcileCanvasDocks,
    setCanvasUserHidden,
    clearCanvasUserHidden,
  } from "../dock/canvasReconciler";
  import { startBrowserDockReconciler, reconcileBrowserDocks } from "../dock/browserDockReconciler";
  import { setDetachHandler } from "../dock/detachRegistry";
  import { browserDockStore } from "../browserDockStore.svelte";
  import { obs, type CanvasInfo, type Monitor, type MultistreamStatus, type MultistreamState } from "../bridge";
  import { canvasStore } from "../canvasStore.svelte";
  import { STATE_COLOR } from "../theme/stateColors";
  import { fmtDuration, fmtBitrate } from "../format";
  import { callOrToast } from "../callToast";
  import { statsStore } from "../statsStore.svelte";
  import { oauthStore } from "../oauthStore.svelte";
  import { pageStore } from "../pageStore.svelte";
  import { suspendPreview } from "../previewGate.svelte";
  import { undoStore } from "../undoStore.svelte";
  import CollectionDialog, { type DialogSpec } from "../CollectionDialog.svelte";
  import ContextMenu, { type ContextMenuItem } from "../ContextMenu.svelte";
  import { openGoLiveModal } from "../goLiveModalOpener.svelte";
  import { goLivePref } from "../goLivePrefStore.svelte";

  let api = $state<DockviewApi | undefined>(undefined);
  let visibleDocks = $state<Record<string, boolean>>({});
  let canvasDocksPresent = $state<Set<string>>(new Set());
  let stopReconciler: (() => void) | undefined;
  let stopBrowserReconciler: (() => void) | undefined;
  let offWindowClosed: (() => void) | undefined;

  // CANVASES bar + GO-LIVE bar data, read independently of the Dockview lifecycle.
  // The canvas list comes from the shared store (one source of truth).
  let canvases = $derived(canvasStore.canvases);
  let outputs = $state<MultistreamStatus[]>([]);
  // Shared 1 Hz stats poll (also feeds the Stats dock + Monitor page).
  let stats = $derived(statsStore.stats);
  let focusedCanvasUuid = $state<string | null>(null);
  let busy = $state(false);
  let dialog = $state<DialogSpec | null>(null);

  // Go-live confirmation prompts (settings.getGeneral), kept in sync via
  // settings.generalChanged. When set, toggleLive routes through a confirm dialog.
  let warnGoLive = $state(false);
  let warnStop = $state(false);

  // Virtual camera: live state + target canvas (uuid; "" = Default / global
  // program video), kept in sync via virtualCam.changed. The right-click picker
  // anchors at vcamMenuPos when open.
  let vcamActive = $state(false);
  let vcamCanvas = $state("");
  let vcamMenuPos = $state<{ x: number; y: number } | null>(null);

  // Aggregate viewer count (Phase 9.0), fed by the host's viewers.changed push
  // while live; null when offline so the chip stays hidden off-stream.
  let viewerTotal = $state<number | null>(null);

  // CANVASES-bar "⋯" overflow: anchor position (viewport coords) when open, the
  // runtime monitor list for the per-monitor projector entries, and the dock-lock
  // flag. Monitors load once on mount; the set rarely changes.
  let overflowPos = $state<{ x: number; y: number } | null>(null);
  let monitors = $state<Monitor[]>([]);
  let docksLocked = $state(false);

  // The focused canvas drives the bottom bar: default to the active CANVASES chip,
  // else the Default canvas, else the first canvas. `null` only before the list
  // loads. Live state below is scoped to THIS canvas's outputs, not all outputs.
  let activeCanvasUuid = $derived(
    focusedCanvasUuid ?? canvases.find((c) => c.isDefault)?.uuid ?? canvases[0]?.uuid ?? null,
  );
  let focusedOutputs = $derived(outputs.filter((o) => o.canvasUuid === activeCanvasUuid));

  // A focused canvas that was deleted/disabled would otherwise stick the GO-LIVE bar
  // on a non-existent canvas; drop the focus so it falls back to the Default canvas
  // via the activeCanvasUuid derivation. Reacts to the shared store's list.
  $effect(() => {
    const list = canvasStore.canvases;
    if (focusedCanvasUuid && !list.some((c) => c.uuid === focusedCanvasUuid)) {
      focusedCanvasUuid = null;
    }
  });

  // Live state across the FOCUSED canvas's enabled outputs: live wins, then
  // connecting, then error, else idle. Drives the focus dot, live badge, button.
  let liveState = $derived.by<MultistreamState>(() => {
    if (focusedOutputs.some((o) => o.state === "live")) return "live";
    if (focusedOutputs.some((o) => o.state === "connecting")) return "connecting";
    if (focusedOutputs.some((o) => o.state === "error")) return "error";
    return "idle";
  });
  // Authoritative GLOBAL live flag for the GO LIVE button: streaming.changed
  // drives it, seeded once on mount from the initial multistream.status read.
  // (The per-canvas focus dot / LIVE badge above still derive from focusedOutputs.)
  let anyRunning = $state(false);

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
    const net = stats.outputs.length === 0 ? "—" : fmtBitrate(totalKbps);
    return [
      { k: "CPU", v: g.cpu.toFixed(1) + "%" },
      { k: "FPS", v: String(Math.round(g.fps)) },
      { k: "RENDER", v: g.renderLagPct.toFixed(1) + "%" },
      { k: "ENCODE", v: g.encodeSkipPct.toFixed(1) + "%" },
      { k: "NET", v: net },
    ];
  });

  onDestroy(() => {
    stopReconciler?.();
    stopBrowserReconciler?.();
    offWindowClosed?.();
    clearTimeout(saveTimer);
  });

  // Store-change reactivity for the user-defined browser docks. The store is a
  // rune-backed $state (no bridge events), so reconcile here whenever the dock set
  // changes (add/edit/remove) or the api becomes ready. Mirrors the per-canvas dot
  // effect above; the reconcile is idempotent.
  $effect(() => {
    if (!api) return;
    void browserDockStore.docks;
    reconcileBrowserDocks(api);
  });

  // Explicitly suspend every native preview overlay (Default + each CanvasDock,
  // which all hide off the ref-counted previewGate) while Studio is off-page,
  // instead of relying on the ResizeObserver -> 0x0 rect path. Restored on return.
  $effect(() => {
    if (pageStore.page !== "studio") {
      return suspendPreview();
    }
  });

  // Gate the shared 1 Hz stats poll on the Studio page. StudioPage never unmounts
  // (it just hides), so subscribing unconditionally would poll app-wide; this
  // subscribes only while Studio is shown and unsubscribes on leave / destroy. The
  // ref-counted store runs a single interval no matter how many consumers subscribe.
  $effect(() => {
    if (pageStore.page !== "studio") return;
    return statsStore.subscribe();
  });

  // Shared OAuth-account store: gates the Events/Multichat restore chips (and their
  // toggle) so an empty, meaningless panel can't be opened with zero logged-in
  // accounts. Studio-only, mirroring the stats subscription above.
  $effect(() => {
    if (pageStore.page !== "studio") return;
    return oauthStore.subscribe();
  });

  // Docks whose content is meaningless without a logged-in platform account. Their
  // restore chip is hidden and toggleDock refuses to open them while none is connected;
  // an already-open one falls back to its own empty state (handled in the dock body).
  const OAUTH_GATED_DOCKS = new Set(["events", "multichat", "channels"]);
  function dockOpenable(id: string): boolean {
    return !OAUTH_GATED_DOCKS.has(id) || oauthStore.hasAnyConnected;
  }

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
    undoStore.start();
    canvasStore.start();
    const loadStatus = () => {
      obs
        .call("multistream.status")
        .then((res) => (outputs = res.outputs))
        .catch(() => {});
    };
    // Single initial status read seeds BOTH the per-canvas outputs list and the
    // authoritative GLOBAL live flag (any enabled output running anywhere => live).
    // Later transitions arrive via multistream.changed / outputBinding.changed
    // (outputs) and streaming.changed (anyRunning).
    obs
      .call("multistream.status")
      .then((res) => {
        outputs = res.outputs;
        anyRunning = res.outputs.some((o) => o.state === "live" || o.state === "connecting");
      })
      .catch(() => {});
    obs
      .call("settings.getGeneral")
      .then((g) => {
        warnGoLive = g.warnBeforeGoLive;
        warnStop = g.warnBeforeStop;
      })
      .catch(() => {});
    obs
      .call("display.listMonitors")
      .then((res) => (monitors = res?.monitors ?? []))
      .catch((e) => console.log("display.listMonitors failed: " + (e as Error).message));
    obs
      .call("virtualCam.status")
      .then((s) => {
        vcamActive = s.active;
        vcamCanvas = s.canvas;
      })
      .catch(() => {});
    const offMulti = obs.on("multistream.changed", (p) => (outputs = p.outputs));
    const offStreaming = obs.on("streaming.changed", (p) => {
      anyRunning = p.active;
      // The viewer poller stops on stream-stop without a final zero push; clear the
      // chip so a stale count never lingers after going offline.
      if (!p.active) viewerTotal = null;
    });
    const offBindings = obs.on("outputBinding.changed", loadStatus);
    const offViewers = obs.on("viewers.changed", (p) => (viewerTotal = p.total));
    const offVcam = obs.on("virtualCam.changed", (s) => {
      vcamActive = s.active;
      vcamCanvas = s.canvas;
    });
    const offGeneral = obs.on("settings.generalChanged", (g) => {
      warnGoLive = g.warnBeforeGoLive;
      warnStop = g.warnBeforeStop;
    });
    return () => {
      offMulti();
      offStreaming();
      offBindings();
      offViewers();
      offVcam();
      offGeneral();
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
  // Multistream/Transitions are intentionally NOT in the default set (Multistream
  // -> Stream page; Transitions has no studio home in the mock); they stay
  // registered in DOCKS so the CANVASES-bar restore chips can still reopen them.
  // The classic Controls dock was removed entirely -- the GO-LIVE bar (live) and
  // the nav-rail Settings page replace it.
  function buildDefaultLayout(dv: DockviewApi): void {
    dv.clear();
    dv.addPanel(panelOptions("preview"));
    // Bottom docks row, anchored below the previews row, then left-to-right.
    dv.addPanel(panelOptions("scenes", { position: { referencePanel: "preview", direction: "below" } }));
    dv.addPanel(panelOptions("sources", { position: { referencePanel: "scenes", direction: "right" } }));
    dv.addPanel(panelOptions("stats", { position: { referencePanel: "sources", direction: "right" } }));
    dv.addPanel(panelOptions("mixer", { position: { referencePanel: "stats", direction: "right" } }));
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
    // Register the tear-out handler once; the custom tab resolves it at click time
    // (see detachRegistry) so detach survives a layout restore.
    setDetachHandler(detachDock);
    dv.onDidLayoutChange(() => {
      refreshVisible();
      persistLayoutSoon(dv);
      // A reorder/move changes panel POSITIONS without resizing them, so the
      // native-preview docks' ResizeObserver never fires; ping them to re-assert
      // their overlay rect so the native HWNDs follow their slots.
      bumpDockLayout();
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
    stopReconciler = startCanvasDockReconciler(dv);
    // User-defined browser docks (iframe panels). Loads the set + asserts panels;
    // ongoing add/edit/remove reactivity is the $effect above.
    stopBrowserReconciler = startBrowserDockReconciler(dv);

    // A detached window closed (explicit redock or user OS-close): restore the
    // dock to the main window. Static docks re-add from DOCKS; dynamic canvas docks
    // are left to the reconciler. Main-window only -- App mounts only here.
    offWindowClosed = obs.on("window.closed", (p) => {
      if (!api) return;
      if (api.getPanel(p.dock)) return; // already present
      if (p.dock.startsWith("canvas:")) {
        void reconcileCanvasDocks(api);
      } else if (p.dock.startsWith("browserdock:")) {
        reconcileBrowserDocks(api);
      } else if (DOCKS.some((d) => d.id === p.dock)) {
        api.addPanel(panelOptions(p.dock));
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
      // Refuse to open an OAuth-gated dock (Events/Multichat) with no connected
      // account -- it would only show its own empty state.
      if (!dockOpenable(id)) return;
      api.addPanel(panelOptions(id));
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
    void reconcileCanvasDocks(api);
    // Likewise re-assert the user-defined browser docks dropped by the clear.
    reconcileBrowserDocks(api);
  }

  // Dockview 6.6.1 has no `locked` api setter; disabling drag-and-drop via
  // updateOptions is the lock mechanism — panels can't be re-tiled or torn while on.
  function toggleLock(): void {
    docksLocked = !docksLocked;
    api?.updateOptions({ disableDnd: docksLocked });
  }

  // Program projectors (the orphaned View-menu actions): open a standalone native
  // window rendering the program output, windowed or fullscreen on a given monitor.
  function openProgramWindowed(): void {
    obs
      .call("projector.open", { target: { kind: "program" }, mode: "windowed" })
      .catch((e) => console.log("projector.open failed: " + (e as Error).message));
  }
  function openProgramFullscreen(monitor: number): void {
    obs
      .call("projector.open", { target: { kind: "program" }, mode: "fullscreen", monitor })
      .catch((e) => console.log("projector.open failed: " + (e as Error).message));
  }

  // Multiview projector for the focused canvas (empty canvas = the Default canvas's
  // scenes). Renders all of that canvas's scenes in a labeled grid.
  function multiviewCanvas(): string {
    return focusedCanvas?.isDefault ? "" : (activeCanvasUuid ?? "");
  }
  function openMultiviewWindowed(): void {
    obs
      .call("projector.open", { target: { kind: "multiview", canvas: multiviewCanvas() }, mode: "windowed" })
      .catch((e) => console.log("projector.open failed: " + (e as Error).message));
  }
  function openMultiviewFullscreen(monitor: number): void {
    obs
      .call("projector.open", { target: { kind: "multiview", canvas: multiviewCanvas() }, mode: "fullscreen", monitor })
      .catch((e) => console.log("projector.open failed: " + (e as Error).message));
  }

  // CANVASES-bar overflow contents. The program/multiview projector entries are
  // hidden pending the projector redesign (the openProgram*/openMultiview* code
  // paths + monitor enumeration are kept so they can be re-surfaced then); only the
  // dock-lock toggle remains (a leading ✓ marks it on, since ContextMenu items have
  // no native checked state).
  let overflowItems = $derived<(ContextMenuItem | null)[]>([
    { label: (docksLocked ? "✓ " : "") + "Lock Docks", action: toggleLock },
  ]);

  function openOverflow(e: MouseEvent): void {
    const r = (e.currentTarget as HTMLElement).getBoundingClientRect();
    overflowPos = { x: r.left, y: r.bottom + 2 };
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
    void reconcileCanvasDocks(api).then(() => refreshVisible());
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
        void callOrToast(
          "canvas.create",
          {
            name,
            baseWidth: def?.baseWidth ?? 1920,
            baseHeight: def?.baseHeight ?? 1080,
            outputWidth: def?.outputWidth,
            outputHeight: def?.outputHeight,
            fpsNum: def?.fpsNum ?? 60,
            fpsDen: def?.fpsDen ?? 1,
          },
          "Create canvas failed",
        );
      },
    };
  }

  async function doToggleLive(): Promise<void> {
    if (busy) return;
    busy = true;
    try {
      // Single source of truth: start/stop every enabled binding across all canvases.
      // The engine no-ops if nothing is armed; the authoritative live flag arrives
      // back via streaming.changed.
      await obs.call(anyRunning ? "streaming.stop" : "streaming.start");
    } catch (e) {
      console.log("streaming toggle failed: " + (e as Error).message);
    } finally {
      busy = false;
    }
  }

  // Going live routes through the Stream Information modal (trigger option A) when
  // the "Ask for stream info on Go Live" pref is on; the modal pushes metadata then
  // calls streaming.start itself. With the pref off, fall back to the optional
  // warn-confirm + instant start. Stopping keeps the single warn/stop path.
  function toggleLive(): void {
    if (anyRunning && warnStop) {
      dialog = {
        kind: "confirm",
        title: "Stop Stream",
        message: "Stop streaming to all destinations?",
        confirmLabel: "Stop",
        onCommit: () => void doToggleLive(),
      };
      return;
    }
    if (!anyRunning && goLivePref.askStreamInfo) {
      openGoLiveModal("golive");
      return;
    }
    if (!anyRunning && warnGoLive) {
      dialog = {
        kind: "confirm",
        title: "Go Live",
        message: "Start streaming to all enabled destinations?",
        confirmLabel: "Go Live",
        onCommit: () => void doToggleLive(),
      };
      return;
    }
    void doToggleLive();
  }

  // Screenshot the focused canvas's program. The bar is per-focused-canvas (like
  // VCAM/go-live), so it captures activeCanvasUuid; passing the Default canvas's
  // uuid is handled server-side as the global program. A toast confirms the save.
  async function takeScreenshot(): Promise<void> {
    try {
      await obs.call("screenshot.takeProgram", { canvas: activeCanvasUuid ?? "" });
    } catch (e) {
      console.log("screenshot failed: " + (e as Error).message);
    }
  }

  // Virtual camera start/stop. Authoritative state arrives via virtualCam.changed,
  // so don't optimistically flip here.
  async function toggleVcam(): Promise<void> {
    try {
      await obs.call(vcamActive ? "virtualCam.stop" : "virtualCam.start");
    } catch (e) {
      console.log("virtualCam toggle failed: " + (e as Error).message);
    }
  }

  // Target-canvas label for the button tooltip: the canvas matching vcamCanvas, else
  // "Default" (empty target or a uuid that no longer resolves -> global program).
  let vcamTargetName = $derived(canvases.find((c) => c.uuid === vcamCanvas)?.name ?? "Default");

  function openVcamMenu(e: MouseEvent): void {
    e.preventDefault();
    const r = (e.currentTarget as HTMLElement).getBoundingClientRect();
    vcamMenuPos = { x: r.left, y: r.bottom + 2 };
  }

  // One picker entry per canvas; the current target is checked. The Default canvas
  // is in the list already (its uuid resolves to the global program video).
  let vcamMenuItems = $derived<ContextMenuItem[]>(
    canvases.map((c) => ({
      label: c.name,
      checked: c.uuid === vcamCanvas,
      action: () => obs.call("virtualCam.setConfig", { canvas: c.uuid }).catch(() => {}),
    })),
  );
</script>

<div class="studio" class:hidden={pageStore.page !== "studio"}>
  <!-- TOP BAR : CANVASES (variant B — full-height segmented blocks). -->
  <div class="canvases-bar">
    <span class="bar-label">Canvases</span>

    <div class="canvases">
      {#each canvases as c (c.uuid)}
        <div class="chip" data-active={c.uuid === activeCanvasUuid ? "1" : "0"}>
          <button class="chip-main" onclick={() => (focusedCanvasUuid = c.uuid)}>
            <span class="chip-name">{c.name}</span>
            <span class="chip-sub">{c.outputWidth}×{c.outputHeight} · {Math.round(c.fpsNum / c.fpsDen)}</span>
          </button>
          <button
            class="eye"
            class:off={!canvasShown(c)}
            title={canvasShown(c) ? "Hide preview" : "Show preview"}
            onclick={() => toggleCanvasPreview(c)}
          >
            {#if canvasShown(c)}
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6">
                <path d="M1.5 12S5 5.5 12 5.5 22.5 12 22.5 12 19 18.5 12 18.5 1.5 12 1.5 12Z" />
                <circle cx="12" cy="12" r="3" />
              </svg>
            {:else}
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6">
                <path
                  d="M4 4l16 16M9.5 9.6A3 3 0 0014.4 14.5M6.5 6.7C3.5 8.3 1.5 12 1.5 12s3.5 6.5 10.5 6.5c1.7 0 3.2-.4 4.5-1M14 5.8C13.4 5.6 12.7 5.5 12 5.5"
                />
              </svg>
            {/if}
          </button>
        </div>
      {/each}

      <button class="add" title="Add canvas" onclick={addCanvas}>
        <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7">
          <path d="M12 5v14M5 12h14" />
        </svg>
        <span class="add-txt">CANVAS</span>
      </button>
    </div>

    <div class="spacer"></div>

    {#if DOCKS.some((d) => visibleDocks[d.id] === false && dockOpenable(d.id))}
      <div class="restore">
        {#each DOCKS as d (d.id)}
          {#if visibleDocks[d.id] === false && dockOpenable(d.id)}
            <button class="restorechip" onclick={() => toggleDock(d.id)}>
              <svg class="plus" width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                <path d="M12 5v14M5 12h14" />
              </svg>
              {d.title}
            </button>
          {/if}
        {/each}
      </div>
    {/if}

    <div class="util">
      <button
        class="iconbtn"
        title={undoStore.canUndo ? "Undo " + undoStore.undoName : "Undo"}
        aria-label="Undo"
        disabled={!undoStore.canUndo}
        onclick={() => undoStore.undo()}
      >
        <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6">
          <path d="M3 8h11a6 6 0 0 1 0 12H8" />
          <path d="M7 4 3 8l4 4" />
        </svg>
      </button>
      <button
        class="iconbtn"
        title={undoStore.canRedo ? "Redo " + undoStore.redoName : "Redo"}
        aria-label="Redo"
        disabled={!undoStore.canRedo}
        onclick={() => undoStore.redo()}
      >
        <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6">
          <path d="M21 8H10a6 6 0 0 0 0 12h6" />
          <path d="M17 4l4 4-4 4" />
        </svg>
      </button>
      <button class="txtbtn" title="Reset dock layout" onclick={resetLayout}>
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6">
          <path d="M20 11A8 8 0 105.6 6.5M4 3v4h4" />
        </svg>
        Reset Layout
      </button>
      <button class="iconbtn" title="More" aria-label="More studio actions" onclick={openOverflow}>
        <svg width="15" height="15" viewBox="0 0 24 24" fill="currentColor">
          <circle cx="5" cy="12" r="1.7" /><circle cx="12" cy="12" r="1.7" /><circle cx="19" cy="12" r="1.7" />
        </svg>
      </button>
    </div>
  </div>

  <div class="host-area">
    <DockHost {onReady} />
  </div>

  <!-- BOTTOM BAR : GO LIVE (variant B — segmented, GO LIVE as right-edge block). -->
  <div class="golive-bar">
    <div class="bb-left">
      <span class="focus-dot" style:background={focusDotColor}></span>
      <span class="focus-name">{focusedLabel}</span>
      {#if liveState === "live"}
        <span class="livebadge"><span class="rec"></span>LIVE {fmtDuration(liveDurationMs, { fixed: true })}</span>
      {/if}
      {#if anyRunning && viewerTotal !== null}
        <span class="viewers" title="Aggregate viewers across connected platforms">
          <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6">
            <path d="M1.5 12S5 6 12 6s10.5 6 10.5 6-3.5 6-10.5 6S1.5 12 1.5 12Z" />
            <circle cx="12" cy="12" r="2.6" />
          </svg>
          {viewerTotal.toLocaleString()}
        </span>
      {/if}
    </div>

    <div class="bb-spacer"></div>

    <div class="bb-right">
      <button
        class="txtbtn"
        onclick={() => void takeScreenshot()}
        title={"Screenshot " + (focusedCanvas?.name ?? "program") + " (Ctrl+Shift+S captures Default)"}
      >
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6">
          <rect x="3" y="6" width="18" height="14" /><path d="M8 6l1.5-2.5h5L16 6" /><circle cx="12" cy="13" r="3.4" />
        </svg>
        Screenshot
      </button>
      <button
        class="txtbtn vcam"
        class:active={vcamActive}
        onclick={() => void toggleVcam()}
        oncontextmenu={openVcamMenu}
        title={"Virtual Camera → " + vcamTargetName + " (right-click to choose canvas)"}
      >
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6">
          <rect x="3" y="6" width="13" height="12" /><path d="M16 10l5-3v10l-5-3" />
        </svg>
        VCAM
      </button>
      <div class="perf" title="Performance">
        {#each perfRow as e (e.k)}
          <span class="cell"><span class="k">{e.k}</span><span class="v">{e.v}</span></span>
        {/each}
      </div>
      <button
        class="txtbtn editinfo"
        disabled={outputs.length === 0}
        onclick={() => openGoLiveModal("edit")}
        title="Edit stream information (title / category / tags) — works before and during the stream"
      >
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6">
          <path d="M15 4l5 5L8 21H3v-5L15 4Z" /><path d="M13 6l5 5" />
        </svg>
        Edit stream info
      </button>
      <button
        class="golive"
        class:running={anyRunning}
        disabled={busy || (!anyRunning && outputs.length === 0)}
        onclick={() => void toggleLive()}
      >
        {#if anyRunning}
          <svg width="13" height="13" viewBox="0 0 24 24" fill="currentColor"><rect x="5" y="5" width="14" height="14" /></svg>
          END STREAM
        {:else}
          <svg width="13" height="13" viewBox="0 0 24 24" fill="currentColor"><path d="M7 4v16l14-8z" /></svg>
          GO LIVE
        {/if}
      </button>
    </div>
  </div>
</div>

{#if dialog}
  <CollectionDialog {...dialog} onClose={() => (dialog = null)} />
{/if}

{#if overflowPos}
  <ContextMenu x={overflowPos.x} y={overflowPos.y} items={overflowItems} onClose={() => (overflowPos = null)} />
{/if}

{#if vcamMenuPos}
  <ContextMenu x={vcamMenuPos.x} y={vcamMenuPos.y} items={vcamMenuItems} onClose={() => (vcamMenuPos = null)} />
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

  /* ============================================================
     TOP BAR : CANVASES  (mock .topbar, variant B "COMMAND DECK")
     Full-height segmented blocks, zero outer padding, mono-first.
     ============================================================ */
  .canvases-bar {
    flex: 0 0 auto;
    height: 46px;
    display: flex;
    align-items: stretch;
    gap: 0;
    padding: 0;
    min-width: 0;
    border-bottom: var(--border-weight) solid var(--color-border);
    background: var(--color-surface-2);
    overflow: hidden;
  }
  /* "CANVASES" label: surface block w/ a subtle horizontal structural-grid gradient
     (the rgba stripe has no token, so it stays inline like the mock), right hairline. */
  .bar-label {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    height: 100%;
    padding: 0 14px;
    font-family: var(--font-mono);
    font-size: 9px;
    font-weight: 500;
    letter-spacing: 0.14em;
    text-transform: uppercase;
    white-space: nowrap;
    color: var(--color-muted);
    border-right: var(--border-weight) solid var(--color-border);
    background:
      repeating-linear-gradient(
        0deg,
        transparent 0,
        transparent 3px,
        rgba(255, 255, 255, 0.02) 3px,
        rgba(255, 255, 255, 0.02) 4px
      ),
      var(--color-surface);
  }
  /* Canvases segment: chips on --color-base, right hairline divider. */
  .canvases {
    flex: 0 1 auto;
    display: flex;
    align-items: center;
    gap: 6px;
    min-width: 0;
    padding: 0 8px 0 10px;
    border-right: var(--border-weight) solid var(--color-border);
  }
  .chip {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    gap: 8px;
    height: 30px;
    padding: 0 8px 0 10px;
    border: var(--border-weight) solid var(--color-border-2);
    background: var(--color-base);
    color: var(--color-dim);
    white-space: nowrap;
    transition:
      border-color 0.12s,
      background 0.12s;
  }
  .chip:hover {
    border-color: var(--color-muted);
  }
  .chip[data-active="1"] {
    border-color: var(--color-accent);
    background: color-mix(in srgb, var(--color-accent) 12%, transparent);
  }
  .chip-main {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 2px;
    line-height: 1;
    background: none;
    border: 0;
    padding: 0;
    height: auto;
    cursor: pointer;
    color: inherit;
    text-align: left;
  }
  .chip-name {
    font-family: var(--font-mono);
    font-size: 10.5px;
    font-weight: 600;
    letter-spacing: 0.02em;
    text-transform: uppercase;
    color: var(--color-text);
    line-height: 1;
  }
  .chip-sub {
    font-family: var(--font-mono);
    font-size: 8px;
    letter-spacing: 0.05em;
    color: var(--color-muted);
    line-height: 1;
  }
  /* Per-chip eye toggle: kept inside the chip, hairline-separated on the left. */
  .eye {
    flex: 0 0 auto;
    width: 22px;
    height: 22px;
    display: flex;
    align-items: center;
    justify-content: center;
    background: none;
    border: 0;
    border-left: var(--border-weight) solid var(--color-border-2);
    color: var(--color-muted);
    cursor: pointer;
  }
  .eye:hover {
    color: var(--color-dim);
  }
  /* Hidden-preview state: the mock dims the glyph below --color-muted; no token
     for this deep grey, so keep it inline like the mock. */
  .eye.off {
    color: #54545c;
  }
  .chip[data-active="1"] .eye {
    border-left-color: color-mix(in srgb, var(--color-accent) 18%, transparent);
    color: var(--color-accent);
  }
  /* Dashed add-canvas: plus glyph + mono "CANVAS" label. */
  .add {
    flex: 0 0 auto;
    height: 30px;
    min-width: 30px;
    margin-left: 2px;
    padding: 0 8px;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
    background: none;
    border: var(--border-weight) dashed var(--color-border);
    color: var(--color-muted);
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.08em;
    cursor: pointer;
  }
  .add:hover {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  /* Elastic gap that lets the canvas chips sit flush-left and the restore/utility
     clusters pin flush-right so the bar reads edge-to-edge. No border of its own:
     the .canvases segment's right hairline already divides here, and .restore/.util
     draw their own left hairline — a border here just doubled the divider and made
     the empty stretch read as a seam / dead space. */
  .spacer {
    flex: 1;
    min-width: 8px;
  }
  /* Restore-docks segment: bordered chips on --color-base, left hairline divider. */
  .restore {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    gap: 8px;
    height: 100%;
    padding: 0 10px;
    border-left: var(--border-weight) solid var(--color-border);
  }
  .restorechip {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    gap: 6px;
    height: 26px;
    padding: 0 9px;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border-2);
    color: var(--color-muted);
    font-size: 10.5px;
    white-space: nowrap;
    cursor: pointer;
  }
  .restorechip:hover {
    border-color: var(--color-border);
    color: var(--color-dim);
    background: var(--color-surface);
  }
  .restorechip .plus {
    flex: 0 0 auto;
    color: var(--color-accent);
  }
  /* Utility segment: undo / redo / reset / more as bordered blocks, left divider. */
  .util {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    gap: 4px;
    height: 100%;
    padding: 0 8px;
    border-left: var(--border-weight) solid var(--color-border);
  }
  .iconbtn {
    flex: 0 0 auto;
    width: 28px;
    height: 28px;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 0;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-dim);
    cursor: pointer;
    transition:
      border-color 0.12s,
      color 0.12s,
      background 0.12s;
  }
  .iconbtn:hover:not(:disabled) {
    border-color: var(--color-muted);
    color: var(--color-text);
    background: var(--color-surface-2);
  }
  .iconbtn:disabled {
    color: #4a4a52;
    cursor: default;
  }
  .txtbtn {
    flex: 0 0 auto;
    height: 28px;
    display: flex;
    align-items: center;
    gap: 7px;
    padding: 0 11px;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-dim);
    font-family: var(--font-ui);
    font-size: 11px;
    white-space: nowrap;
    cursor: pointer;
    transition:
      border-color 0.12s,
      color 0.12s,
      background 0.12s;
  }
  .txtbtn:hover:not(:disabled) {
    border-color: var(--color-muted);
    color: var(--color-text);
    background: var(--color-surface-2);
  }

  /* ============================================================
     BOTTOM BAR : GO LIVE  (mock .bottombar, variant B)
     Segmented; GO LIVE is a full-height right-edge block.
     ============================================================ */
  .golive-bar {
    flex: 0 0 auto;
    height: 60px;
    display: flex;
    align-items: stretch;
    gap: 0;
    padding: 0;
    border-top: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  /* Left cluster: focus dot + label (+ live badge / viewers when live), right
     hairline, on --color-surface. */
  .bb-left {
    display: flex;
    align-items: center;
    gap: 14px;
    min-width: 0;
    padding: 0 14px;
    background: var(--color-surface);
    border-right: var(--border-weight) solid var(--color-border);
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
    white-space: nowrap;
  }
  .livebadge {
    display: flex;
    align-items: center;
    gap: 7px;
    height: 24px;
    padding: 0 9px;
    font-family: var(--font-mono);
    font-size: 11px;
    font-weight: 500;
    letter-spacing: 0.06em;
    color: var(--color-live);
    white-space: nowrap;
    border: var(--border-weight) solid color-mix(in srgb, var(--color-live) 55%, transparent);
    background: color-mix(in srgb, var(--color-live) 14%, transparent);
  }
  .livebadge .rec {
    width: 7px;
    height: 7px;
    flex: 0 0 auto;
    background: var(--color-live);
    animation: pulse 1.6s ease-in-out infinite;
  }
  @keyframes pulse {
    0%,
    100% {
      opacity: 1;
    }
    50% {
      opacity: 0.35;
    }
  }
  .viewers {
    display: flex;
    align-items: center;
    gap: 6px;
    height: 24px;
    padding: 0 9px;
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-dim);
    white-space: nowrap;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface-2);
  }
  .viewers svg {
    color: var(--color-muted);
  }
  /* Base-colored spacer between the clusters. */
  .bb-spacer {
    flex: 1;
    min-width: 8px;
    background: var(--color-base);
  }
  /* Right cluster: text buttons + perf readout + GO LIVE, all gapless full-height. */
  .bb-right {
    display: flex;
    align-items: stretch;
    gap: 0;
  }
  /* Text buttons (Screenshot / VCAM / Edit): full-height surface blocks, only a
     left hairline, surface-2 hover. */
  .bb-right .txtbtn {
    height: auto;
    border: 0;
    border-left: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .bb-right .txtbtn:hover:not(:disabled) {
    background: var(--color-surface-2);
    color: var(--color-text);
  }
  .vcam.active {
    color: var(--meter-green);
    border-left-color: var(--meter-green);
  }
  .vcam.active:hover:not(:disabled) {
    color: var(--meter-green);
  }
  .editinfo:disabled {
    opacity: 0.5;
    cursor: default;
  }
  /* Perf readout: bordered surface segment, centered cells with internal dividers. */
  .perf {
    display: flex;
    align-items: center;
    gap: 0;
    font-family: var(--font-mono);
    font-size: 11px;
    letter-spacing: 0.02em;
    white-space: nowrap;
    background: var(--color-surface);
    border-left: var(--border-weight) solid var(--color-border);
    border-right: var(--border-weight) solid var(--color-border);
  }
  .perf .cell {
    display: flex;
    align-items: baseline;
    gap: 5px;
    padding: 0 10px;
  }
  .perf .cell + .cell {
    border-left: var(--border-weight) solid var(--color-border-2);
  }
  .perf .k {
    color: var(--color-muted);
  }
  .perf .v {
    color: var(--color-dim);
  }
  /* GO LIVE: full-height right-edge accent block; red END STREAM when live. The
     global `button { height: var(--control-height) }` would otherwise pin it to 30px
     and float it in the 60px bar — height:auto + align-self:stretch make it fill the
     bar edge-to-edge like the Screenshot/VCAM/perf/Edit cells beside it. */
  .golive {
    height: auto;
    align-self: stretch;
    display: flex;
    align-items: center;
    gap: 9px;
    padding: 0 26px;
    background: var(--color-accent);
    color: var(--color-accent-ink);
    border: 0;
    border-left: var(--border-weight) solid var(--color-accent);
    font-family: var(--font-mono);
    font-size: 13px;
    font-weight: 600;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    cursor: pointer;
    transition: filter 0.12s;
  }
  .golive:hover:not(:disabled) {
    filter: brightness(1.08);
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
