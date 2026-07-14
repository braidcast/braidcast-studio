import type { DockviewApi } from "dockview-core";
import { browserDockStore } from "$lib/stores/browserDockStore.svelte";
import { SIDE_DOCK_WIDTH } from "$lib/docking/dockRegistry";

// User-defined browser docks. Each stored {id,title,url} maps to ONE Dockview
// panel hosting an <iframe> (see BrowserDock.svelte). This reconciler diffs the
// wanted set (the store) against the live panels and adds/removes/updates them,
// mirroring canvasReconciler.ts.
//
// Open/closed model: every stored browser dock is shown as a panel — add = open +
// persist, remove = close + delete. There is no separate open/closed state.
//
// KEY DESIGN (same as the canvas reconciler): panels are reconciled FROM THE STORE
// by id, so url/title ride in params freshly each pass. A layout-restore that
// brings back a `browserdock:<id>` panel gets its url re-supplied from the store by
// id here — sidestepping the "saved layout lacks params" problem.
//
// Reactivity: unlike canvasReconciler (driven by obs.on events), the store is a
// rune-backed $state with no bridge events, so the store-change trigger lives in
// StudioPage's $effect (reads browserDockStore.docks -> reconcile). This module's
// start function only kicks off the initial load + assertion.

const PANEL_PREFIX = "browserdock:";

function panelId(id: string): string {
  return PANEL_PREFIX + id;
}

// Reassert the wanted browser-dock set against the live panels. Idempotent: safe on
// boot, on every store mutation, and after a layout reset/restore.
export function reconcileBrowserDocks(api: DockviewApi): void {
  const wanted = browserDockStore.docks;
  const wantedIds = new Set(wanted.map((d) => panelId(d.id)));

  // Remove panels for deleted docks.
  for (const p of api.panels) {
    if (p.id.startsWith(PANEL_PREFIX) && !wantedIds.has(p.id)) {
      api.removePanel(p);
    }
  }

  // Add panels for new docks; refresh title + url params on existing ones (an edit
  // re-supplies the url so a renamed/retargeted dock updates in place).
  let refId = "preview";
  for (const d of wanted) {
    const id = panelId(d.id);
    const title = d.title || "Browser";
    const existing = api.getPanel(id);
    if (existing) {
      existing.api.setTitle(title);
      existing.api.updateParameters({ url: d.url, title });
      refId = id;
      continue;
    }
    const hasAnchor = api.getPanel(refId) !== undefined;
    api.addPanel({
      id,
      component: "browserdock",
      title,
      params: { url: d.url, title },
      position: hasAnchor ? { referencePanel: refId, direction: "right" } : undefined,
      initialWidth: SIDE_DOCK_WIDTH,
    });
    refId = id;
  }
}

export function startBrowserDockReconciler(api: DockviewApi): () => void {
  // Load, THEN assert the set. Reconciling before the store loads would diff the
  // wanted set against an empty list and remove `browserdock:` panels a layout
  // restore just recreated. The ongoing store-change reactivity is owned by
  // StudioPage's $effect (also gated on browserDockStore.loaded), since the store is
  // rune-based, not event-based.
  void browserDockStore.load().then(() => reconcileBrowserDocks(api));
  return () => {};
}
