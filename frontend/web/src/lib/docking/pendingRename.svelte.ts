import { untrack } from "svelte";
import type { SceneItemRef } from "$lib/api/bridge";
import { RenameWait, type RenameDock } from "$lib/docking/renameWait";
import { anyEscOwner } from "$lib/utils/escStack";

export interface PendingRenameDock extends RenameDock {
  scene(): string | null;
}

// A sources dock's rename wait (RenameWait), kept by the dock's own effects: the editor opens
// once the awaited row is shown, and the wait is dropped by a scene change, or by Escape
// unless a menu or dialog owns the key. The dock drops it too when a rename starts any other
// way, and serves its app-level F2 requests through `serve`.
//
// Construct it during the dock's initialisation, before the effect that calls `serve`: the
// scene-change effect clears the wait on its first run, and effects first run in the order
// they were created.
export class PendingRename extends RenameWait {
  constructor(dock: PendingRenameDock) {
    // Raw: the ref is replaced, never mutated, and a deep proxy would only add cost.
    let ref = $state.raw<SceneItemRef | null>(null);
    super(
      { get: () => ref, set: (next) => (ref = next) },
      {
        ...dock,
        // Untracked, so the calling effect does not come to depend on what the dock reads
        // while it opens the editor or sends the expand.
        begin: (item) => untrack(() => dock.begin(item)),
        expand: (group) => untrack(() => dock.expand(group)),
      },
    );

    $effect(() => this.openWhenShown());

    $effect(() => {
      if (!this.waiting) {
        return;
      }
      const onKey = (e: KeyboardEvent) => {
        if (e.key === "Escape" && !anyEscOwner()) {
          this.clear();
        }
      };
      window.addEventListener("keydown", onKey);
      return () => window.removeEventListener("keydown", onKey);
    });

    $effect(() => {
      void dock.scene();
      this.clear();
    });
  }
}
