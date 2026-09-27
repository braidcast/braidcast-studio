import { obs } from "$lib/api/bridge";
import type { DockviewApi, SerializedDockview } from "dockview-core";

// Persists the Dockview layout (toJSON) to layout.json via the bridge and restores it
// on boot. Save failures are non-fatal -- the in-memory layout is unaffected.
//
// restore() tells "nothing saved" apart from "saved but would not apply": the caller
// builds the default layout for both, but a failed one must not be overwritten, so
// it is set aside first (layout.quarantine copies it to layout.failed-<time>.json).
// A file that is there but could not be read counts as failed, not as nothing saved.
export type LayoutRestore = "restored" | "none" | "failed";

export interface LayoutPersister {
  /** A user gesture on the dock chrome (a sash, a tab strip). Arms unless gestures are
   * not trusted yet -- see allowGestures. */
  armFromGesture(): void;
  /** An explicit layout action (open/close a dock, reset, detach, redock). Always arms. */
  armExplicit(): void;
  /** Lets gestures arm. Withheld until the saved layout is restored, or is safely copied
   * aside; while it is withheld only an explicit action can overwrite layout.json. */
  allowGestures(): void;
  /** Takes the layout as it now stands as the one on disk: the baseline a save must
   * differ from. */
  settle(): void;
  /** Dockview reported a layout change. */
  changed(): void;
  dispose(): void;
}

// Only the arrangement decides whether a save is due. Which group has focus changes on
// every click into a dock; written each time, it would rotate layout.json into .bak on
// every focus change, and a focus is nothing a restore needs to get right.
const arrangement = (layout: SerializedDockview): string => JSON.stringify({ ...layout, activeGroup: undefined });

// Writes the layout only once the user has armed it. Dockview fires onDidLayoutChange
// for every change, including fromJSON on a restore, the fallback default, the
// reconcilers' dock adds and a group merely taking focus, and each layout.save rotates
// layout.json into layout.json.bak -- so saving a fallback default would push the
// user's saved layout out to .bak, and the next save out of existence. Changes after
// arming are coalesced (one per addPanel while a layout is assembled) into a single
// trailing save, skipped when the arrangement is what was last saved or settled.
export function createLayoutPersister(
  snapshot: () => SerializedDockview | null,
  write: (layout: string) => void,
  delayMs = 250,
): LayoutPersister {
  let armed = false;
  let gestures = false;
  let last: string | null = null;
  let timer: ReturnType<typeof setTimeout> | undefined;
  const flush = (): void => {
    const layout = snapshot();
    if (!layout) return;
    const key = arrangement(layout);
    if (key === last) return;
    last = key;
    write(JSON.stringify(layout));
  };
  return {
    armFromGesture(): void {
      if (gestures) armed = true;
    },
    armExplicit(): void {
      armed = true;
    },
    allowGestures(): void {
      gestures = true;
    },
    settle(): void {
      const layout = snapshot();
      last = layout ? arrangement(layout) : null;
    },
    changed(): void {
      if (!armed) return;
      clearTimeout(timer);
      timer = setTimeout(flush, delayMs);
    },
    dispose(): void {
      clearTimeout(timer);
    },
  };
}

export const layoutStore = {
  async write(layout: string): Promise<void> {
    try {
      await obs.call("layout.save", { layout });
    } catch {
      // non-fatal: layout still live in-memory
    }
  },

  async restore(api: DockviewApi): Promise<LayoutRestore> {
    let saved: string;
    let present: boolean;
    try {
      // A host without the handler resolves null: nothing saved.
      const loaded = await obs.call("layout.load");
      saved = loaded?.layout ?? "";
      present = loaded?.present === true;
    } catch (e) {
      console.log("OBSSHELL: layout.load failed: " + (e as Error).message);
      return "failed";
    }
    if (!saved) {
      if (present) {
        console.log("OBSSHELL: saved layout is on disk but could not be read");
        return "failed";
      }
      return "none";
    }
    try {
      api.fromJSON(JSON.parse(saved));
      return "restored";
    } catch (e) {
      console.log("OBSSHELL: saved layout did not restore: " + (e as Error).message);
      return "failed";
    }
  },

  // Keep a copy of the saved layout that failed to restore; returns the copy's file
  // name, or "" when there was nothing to copy or the copy failed.
  async quarantine(): Promise<string> {
    try {
      return (await obs.call("layout.quarantine"))?.file ?? "";
    } catch (e) {
      console.log("OBSSHELL: layout.quarantine failed: " + (e as Error).message);
      return "";
    }
  },
};
