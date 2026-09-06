// The scene-list reorder affordances shared by the global Scenes dock (Default
// canvas) and each CanvasDock's per-canvas scene list: drag-to-reorder, the "Order"
// context submenu, and the Move up / Move down toolbar pair. The two docks render
// different markup and address different canvases, but the interaction, the labels
// and the edge predicates are identical and both end in the same bridge call
// (`scenes.reorder`), so each shape lives here once instead of once per dock.
//
// Not shared here: the `obs.call("scenes.reorder", …)` wrappers themselves. The two
// docks differ in whether they send `canvas` AND in how they surface an error
// (ScenesDock clears its inline `actionError` before each call; CanvasDock reports
// through its DockError), so folding them together would change one dock's
// behaviour to suit the other.

import type { ReorderDirection } from "$lib/api/bridge";
import type { ToolAction } from "$lib/docking/ListToolbar.svelte";
import type { ContextMenuItems } from "$lib/menus/ContextMenu.svelte";

// One scene's position within the FULL (unfiltered) list, plus how to move it.
// Both helpers below take this so their disabled predicates cannot drift apart.
export interface SceneOrderTarget {
  // Index in the full list; -1 when the scene is absent (or nothing is selected).
  idx: number;
  // Length of the full list, for the bottom-edge predicate.
  count: number;
  // True while a name filter is active: every entry is disabled, since an index
  // only means something against the full ordering.
  disabled: boolean;
  // Perform the relative move.
  move: (direction: ReorderDirection) => void;
}

const atTop = (t: SceneOrderTarget): boolean => t.disabled || t.idx <= 0;
const atBottom = (t: SceneOrderTarget): boolean => t.disabled || t.idx < 0 || t.idx >= t.count - 1;

// The "Order" submenu: Up / Down / Top / Bottom, disabled at the matching edge.
export function sceneOrderMenuChildren(t: SceneOrderTarget): ContextMenuItems {
  return [
    { label: "Up", disabled: atTop(t), action: () => t.move("up") },
    { label: "Down", disabled: atBottom(t), action: () => t.move("down") },
    { label: "Top", disabled: atTop(t), action: () => t.move("top") },
    { label: "Bottom", disabled: atBottom(t), action: () => t.move("bottom") },
  ];
}

// The bottom toolbar's Move up / Move down pair. A dock with further right-hand
// actions (the Scenes dock's grid/list toggle) spreads these and appends its own.
export function sceneMoveActions(t: SceneOrderTarget): ToolAction[] {
  return [
    { icon: "up", title: "Move up", disabled: atTop(t), onClick: () => t.move("up") },
    { icon: "down", title: "Move down", disabled: atBottom(t), onClick: () => t.move("down") },
  ];
}

interface SceneDragOptions {
  // Perform the move: the owner's `scenes.reorder { name, to }` call.
  move: (name: string, to: number) => void;
  // The dragged scene's CURRENT index in the full list, resolved at drop time
  // rather than captured at dragstart: a `scenes.changed` event can rebuild the
  // list mid-drag, and a stale index would either move the wrong row or make the
  // no-op check swallow a real drop. -1 when the scene is gone.
  indexOf: (name: string) => number;
  // True while the list is filtered; blocks the whole interaction.
  disabled?: () => boolean;
}

// Native HTML5 drag-to-reorder, rather than listReorder.svelte.ts's pointer
// capture: that controller's contract is a whole-list `commit(order)` for the
// master lists, while these lists move ONE named row to an index and let the host
// re-list from the bridge.
//
// Indices are the row's position in the rendered list. That equals its position in
// the full ordering because dragging is disabled while a name filter is active.
export class SceneDragReorder {
  // The row being dragged, or null when no drag is in flight.
  dragName = $state<string | null>(null);
  // The row the pointer is currently over; the drop-indicator styling reads this.
  dragOverIdx = $state<number | null>(null);

  #move: (name: string, to: number) => void;
  #indexOf: (name: string) => number;
  #disabled: () => boolean;

  constructor(options: SceneDragOptions) {
    this.#move = options.move;
    this.#indexOf = options.indexOf;
    this.#disabled = options.disabled ?? (() => false);
  }

  // True for the row that should show the drop indicator (never the dragged row).
  isDropTarget(idx: number, name: string): boolean {
    return this.dragOverIdx === idx && this.dragName !== null && this.dragName !== name;
  }

  onDragStart = (e: DragEvent, name: string): void => {
    if (this.#disabled()) {
      return;
    }
    this.dragName = name;
    if (e.dataTransfer) {
      e.dataTransfer.effectAllowed = "move";
      e.dataTransfer.setData("text/plain", name); // Firefox requires data
    }
  };

  onDragOver = (e: DragEvent, idx: number): void => {
    if (this.dragName === null) {
      return;
    }
    e.preventDefault(); // mark this a valid drop target
    if (e.dataTransfer) {
      e.dataTransfer.dropEffect = "move";
    }
    this.dragOverIdx = idx;
  };

  onDrop = (e: DragEvent, idx: number): void => {
    e.preventDefault();
    const name = this.dragName;
    this.#reset();
    if (name === null) {
      return;
    }
    const from = this.#indexOf(name);
    if (from < 0 || from === idx) {
      return;
    }
    this.#move(name, idx);
  };

  onDragEnd = (): void => {
    this.#reset();
  };

  #reset(): void {
    this.dragName = null;
    this.dragOverIdx = null;
  }
}
