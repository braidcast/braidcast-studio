import { obs, type CanvasInfo } from "../bridge";
import type { DockviewApi } from "dockview-core";

// Output-gated composite canvas docks. A non-default canvas gets ONE inline dock
// (preview + its own scenes + sources, see CanvasDock.svelte) only while >=1
// enabled output binds it (CanvasInfo.enabled). This reconciler diffs the wanted
// set against the live Dockview panels and adds/removes/renames canvas docks,
// re-running on canvas.changed / outputBinding.changed. New canvas docks tab-stack
// in the center with the Default preview (center-modes.html "tabs"); the user can
// then drag to split/takeover/float.

const PANEL_PREFIX = "canvas:";

function panelId(uuid: string): string {
  return PANEL_PREFIX + uuid;
}

export function startCanvasDockReconciler(api: DockviewApi, detachDock: (panelId: string) => void): () => void {
  async function reconcile(): Promise<void> {
    let canvases: CanvasInfo[];
    try {
      canvases = await obs.call("canvas.list");
    } catch {
      return; // transient; the next event re-runs it
    }
    const wanted = canvases.filter((c) => !c.isDefault && c.enabled);
    const wantedIds = new Set(wanted.map((c) => panelId(c.uuid)));

    // Remove canvas docks that are no longer wanted (disabled or deleted).
    for (const p of api.panels) {
      if (p.id.startsWith(PANEL_PREFIX) && !wantedIds.has(p.id)) {
        api.removePanel(p);
      }
    }

    // Add docks for newly-enabled canvases; rename existing ones in place.
    for (const c of wanted) {
      const id = panelId(c.uuid);
      const existing = api.getPanel(id);
      if (existing) {
        existing.api.setTitle("Canvas · " + c.name);
        continue;
      }
      const preview = api.getPanel("preview");
      api.addPanel({
        id,
        component: "canvas",
        title: "Canvas · " + c.name,
        params: { canvasUuid: c.uuid, canvasName: c.name, __accent: true, __detach: detachDock },
        position: preview ? { referencePanel: "preview", direction: "within" } : undefined,
      });
    }
  }

  void reconcile();
  const offCanvas = obs.on("canvas.changed", () => void reconcile());
  const offBindings = obs.on("outputBinding.changed", () => void reconcile());
  return () => {
    offCanvas();
    offBindings();
  };
}
