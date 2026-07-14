// Shared pointer drag-to-reorder for the master lists (Canvases page + Streams tab).
// The row markup differs per page but the interaction is identical, so both create
// one controller and apply its `row` action to each row. Pointer-based (not native
// HTML5 DnD) for CEF smoothness; a movement threshold disambiguates a plain
// click-select from a drag-reorder so selection is never hijacked.

import type { Action } from "svelte/action";

const THRESHOLD = 5;

interface ReorderOptions {
  // Live ordered id list (reactive getter) — its length defines the row count.
  getIds: () => string[];
  // Persist the new order: the caller does the optimistic store update + bridge call.
  commit: (order: string[]) => void;
}

class ReorderController {
  dragging = $state(false);
  // Insertion index in original-list coordinates (0..n); -1 when idle.
  dropIndex = $state(-1);

  #getIds: () => string[];
  #commit: (order: string[]) => void;
  #nodes = new Map<HTMLElement, number>();
  #fromIndex = -1;
  #startX = 0;
  #startY = 0;
  #pointerId = -1;
  #justDragged = false;

  constructor(opts: ReorderOptions) {
    this.#getIds = opts.getIds;
    this.#commit = opts.commit;
  }

  // The index currently being dragged (for a per-row "lifting" style), else -1.
  get dragIndex(): number {
    return this.dragging ? this.#fromIndex : -1;
  }

  // Row onclick calls this first: true means a drag just finished, so suppress the
  // click (don't select). Clears the flag so the next genuine click selects.
  consumeClick(): boolean {
    const v = this.#justDragged;
    this.#justDragged = false;
    return v;
  }

  #computeDrop(y: number): number {
    const rects = [...this.#nodes.entries()]
      .map(([el, index]) => {
        const r = el.getBoundingClientRect();
        return { index, mid: r.top + r.height / 2 };
      })
      .sort((a, b) => a.index - b.index);
    for (const r of rects) {
      if (y < r.mid) {
        return r.index;
      }
    }
    return rects.length;
  }

  #drop(): void {
    const ids = this.#getIds();
    const from = this.#fromIndex;
    let to = this.dropIndex;
    if (from < 0 || to < 0 || from >= ids.length) {
      return;
    }
    // Dropping just before/after its own slot is a no-op.
    if (to === from || to === from + 1) {
      return;
    }
    const next = ids.slice();
    const [moved] = next.splice(from, 1);
    // Removing `from` shifts every index above it down by one.
    if (to > from) {
      to -= 1;
    }
    next.splice(to, 0, moved);
    this.#commit(next);
  }

  row: Action<HTMLElement, number> = (node, indexParam) => {
    let index = indexParam;
    this.#nodes.set(node, index);

    const move = (e: PointerEvent): void => {
      if (!this.dragging) {
        const dx = e.clientX - this.#startX;
        const dy = e.clientY - this.#startY;
        if (Math.hypot(dx, dy) <= THRESHOLD) {
          return;
        }
        this.dragging = true;
        this.#justDragged = true;
      }
      this.dropIndex = this.#computeDrop(e.clientY);
    };
    const up = (): void => {
      node.removeEventListener("pointermove", move);
      node.removeEventListener("pointerup", up);
      node.removeEventListener("pointercancel", up);
      if (node.hasPointerCapture(this.#pointerId)) {
        node.releasePointerCapture(this.#pointerId);
      }
      if (this.dragging) {
        this.#drop();
      }
      this.dragging = false;
      this.dropIndex = -1;
      this.#fromIndex = -1;
    };
    const down = (e: PointerEvent): void => {
      if (e.button !== 0) {
        return;
      }
      this.#fromIndex = index;
      this.#startX = e.clientX;
      this.#startY = e.clientY;
      this.#pointerId = e.pointerId;
      this.#justDragged = false;
      node.setPointerCapture(e.pointerId);
      node.addEventListener("pointermove", move);
      node.addEventListener("pointerup", up);
      node.addEventListener("pointercancel", up);
    };

    node.addEventListener("pointerdown", down);

    return {
      update: (newIndex: number): void => {
        index = newIndex;
        this.#nodes.set(node, newIndex);
      },
      destroy: (): void => {
        node.removeEventListener("pointerdown", down);
        node.removeEventListener("pointermove", move);
        node.removeEventListener("pointerup", up);
        node.removeEventListener("pointercancel", up);
        this.#nodes.delete(node);
      },
    };
  };
}

export function createReorder(opts: ReorderOptions): ReorderController {
  return new ReorderController(opts);
}

export type { ReorderController };
