import { obs } from "$lib/api/bridge";
import type { DockviewApi } from "dockview-core";

// Persists the Dockview layout (toJSON) to layout.json via the bridge and
// restores it on boot. restore() returns false when nothing valid is saved (or
// fromJSON throws on a malformed blob), so App falls back to buildDefaultLayout.
// Save failures are non-fatal -- the in-memory layout is unaffected.
export const layoutStore = {
  async save(api: DockviewApi): Promise<void> {
    try {
      const layout = JSON.stringify(api.toJSON());
      await obs.call("layout.save", { layout });
    } catch {
      // non-fatal: layout still live in-memory
    }
  },

  async restore(api: DockviewApi): Promise<boolean> {
    try {
      const saved = await obs.call("layout.load");
      if (!saved || !saved.layout) {
        return false;
      }
      api.fromJSON(JSON.parse(saved.layout));
      return true;
    } catch {
      // absent / malformed -> caller builds the default layout
      return false;
    }
  },
};
