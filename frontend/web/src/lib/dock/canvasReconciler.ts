import { obs, type CanvasInfo } from "../bridge";
import type { DockviewApi } from "dockview-core";
import { SIDE_DOCK_WIDTH } from "./dockRegistry";

// Output-gated composite canvas docks. A non-default canvas gets ONE inline dock
// (preview + its own scenes + sources, see CanvasDock.svelte) only while >=1
// enabled output binds it (CanvasInfo.enabled). This reconciler diffs the wanted
// set against the live Dockview panels and adds/removes/renames canvas docks,
// re-running on canvas.changed / outputBinding.changed. New canvas docks sit in
// the previews row to the RIGHT of the Default `preview` (the mock's side-by-side
// canvas previews); the user can then drag to split/takeover/float.

const PANEL_PREFIX = "canvas:";

function panelId(uuid: string): string {
  return PANEL_PREFIX + uuid;
}

// User "hidden" set: a per-canvas suppression layered ON TOP of output-gating.
// A canvas dock shows only when it is output-gated-enabled AND not user-hidden.
// Output-gating still removes a dock when its outputs are disabled; user-hide is
// an additional, persistent suppression that survives reconciler re-runs (the
// eye-toggle in the CANVASES bar flips membership) until the user re-shows it or
// the layout is reset. Module state so every reconcile pass reads the same set.
const userHidden = new Set<string>();

export function setCanvasUserHidden(uuid: string, hidden: boolean): void {
  if (hidden) {
    userHidden.add(uuid);
  } else {
    userHidden.delete(uuid);
  }
}

export function clearCanvasUserHidden(): void {
  userHidden.clear();
}

// Reassert the wanted canvas-dock set against the live panels. Safe to call any
// time (idempotent): on boot, on canvas.changed/outputBinding.changed, and after a
// layout reset/restore that may have cleared the dynamic docks.
export async function reconcileCanvasDocks(api: DockviewApi): Promise<void> {
  let canvases: CanvasInfo[];
  try {
    canvases = await obs.call("canvas.list");
  } catch {
    return; // transient; the next event re-runs it
  }
  const wanted = canvases.filter((c) => !c.isDefault && c.enabled && !userHidden.has(c.uuid));
  const wantedIds = new Set(wanted.map((c) => panelId(c.uuid)));

  // Remove canvas docks that are no longer wanted (disabled or deleted).
  for (const p of api.panels) {
    if (p.id.startsWith(PANEL_PREFIX) && !wantedIds.has(p.id)) {
      api.removePanel(p);
    }
  }

  // Add docks for newly-enabled canvases; rename existing ones in place. Each new
  // dock is placed to the RIGHT of the previous previews-row panel (the Default
  // `preview`, or the last canvas dock already added this pass), so the row stays
  // ordered left-to-right per the mock. `refId` tracks that anchor across the
  // iteration; a missing anchor (preview closed/detached) falls back to default
  // placement. Each additional canvas owns its scenes/sources -> `OWN S/S` badge.
  let refId = "preview";
  for (const c of wanted) {
    const id = panelId(c.uuid);
    const existing = api.getPanel(id);
    if (existing) {
      existing.api.setTitle("Canvas · " + c.name);
      refId = id;
      continue;
    }
    const hasAnchor = api.getPanel(refId) !== undefined;
    api.addPanel({
      id,
      component: "canvas",
      title: "Canvas · " + c.name,
      params: {
        canvasUuid: c.uuid,
        canvasName: c.name,
        __accent: true,
        __dot: "var(--color-muted)",
        __badge: "OWN S/S",
      },
      position: hasAnchor ? { referencePanel: refId, direction: "right" } : undefined,
      initialWidth: SIDE_DOCK_WIDTH,
    });
    refId = id;
  }
}

export function startCanvasDockReconciler(api: DockviewApi): () => void {
  void reconcileCanvasDocks(api);
  const offCanvas = obs.on("canvas.changed", () => void reconcileCanvasDocks(api));
  const offBindings = obs.on("outputBinding.changed", () => void reconcileCanvasDocks(api));
  return () => {
    offCanvas();
    offBindings();
  };
}
