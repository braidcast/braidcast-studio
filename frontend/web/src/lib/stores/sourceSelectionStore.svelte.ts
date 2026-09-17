import { untrack } from "svelte";
import type { ObsEvents, SceneItem, SceneItemRef } from "$lib/api/bridge";
import { findItem, normalizeRefs, sameItem, sameRefList, toRef, withoutChildrenOf } from "$lib/utils/sceneItemRef";

function refOf(item: SceneItem | undefined): SceneItemRef | null {
  return item ? toRef(item) : null;
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
  // Ref lists pushed to the preview whose sceneItem.selected echo has not arrived yet,
  // oldest first, each normalized the way the host echoes it. Not reactive: only
  // adoptPreview reads it.
  private _pushes: SceneItemRef[][] = [];

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

  // The set as bridge refs, focused member last. Anything addressing the members reads
  // these rather than bare ids, which cannot tell a group's child from a top-level item.
  get refs(): SceneItemRef[] {
    return this._ordered.map(toRef);
  }

  // `refs` without any child whose own group is also selected, for removal: removing the
  // group already takes its children, so removing such a child as well would fail after.
  get removalRefs(): SceneItemRef[] {
    const groups = new Set(this._items.flatMap((i) => (i.children?.length ? [i.children[0].group] : [])));
    return this.refs.filter((r) => r.group === null || !groups.has(r.group));
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

  // A group row is collapsing, hiding its children, so any selected child leaves the set and
  // the app-level shortcuts cannot act on a row the collapse just hid. The group is never
  // added in their place: that would widen the selection, so a Delete meant for one child
  // would remove the whole group. Focus and pivot settle as for any removal, and the set may
  // end up empty. Returns null when the set did not change. Otherwise the caller pushes the
  // new set to the preview, and gets back a restore for when the host refuses the collapse:
  // provided the set is still the one the collapse left, it puts back the members `list`
  // still holds, with focus and pivot as they were, and reports whether it did.
  collapseGroup(group: SceneItem): ((list: readonly SceneItem[]) => boolean) | null {
    const rest = withoutChildrenOf(this._items, group);
    if (rest === null) {
      return null;
    }
    const before = { items: this._items, focus: this._focus, pivot: this._pivot };
    this._items = rest;
    this.settleRoles();
    const left = this.refs;
    return (list) => {
      if (!sameRefList(this.refs, left)) {
        return false;
      }
      this._items = before.items.map((i) => findItem(list, i)).filter((i): i is SceneItem => i != null);
      this._focus = before.focus;
      this._pivot = before.pivot;
      this.settleRoles();
      return true;
    };
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
  pushToPreview(send: (refs: SceneItemRef[]) => Promise<unknown>): void {
    const refs = normalizeRefs(this.refs);
    this._pushes.push(refs);
    send(refs).catch(() => {
      const at = this._pushes.indexOf(refs);
      if (at >= 0) {
        this._pushes.splice(at, 1);
      }
    });
  }

  // Adopt a sceneItem.selected payload against the owning dock's list: its whole set,
  // focused on the member the preview reports. Refs the list no longer holds are dropped. A
  // payload without `refs` names top-level items by id.
  //
  // The echo of this model's own push is skipped. Local state applied that push before
  // sending it, so adopting the echo would undo whatever changed since: two quick
  // Ctrl-clicks would have the first echo drop the second item and re-pivot. The cost is
  // one race: a preview click native handled before a dock push, but whose event lands
  // after that push was sent, is adopted over it, so until the next selection the dock
  // shows the preview's set while native holds the pushed one, whose echo is then skipped. Pushes queued ahead of the matching
  // one are dropped with it; native echoes pushes in order, so theirs are not coming.
  adoptPreview(
    selected: Pick<ObsEvents["sceneItem.selected"], "id" | "ids"> &
      Partial<Pick<ObsEvents["sceneItem.selected"], "refs" | "group">>,
    list: SceneItem[],
  ): void {
    const refs =
      selected.refs?.map(toRef) ??
      (selected.ids ?? (selected.id != null ? [selected.id] : [])).map((id) => ({ id, group: null }));
    const echoed = this._pushes.findIndex((pushed) => sameRefList(pushed, refs));
    if (echoed >= 0) {
      this._pushes.splice(0, echoed + 1);
      return;
    }
    const picked = refs.map((ref) => findItem(list, ref)).filter((i): i is SceneItem => i != null);
    this.setAll(picked, selected.id != null ? { id: selected.id, group: selected.group ?? null } : null);
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
