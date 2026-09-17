import type { SceneItem, SceneItemRef } from "$lib/api/bridge";
import { revealRow, type TreeRow } from "$lib/docking/sourceTree";
import { findItem, sameItem, toRef } from "$lib/utils/sceneItemRef";

// What a sources dock lends its rename wait: its list, the rows its tree shows, and the two
// calls that open the inline editor and expand a group.
export interface RenameDock {
  items(): readonly SceneItem[];
  rows(): readonly TreeRow[];
  filtering(): boolean;
  begin(item: SceneItem): void;
  // Resolves to whether the host took the expand.
  expand(group: SceneItem): Promise<boolean>;
}

// Where the awaited ref is held. PendingRename backs it with reactive state; a test can use
// a plain variable.
export interface RefCell {
  get(): SceneItemRef | null;
  set(ref: SceneItemRef | null): void;
}

// A rename a sources dock has accepted for a row it cannot show yet, because the row's group
// is still collapsed. The editor only mounts with its row, so the rename waits here for the
// row to appear in the dock's rows, whichever reload brings it, rather than for one
// particular reload that a newer one can supersede.
export class RenameWait {
  readonly #cell: RefCell;
  readonly #dock: RenameDock;
  // Bumped by every request and every clear. A refused expand drops the wait only while its
  // own request is still the latest, which a counter tells apart without comparing refs.
  #seq = 0;

  constructor(cell: RefCell, dock: RenameDock) {
    this.#cell = cell;
    this.#dock = dock;
  }

  get waiting(): boolean {
    return this.#cell.get() !== null;
  }

  // Serve an app-level rename request for `ref`. Any earlier wait is dropped first. A row the
  // dock does not list, or one a filter hides, is left unserved and the request unconsumed,
  // so its sender can report it. Otherwise the request is consumed, then the editor opens on
  // a shown row, or waits while the row's group is expanded.
  //
  // Reads only the dock's list, rows and filter, never the wait itself, so an effect that
  // calls this re-runs when those load but not when the wait changes.
  serve(ref: SceneItemRef, consume: () => boolean): void {
    this.clear();
    const items = this.#dock.items();
    const item = findItem(items, ref);
    if (!item) {
      return;
    }
    const reveal = revealRow(items, this.#dock.rows(), item, this.#dock.filtering());
    if (reveal.kind === "hidden" || !consume()) {
      return;
    }
    if (reveal.kind === "shown") {
      this.#dock.begin(item);
      return;
    }
    const mine = ++this.#seq;
    this.#cell.set(toRef(item));
    void this.#dock.expand(reveal.group).then((took) => {
      if (!took && mine === this.#seq) {
        this.#cell.set(null);
      }
    });
  }

  // Writes without reading, so an effect that calls this does not come to depend on the wait
  // and clear each new one the moment it starts.
  clear(): void {
    this.#seq++;
    this.#cell.set(null);
  }

  // Open the editor once the awaited row is shown.
  openWhenShown(): void {
    const ref = this.#cell.get();
    const row = ref ? this.#dock.rows().find((r) => sameItem(r.item, ref)) : undefined;
    if (row) {
      this.clear();
      this.#dock.begin(row.item);
    }
  }
}
