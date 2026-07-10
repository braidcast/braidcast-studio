// Undo/redo state mirror. The C++ undo stack records transform/visibility/lock/
// scale-filter/reorder/rename/add-source/remove-source mutations; this singleton
// reflects its can-undo/can-redo state (+ the next action's name for tooltips) and
// drives both the global Ctrl+Z/Ctrl+Y shortcuts and the toolbar buttons. The
// engine pushes `undo.changed` after every stack mutation; start() also seeds the
// initial state via undo.state.

import { obs } from "./bridge";
import { EV } from "./eventNames";
import type { UndoState } from "./bridge";

class UndoStore {
  canUndo = $state(false);
  canRedo = $state(false);
  undoName = $state("");
  redoName = $state("");

  #started = false;

  // Idempotent: the app root starts it; an earlier-mounting host start() is a no-op.
  start(): void {
    if (this.#started) {
      return;
    }
    this.#started = true;
    obs.on(EV.undoChanged, (s) => this.apply(s));
    void this.refresh();
  }

  async refresh(): Promise<void> {
    try {
      this.apply(await obs.call("undo.state"));
    } catch {
      /* leave defaults */
    }
  }

  apply(s: UndoState): void {
    this.canUndo = s.canUndo;
    this.canRedo = s.canRedo;
    this.undoName = s.undoName ?? "";
    this.redoName = s.redoName ?? "";
  }

  undo(): void {
    if (this.canUndo) {
      void obs.call("undo.undo").catch(() => {});
    }
  }

  redo(): void {
    if (this.canRedo) {
      void obs.call("undo.redo").catch(() => {});
    }
  }
}

export const undoStore = new UndoStore();
