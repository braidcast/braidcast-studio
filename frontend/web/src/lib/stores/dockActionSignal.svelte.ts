import { tick } from "svelte";

// One-shot bridge for the app-level shortcuts. `onKeydown` lives in App.svelte but the
// inline-rename editors and the scene-removal actions live inside the docks, so a shortcut
// pokes this signal with an action and the owning dock's $effect calls its EXISTING
// beginRename / remove for the matching row. No rename UI, no removal call, and no
// confirmation lives here -- only the request hand-off, so each dock keeps its own error
// surface and the keyboard becomes a second caller of the dock action rather than a second
// implementation of it.
//
// Adding a shortcut that has to reach into a dock is one more case in this union plus one
// more branch in the owning dock's effect.
export type DockAction =
  | { kind: "renameSource"; id: number }
  | { kind: "renameScene"; name: string }
  | { kind: "removeScene"; name: string };

interface DockActionRequest {
  // Addresses the request (null = Default): scene-item ids are per-scene counters and scene
  // names are per-canvas, so a CanvasDock row can name a different row in the Default
  // dock's list, and every consumer would otherwise match on the collision and act on the
  // wrong one.
  canvas: string | null;
  action: DockAction;
  seq: number;
}

class DockActionSignal {
  #pending = $state<DockActionRequest | null>(null);
  #seq = 0;

  get pending(): DockActionRequest | null {
    return this.#pending;
  }

  /** Poke the owning dock; resolves to whether one actually took the request.
   *
   *  The request is consumed exactly once, globally: the matching dock nulls it before it
   *  acts, so it cannot be replayed. That has to be state on the signal rather than a
   *  per-component "already handled" seq — the component is not what persists, so a dock
   *  closed and reopened would re-run a request answered long ago, and for `removeScene`
   *  that means acting on a confirm the user gave for a different scene.
   *
   *  Anything still pending once the flush this schedules has run had no listening dock (it
   *  is closed, or the row it names is gone), so it is dropped rather than left to fire on
   *  some later mount — and the caller is told, since a destructive action that silently
   *  does nothing is worse than one that fails loudly. */
  async request(canvas: string | null, action: DockAction): Promise<boolean> {
    const seq = ++this.#seq;
    this.#pending = { canvas, action, seq };
    await tick();
    if (this.#pending?.seq === seq) {
      this.#pending = null;
      return false;
    }
    return true;
  }

  /** Taken by the matching dock immediately before it acts, so a re-entrant run of its own
   *  effect (this write is a read of that effect) sees nothing left to do. Returns false when
   *  the request is already gone — superseded, or taken by an earlier run — and the caller
   *  must not act on it. */
  consume(seq: number): boolean {
    if (this.#pending?.seq !== seq) {
      return false;
    }
    this.#pending = null;
    return true;
  }
}

export const dockAction = new DockActionSignal();
