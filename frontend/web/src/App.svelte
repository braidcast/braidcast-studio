<script lang="ts">
  import { obs } from "./lib/bridge";
  import MenuBar from "./lib/MenuBar.svelte";
  import DockHost from "./lib/dock/DockHost.svelte";
  import { DOCKS, panelOptions } from "./lib/dock/dockRegistry";
  import { layoutStore } from "./lib/dock/layoutStore.svelte";
  import { startCanvasDockReconciler } from "./lib/dock/canvasReconciler";
  import { themeStore } from "./lib/theme/themeStore.svelte";
  import SettingsModal from "./lib/SettingsModal.svelte";
  import { settingsOpener, closeSettings } from "./lib/settingsOpener.svelte";
  import type { DockviewApi } from "dockview-core";

  let version = $state("…");
  let api = $state<DockviewApi | undefined>(undefined);
  let visibleDocks = $state<Record<string, boolean>>({});
  let locked = $state(false);

  // Apply the saved (or default Industrial) theme before first paint settles.
  void themeStore.hydrate();

  $effect(() => {
    obs
      .call("getVersion")
      .then((v) => (version = v || "unknown"))
      .catch((e) => (version = "error: " + (e as Error).message));
  });

  // Single tear-out seam. P1 defers real floating-window detach to next phase
  // (see the plan's floating-scope decision); the ⧉ affordance routes here.
  function detachDock(panelId: string): void {
    console.log("OBSSHELL: detach requested for " + panelId + " (wired next phase)");
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
    startCanvasDockReconciler(dv, detachDock);
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
    if (api) buildDefaultLayout(api);
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
