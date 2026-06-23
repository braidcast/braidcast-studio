<script lang="ts">
  import { obs } from "./lib/bridge";
  import MenuBar from "./lib/MenuBar.svelte";
  import DockHost from "./lib/dock/DockHost.svelte";
  import { DOCKS, panelOptions } from "./lib/dock/dockRegistry";
  import { layoutStore } from "./lib/dock/layoutStore.svelte";
  import { startCanvasDockReconciler, reconcileCanvasDocks } from "./lib/dock/canvasReconciler";
  import { onDestroy } from "svelte";
  import { themeStore } from "./lib/theme/themeStore.svelte";
  import SettingsModal from "./lib/SettingsModal.svelte";
  import { settingsOpener, closeSettings } from "./lib/settingsOpener.svelte";
  import ThemeEditor from "./lib/ThemeEditor.svelte";
  import { themeEditorOpener, closeThemeEditor } from "./lib/themeEditorOpener.svelte";
  import type { DockviewApi } from "dockview-core";

  let version = $state("…");
  let api = $state<DockviewApi | undefined>(undefined);
  let visibleDocks = $state<Record<string, boolean>>({});
  let locked = $state(false);
  let stopReconciler: (() => void) | undefined;

  onDestroy(() => stopReconciler?.());

  // Apply the saved (or default Industrial) theme before first paint settles.
  void themeStore.hydrate();

  $effect(() => {
    obs
      .call("getVersion")
      .then((v) => (version = v || "unknown"))
      .catch((e) => (version = "error: " + (e as Error).message));
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

  // Build the dock-model.html default arrangement:
  //   center column = Preview (top) over a bottom row of
  //     Scenes · Sources · Audio Mixer · Transitions · Controls (Controls rightmost)
  //   right rail   = Multistream, right of the center. Non-default canvas docks are
  //     added at runtime by the reconciler (tab-stacked into the center preview).
  function buildDefaultLayout(dv: DockviewApi): void {
    dv.clear();
    dv.addPanel(panelOptions("preview", detachDock));
    // Bottom row, left-to-right, anchored below the preview.
    dv.addPanel(panelOptions("scenes", detachDock, { position: { referencePanel: "preview", direction: "below" } }));
    dv.addPanel(panelOptions("sources", detachDock, { position: { referencePanel: "scenes", direction: "right" } }));
    dv.addPanel(panelOptions("mixer", detachDock, { position: { referencePanel: "sources", direction: "right" } }));
    dv.addPanel(
      panelOptions("transitions", detachDock, { position: { referencePanel: "mixer", direction: "right" } }),
    );
    dv.addPanel(
      panelOptions("controls", detachDock, { position: { referencePanel: "transitions", direction: "right" } }),
    );
    // Right rail spanning the height, right of the preview/center column.
    dv.addPanel(
      panelOptions("multistream", detachDock, { position: { referencePanel: "preview", direction: "right" } }),
    );
    logDocks(dv);
    refreshVisible();
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
    buildDefaultLayout(api);
    // buildDefaultLayout clears the dynamic canvas docks; re-assert them (the
    // reconciler's event subscriptions stay live but only fire on canvas changes).
    void reconcileCanvasDocks(api, detachDock);
  }

  // Dockview 6.6.1 has no `locked` api setter; disabling drag-and-drop via
  // updateOptions is the lock mechanism — panels can't be re-tiled or torn while on.
  function toggleLock(): void {
    locked = !locked;
    api?.updateOptions({ disableDnd: locked });
  }
</script>

<div class="shell">
  <MenuBar {visibleDocks} {toggleDock} {resetLayout} {locked} {toggleLock} />
  <div class="host-area">
    <DockHost {onReady} />
  </div>
  <footer class="statusbar"><span>libobs {version}</span></footer>
</div>

{#if settingsOpener.open}
  <SettingsModal initialTab={settingsOpener.tab} editCanvas={settingsOpener.editCanvas} onClose={closeSettings} />
{/if}

{#if themeEditorOpener.open}
  <ThemeEditor onClose={closeThemeEditor} />
{/if}

<style>
  .shell {
    display: flex;
    flex-direction: column;
    height: 100%;
    background: var(--color-base);
  }
  .host-area {
    flex: 1;
    min-height: 0;
  }
  .statusbar {
    flex: 0 0 auto;
    border-top: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
    padding: 4px 12px;
    font-family: var(--font-ui);
    font-size: 10px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    color: var(--color-muted);
  }
</style>
