import { untrack } from "svelte";
import type { ObsEvents, SceneItem, SceneItemRef } from "$lib/api/bridge";

// Whether two refs name the same scene item. An id is unique only within its owner, so
// the owning group is part of the identity; an absent group means top level.
export function sameItem(a: SceneItemRef, b: SceneItemRef): boolean {
  return a.id === b.id && (a.group ?? null) === (b.group ?? null);
}

function refOf(item: SceneItem | undefined): SceneItemRef | null {
  return item ? { id: item.id, group: item.group } : null;
}

// The row a ref names in a dock's list, looking inside group rows for a child.
function findItem(list: SceneItem[], ref: SceneItemRef): SceneItem | undefined {
  for (const row of list) {
    if (sameItem(row, ref)) {
      return row;
    }
    const child = row.children?.find((c) => sameItem(c, ref));
    if (child) {
      return child;
    }
  }
  return undefined;
}

// The source-list selection model for ONE source list (a scene's scene items).
// `SourcesDock` (Default canvas, channel-0 path) drives the exported `sourceSelection`
// singleton; each `CanvasDock` builds its own instance for its per-canvas source list.
// The app-level Delete / Ctrl+C / Ctrl+V handlers reach whichever of those the user last
// clicked in through `activeSurface` (activeSurfaceStore.svelte.ts), not this export.
//
// Two members are tracked apart from the set. The FOCUS is what `item` returns, so
// properties and single-item actions follow the last item the user acted on, wherever
// they acted. The PIVOT is the Shift-range origin: a plain or Ctrl click in a dock sets
// it; otherwise it only changes when it leaves the set. Each is a member of the set, or
// null when it is empty.
export class SourceSelection {
  // The scene the selection belongs to. Reconcile clears the set when this changes,
  // since a selection only makes sense within one scene.
  scene = $state<string | null>(null);

  // Selected scene items. Held as objects (not just ids) so `item` and `items` resolve
  // without the owning dock's list; reconcile() refreshes them.
  private _items = $state<SceneItem[]>([]);
  private _focus = $state<SceneItemRef | null>(null);
  private _pivot = $state<SceneItemRef | null>(null);
  // Id lists pushed to the preview whose sceneItem.selected echo has not arrived yet,
  // oldest first. Not reactive: only adoptPreview reads it.
  private _pushes: number[][] = [];

  // The set with the focused member last, the order the docks push to preview.select:
  // native reports the last member back as the focus, so any other order would echo a
  // different one.
  private _ordered = $derived.by(() => {
    const focus = this._focus;
    if (!focus) {
      return this._items;
    }
    return [...this._items.filter((i) => !sameItem(i, focus)), ...this._items.filter((i) => sameItem(i, focus))];
  });

  // The focused member, or null when empty. With exactly one selected it is that item.
  get item(): SceneItem | null {
    const focus = this._focus;
    return focus ? (this._items.find((i) => sameItem(i, focus)) ?? null) : null;
  }

  // The full selection set, focused member last. Batch ops loop this.
  get items(): SceneItem[] {
    return this._ordered;
  }

  get ids(): number[] {
    return this._ordered.map((i) => i.id);
  }

  get size(): number {
    return this._items.length;
  }

  has(ref: SceneItemRef): boolean {
    return this._items.some((i) => sameItem(i, ref));
  }

  // Plain click: replace the selection with just this item, which becomes focus and pivot.
  selectOne(item: SceneItem): void {
    this._items = [item];
    this._focus = refOf(item);
    this._pivot = this._focus;
  }

  // Ctrl/Cmd click: add or remove this item. An added item becomes focus and pivot; a
  // removed one hands either role it held to the fallback `settleRoles` gives.
  toggle(item: SceneItem): void {
    if (this.has(item)) {
      this._items = this._items.filter((i) => !sameItem(i, item));
      this.settleRoles();
    } else {
      this._items = [...this._items, item];
      this._focus = refOf(item);
      this._pivot = this._focus;
    }
  }

  // Shift click: select the pivot→item span (inclusive) over the dock's ordered list and
  // focus the clicked item. The pivot stays put so a later Shift-click spans from the same
  // origin. With no pivot (or one filtered out of the list) it degrades to a plain click.
  range(item: SceneItem, ordered: SceneItem[]): void {
    const pivot = this._pivot;
    const a = pivot ? ordered.findIndex((i) => sameItem(i, pivot)) : -1;
    const b = ordered.findIndex((i) => sameItem(i, item));
    if (b < 0) {
      return;
    }
    if (a < 0) {
      this.selectOne(item);
      return;
    }
    const [lo, hi] = a <= b ? [a, b] : [b, a];
    this._items = ordered.slice(lo, hi + 1);
    this._focus = refOf(item);
  }

  // Replace the whole set at once, focusing `focus`. This is how a selection made in the
  // PREVIEW (Ctrl-click, rubber band, click-through) arrives: the native surface sends
  // its set and its focus in one sceneItem.selected, so the dock adopts it wholesale
  // rather than replaying it as a sequence of clicks. An empty list clears. The pivot is
  // kept whenever it is still a member, so a preview Ctrl-click moves the focus while the
  // next dock Shift-click still spans from the dock's own origin.
  setAll(items: SceneItem[], focus: SceneItemRef | null): void {
    this._items = [...items];
    this._focus = focus;
    this.settleRoles();
  }

  // Push the set to the native preview through `send`, the dock's preview.select call.
  // The push is remembered until its echo arrives, so adoptPreview can tell the echo from
  // a selection made in the preview. A refused call never echoes, so it is forgotten.
  pushToPreview(send: (ids: number[]) => Promise<unknown>): void {
    const ids = this.ids;
    this._pushes.push(ids);
    send(ids).catch(() => {
      const at = this._pushes.indexOf(ids);
      if (at >= 0) {
        this._pushes.splice(at, 1);
      }
    });
  }

  // Adopt a sceneItem.selected payload against the owning dock's list: its whole set,
  // focused on the member the preview reports. Ids the list no longer holds are dropped.
  //
  // The echo of this model's own push is skipped. Local state applied that push before
  // sending it, so adopting the echo would undo whatever changed since: two quick
  // Ctrl-clicks would have the first echo drop the second item and re-pivot. The cost is
  // one race: a preview click native handled before a dock push, but whose event lands
  // after that push was sent, is adopted over it, so until the next selection the dock
  // shows the preview's set while native holds the pushed one, whose echo is then skipped. Pushes queued ahead of the matching
  // one are dropped with it; native echoes pushes in order, so theirs are not coming.
  adoptPreview(selected: Pick<ObsEvents["sceneItem.selected"], "id" | "ids">, list: SceneItem[]): void {
    const ids = selected.ids ?? (selected.id != null ? [selected.id] : []);
    const echoed = this._pushes.findIndex(
      (pushed) => pushed.length === ids.length && pushed.every((id, i) => id === ids[i]),
    );
    if (echoed >= 0) {
      this._pushes.splice(0, echoed + 1);
      return;
    }
    const picked = ids.map((id) => findItem(list, { id })).filter((i): i is SceneItem => i != null);
    this.setAll(picked, selected.id != null ? { id: selected.id } : null);
  }

  clear(): void {
    this._items = [];
    this._focus = null;
    this._pivot = null;
  }

  // Called by the owning dock whenever it reloads its list: refresh the held objects
  // (a rename/visibility toggle produces fresh SceneItems), drop any selected item that
  // vanished, and clear the whole set on a scene change (per-scene selection).
  reconcile(scene: string | null, list: SceneItem[]): void {
    // The docks call this from a $effect. It reads the selection's state AND writes it,
    // so tracking those reads would make the effect depend on state it just wrote and
    // re-fire without end (effect_update_depth_exceeded). untrack the body: the effect
    // then tracks only the scene/list it was called with, while the writes still notify
    // the selection's own readers.
    untrack(() => {
      if (scene !== this.scene) {
        this.scene = scene;
        this.clear();
        // An echo for the old scene never reaches adoptPreview (the docks filter by scene),
        // so its entry would otherwise sit in the queue and swallow a later preview
        // selection that happens to hold the same ids.
        this._pushes = [];
        return;
      }
      this._items = this._items.map((sel) => findItem(list, sel)).filter((i): i is SceneItem => i != null);
      this.settleRoles();
    });
  }

  // Keep focus and pivot members after the set changed under them: a focus that left falls
  // to the last member, and a pivot that left falls to the focus.
  private settleRoles(): void {
    const member = (ref: SceneItemRef | null) => ref != null && this.has(ref);
    if (!member(this._focus)) {
      this._focus = refOf(this._items.at(-1));
    }
    if (!member(this._pivot)) {
      this._pivot = this._focus;
    }
  }
}

export const sourceSelection = new SourceSelection();
