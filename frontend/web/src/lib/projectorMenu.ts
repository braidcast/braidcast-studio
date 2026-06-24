// Shared builder for the projector entries spliced into the docks' context menus.
//
// A projector is a standalone native window that renders a target (program /
// scene / source / canvas) on a monitor; it closes itself (Esc / window close),
// so these menus only OPEN projectors -- there is no close affordance here.
//
// Monitor lists are dynamic, so the per-monitor "Fullscreen Projector" entries
// depend on a runtime display enumeration. Context menus, however, are built
// synchronously in the right-click handler (the dock assigns the item array into
// $state at click time). To bridge that, we cache the monitor list at module
// scope: docks call `prefetchMonitors()` on mount, and `projectorItems(target)`
// reads the cache synchronously when a menu opens. The async `projectorMenuItems`
// is the same builder for callers that can await (it ensures the cache first).
//
// If the monitor enumeration fails (or hasn't resolved yet), we degrade to just
// the "Windowed Projector" entry, which needs no monitor.

import { obs, type Monitor, type ProjectorTarget } from "./bridge";
import type { ContextMenuItem } from "./ContextMenu.svelte";

let monitors: Monitor[] = [];
let inFlight: Promise<Monitor[]> | null = null;

async function fetchMonitors(): Promise<Monitor[]> {
  try {
    const res = await obs.call("display.listMonitors");
    monitors = res?.monitors ?? [];
  } catch (e) {
    console.log("display.listMonitors failed: " + (e as Error).message);
    monitors = [];
  }
  return monitors;
}

/** Warm the monitor cache (idempotent). Call on dock mount so the synchronous
 * `projectorItems` has data by the time a menu opens. */
export function prefetchMonitors(): void {
  if (inFlight) {
    return;
  }
  inFlight = fetchMonitors().finally(() => {
    inFlight = null;
  });
}

function open(target: ProjectorTarget, mode: "fullscreen" | "windowed", monitor?: number) {
  obs
    .call("projector.open", monitor === undefined ? { target, mode } : { target, mode, monitor })
    .catch((e) => console.log("projector.open failed: " + (e as Error).message));
}

/** Build the projector context-menu items for `target` from the cached monitor
 * list (synchronous). ContextMenu has no submenus, so these are flat entries the
 * caller splices into its existing menu: a "Windowed Projector" plus one
 * "Fullscreen Projector -- <name> (WxH)" per monitor. Falls back to just the
 * windowed entry when no monitors are cached yet. */
export function projectorItems(target: ProjectorTarget): ContextMenuItem[] {
  const items: ContextMenuItem[] = [{ label: "Windowed Projector", action: () => open(target, "windowed") }];
  for (const m of monitors) {
    items.push({
      label: `Fullscreen Projector — ${m.name} (${m.width}×${m.height})`,
      action: () => open(target, "fullscreen", m.index),
    });
  }
  return items;
}

/** Async variant matching the spec contract: ensures monitors are loaded, then
 * builds the same flat item list. Use where awaiting before showing the menu is
 * acceptable; docks prefer prefetch + the synchronous `projectorItems`. */
export async function projectorMenuItems(target: ProjectorTarget): Promise<ContextMenuItem[]> {
  if (monitors.length === 0) {
    await (inFlight ?? fetchMonitors());
  }
  return projectorItems(target);
}
