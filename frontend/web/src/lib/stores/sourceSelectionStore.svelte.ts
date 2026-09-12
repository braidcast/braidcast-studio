import { untrack } from "svelte";
import type { SceneItem } from "$lib/api/bridge";

// The source-list selection model for ONE source list (a scene's scene items).
// `SourcesDock` (Default canvas, channel-0 path) drives the exported `sourceSelection`
// singleton; each `CanvasDock` builds its own instance for its per-canvas source list.
// The app-level Delete / Ctrl+C / Ctrl+V handlers reach whichever of those the user last
// clicked in through `activeSurface` (activeSurfaceStore.svelte.ts), not this export.
// The model holds a multi-selection set while keeping `item` = the PRIMARY (anchor /
// last-clicked) member, so single-item readers behave exactly as when only one is
// selected.
export class SourceSelection {
  // The scene the selection belongs to. Reconcile clears the set when this changes,
  // since a selection only makes sense within one scene.
  scene = $state<string | null>(null);

  // Selected scene items, insertion-ordered. Held as objects (not just ids) so `item`
  // and `items` resolve without the owning dock's list; reconcile() refreshes them.
  private _items = $state<SceneItem[]>([]);
  // The anchor is the primary member: the Shift-range pivot and what `item` returns.
  private _anchorId = $state<number | null>(null);

  // Primary (anchor) selection, or null when empty — the single-selection accessor
  // every existing reader relies on. With exactly one selected it is that one item.
  get item(): SceneItem | null {
    return this._items.find((i) => i.id === this._anchorId) ?? null;
  }

  // The full selection set (insertion-ordered). Batch ops loop this.
  get items(): SceneItem[] {
    return this._items;
  }

  get ids(): number[] {
    return this._items.map((i) => i.id);
  }

  get size(): number {
    return this._items.length;
  }

  has(id: number): boolean {
    return this._items.some((i) => i.id === id);
  }

  // Plain click: replace the selection with just this item, which becomes the anchor.
  selectOne(item: SceneItem): void {
    this._items = [item];
    this._anchorId = item.id;
  }

  // Ctrl/Cmd click: add or remove this item; the anchor follows the last touched row
  // (the newly added one, or the last remaining member when the anchor was removed).
  toggle(item: SceneItem): void {
    if (this.has(item.id)) {
      this._items = this._items.filter((i) => i.id !== item.id);
      if (this._anchorId === item.id) {
        this._anchorId = this._items.at(-1)?.id ?? null;
      }
    } else {
      this._items = [...this._items, item];
      this._anchorId = item.id;
    }
  }

  // Shift click: select the anchor→item span (inclusive) over the dock's ordered list.
  // The anchor stays put so a subsequent Shift-click re-pivots from the same origin.
  // With no anchor (or one filtered out of the list) it degrades to a plain click.
  range(item: SceneItem, ordered: SceneItem[]): void {
    const a = ordered.findIndex((i) => i.id === this._anchorId);
    const b = ordered.findIndex((i) => i.id === item.id);
    if (b < 0) {
      return;
    }
    if (a < 0) {
      this.selectOne(item);
      return;
    }
    const [lo, hi] = a <= b ? [a, b] : [b, a];
    this._items = ordered.slice(lo, hi + 1);
  }

  // Replace the whole set at once. This is how a selection made in the PREVIEW
  // (Ctrl-click, rubber band, click-through) arrives: the native surface sends its set
  // in one sceneItem.selected, so the dock adopts it wholesale rather than replaying it
  // as a sequence of clicks. An empty list clears.
  //
  // The anchor is KEPT whenever it is still a member, and only falls back to the last
  // element when it is not. That is the invariant range() documents above -- the pivot
  // has to survive so a second Shift-click still spans from the same origin -- and every
  // dock click echoes back through here, because pushing the set to the preview makes
  // the preview emit it straight back. Taking the last element unconditionally would let
  // that echo walk the anchor to the end of the range the user just made, so a
  // Shift-click on row 2 then row 5 then row 8 would yield 5..8 instead of 2..8.
  setAll(items: SceneItem[]): void {
    this._items = [...items];
    const anchorHeld = this._anchorId != null && items.some((i) => i.id === this._anchorId);
    if (!anchorHeld) {
      this._anchorId = items.at(-1)?.id ?? null;
    }
  }

  clear(): void {
    this._items = [];
    this._anchorId = null;
  }

  // Called by the owning dock whenever it reloads its list: refresh the held objects
  // (a rename/visibility toggle produces fresh SceneItems), drop any selected item that
  // vanished, and clear the whole set on a scene change (per-scene selection).
  reconcile(scene: string | null, list: SceneItem[]): void {
    // The docks call this from a $effect. It reads this._items/_anchorId AND writes
    // them, so tracking those reads would make the effect depend on state it just wrote
    // and re-fire without end (effect_update_depth_exceeded). untrack the body: the
    // effect then tracks only the scene/list it was called with, while the writes still
    // notify the selection's own readers.
    untrack(() => {
      if (scene !== this.scene) {
        this.scene = scene;
        this.clear();
        return;
      }
      const next = this._items
        .map((sel) => list.find((i) => i.id === sel.id))
        .filter((i): i is SceneItem => i != null);
      this._items = next;
      if (this._anchorId != null && !next.some((i) => i.id === this._anchorId)) {
        this._anchorId = next.at(-1)?.id ?? null;
      }
    });
  }
}

export const sourceSelection = new SourceSelection();
